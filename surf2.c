#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>
#include <bsd/string.h>
#include <libgen.h>
#include <X11/Xatom.h>
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <webkit2/webkit2.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <glib/gprintf.h>

#include "arg.h"

#define LENGTH(x)	(sizeof x / sizeof x[0])
#define CLEANMASK(mask)	(mask & (MODKEY|GDK_SHIFT_MASK))

enum _atom { ATOMFIND, ATOMGO, ATOMURI, ATOMLAST };

union _arg {
	gboolean b;
	gint i;
	const void *v;
};

struct _client {
	GtkWidget *win;
	Window xwin;
	WebKitWebView *view;
	WebKitFindController *finder;
	WebKitWebInspector *inspector;
	const gchar *uri;
	gchar *title;
	gchar *hoveruri;
	gchar *hovertitle;
	gchar *hovercontent;
	gint cookiepolicy;
	gint progress;
	gboolean committed;
	gboolean fullscreen;
	gboolean insecure;
	gboolean inspecting;
	gboolean ssl;
	gboolean sslfailed;
	gboolean styled;
	struct _client *next;
};

typedef struct _key {
	guint mod;
	guint keyval;
	void (*func)(struct _client *c, const union _arg *arg);
	const union _arg arg;
} Key;

char *argv0;
static Display *dpy;
static Atom atoms[ATOMLAST];
static struct _client *clients;
static Window embed;
static bool showwinid;
static bool usingproxy;
static char winid[21];
static char pagestats[3];
static char togglestats[8];
static gint cookiepolicy;

static void addaccelgroup(struct _client *);
static char *buildpath(const char *);
static void cleanup(void);
static void clipboard(struct _client *, const union _arg *);
static gchar *copystr(char **, const char *);
static WebKitWebView *createwindow(WebKitWebView *, struct _client *);
static gboolean decidepolicy(WebKitWebView *, WebKitPolicyDecision *,
    WebKitPolicyDecisionType, struct _client *);
static void destroyclient(struct _client *);
static void destroywin(GtkWidget *, struct _client *);
static void die(const char *, ...);
static void find(struct _client *, const union _arg *);
static const char *getatom(struct _client *, enum _atom);
static WebKitCookieAcceptPolicy getcookiepolicy(void);
static void getpagestats(struct _client *);
static void gettogglestats(struct _client *);
static gboolean initdownload(struct _client *, const union _arg *);
static void insecurecontent(WebKitWebView *, WebKitInsecureContentEvent,
    struct _client *);
static void inspector(struct _client *, const union _arg *);
static gboolean keypress(GtkAccelGroup *, GObject *, guint, GdkModifierType,
    struct _client *);
static void loadchanged(WebKitWebView *, WebKitLoadEvent, struct _client *);
static void loadprogressed(WebKitWebView *, GParamSpec *, struct _client *);
static void loaduri(struct _client *, const union _arg *);
static void mousetargetchanged(WebKitWebView *, WebKitHitTestResult *, guint,
    struct _client *);
static void navigate(struct _client *, const union _arg *);
static struct _client *newclient(void);
static void newwindow(struct _client *, const union _arg *, bool);
static void pasteuri(GtkClipboard *, const char *, gpointer);
static gboolean permissionrequest(WebKitWebView *, WebKitPermissionRequest *,
    struct _client *);
static void print(struct _client *, const union _arg *);
static GdkFilterReturn processx(GdkXEvent *, GdkEvent *, gpointer);
static void reload(struct _client *, const union _arg*);
static void resourceloadstarted(WebKitWebView *, WebKitWebResource *,
    WebKitURIRequest *, struct _client *);
static void runjavascript(WebKitWebView *, const char *, ...);
static void scroll_v(struct _client *, const union _arg *);
static void scroll_h(struct _client *, const union _arg *);
static void setatom(struct _client *, enum _atom, const char *);
static char setcookiepolicy(const WebKitCookieAcceptPolicy);
static void setup(int *, char **[]);
static void show(WebKitWebView *, struct _client *);
static void sigchld(int);
static void spawn(struct _client *, const union _arg *);
static void stop(struct _client *, const union _arg *);
static void titlechanged(WebKitWebView *, GParamSpec *, struct _client *);
static void toggle(struct _client *, const union _arg *);
static void togglecookiepolicy(struct _client *, const union _arg *);
static void togglefullscreen(struct _client *, const union _arg *);
static void togglegeolocation(struct _client *, const union _arg *);
static void togglestyle(struct _client *, const union _arg *);
static void updatetitle(struct _client *);
static void updatewinid(struct _client *);
static void usage(void);
static void zoom(struct _client *, const union _arg *);

#include "config.h"

static void
addaccelgroup(struct _client *c) {
	int i;
	GtkAccelGroup *group;
	GClosure *closure;

	group = gtk_accel_group_new();

	for (i = 0; i < LENGTH(keys); i++) {
		closure = g_cclosure_new(G_CALLBACK(keypress), c, NULL);
		gtk_accel_group_connect(group, keys[i].keyval, keys[i].mod,
		    0, closure);
	}
	gtk_window_add_accel_group(GTK_WINDOW(c->win), group);
}

static char *
buildpath(const char *path) {
	char *apath, *p;
	FILE *f;

	/* creating directory */
	if (path[0] == '/')
		apath = g_strdup(path);
	else if (path[0] == '~') {
		if (path[1] == '/')
			apath = g_strconcat(g_get_home_dir(), &path[1], NULL);
		else
			apath = g_strconcat(g_get_home_dir(), "/",
			    &path[1], NULL);
	} else
		apath = g_strconcat(g_get_current_dir(), "/", path, NULL);

	p = strchr(apath, '/');
	if (p != NULL) {
		*p = '\0';
		g_mkdir_with_parents(apath, 0700);
		g_chmod(apath, 0700); /* in case it existed */
		*p = '/';
	}

	/* creating file (gives error when apath ends with "/") */
	f = fopen(apath, "a");
	if (f != NULL) {
		g_chmod(apath, 0600); /* always */
		fclose(f);
	}

	return apath;
}

static void
cleanup(void) {
	while (clients)
		destroyclient(clients);
}

static void
clipboard(struct _client *c, const union _arg *arg) {
	gboolean paste;
	GtkClipboard *clipboard;

	paste = arg->b;
	clipboard = gtk_clipboard_get(GDK_SELECTION_PRIMARY);

	if (paste) {
		gtk_clipboard_request_text(clipboard, pasteuri, c);
	} else {
		gtk_clipboard_set_text(clipboard, c->hoveruri
			? c->hoveruri : c->uri, -1);
	}
}

static gchar *
copystr(char **dst, const char *src) {
	gchar *tmp;

	tmp = g_strdup(src);
	if (dst && *dst) {
		g_free(*dst);
		*dst = tmp;
	}

	return tmp;
}

static WebKitWebView *
createwindow(WebKitWebView *v, struct _client *c) {
	struct _client *n;

	n = newclient();

	return n->view;
}

static gboolean
decidepolicy(WebKitWebView *v, WebKitPolicyDecision *d,
    WebKitPolicyDecisionType dt, struct _client *c) {
	WebKitNavigationAction *na;
	WebKitResponsePolicyDecision *rd;
	guint button, mods;
	union _arg arg;

	switch (dt) {
	case WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION:
		na = webkit_navigation_policy_decision_get_navigation_action(
		    WEBKIT_NAVIGATION_POLICY_DECISION(d));

		if (webkit_navigation_action_is_user_gesture(na)
		    && webkit_navigation_action_get_navigation_type(na)
		    == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
			button = webkit_navigation_action_get_mouse_button(na);
			mods = webkit_navigation_action_get_modifiers(na);
			if (button == 2 || (button == 1 && mods & CLEANMASK(MODKEY))) {
				arg.v = webkit_uri_request_get_uri(
				    webkit_navigation_action_get_request(na));
				newwindow(c, &arg, false);
				webkit_policy_decision_ignore(d);
			}
		}
		break;
	case WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION:
		na = webkit_navigation_policy_decision_get_navigation_action(
		    WEBKIT_NAVIGATION_POLICY_DECISION(d));

		if (webkit_navigation_action_is_user_gesture(na)
		    && webkit_navigation_action_get_navigation_type(na)
		    == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED) {
			arg.v = webkit_uri_request_get_uri(
			    webkit_navigation_action_get_request(na));
			newwindow(c, &arg, false);
		}
		webkit_policy_decision_ignore(d);
		break;
	case WEBKIT_POLICY_DECISION_TYPE_RESPONSE:
		rd = WEBKIT_RESPONSE_POLICY_DECISION(d);
		if (!webkit_response_policy_decision_is_mime_type_supported(rd)) {
			arg.v = webkit_uri_request_get_uri(
			    webkit_response_policy_decision_get_request(rd));
			initdownload(c, &arg);
			webkit_policy_decision_ignore(d);
		}
		break;
	default:
		return FALSE;
	}

	return TRUE;
}

static void
destroyclient(struct _client *c) {
	struct _client *p;

	webkit_web_view_stop_loading(c->view);
	gtk_widget_destroy(GTK_WIDGET(c->view));
	gtk_widget_destroy(c->win);

	for (p = clients; p && p->next != c; p = p->next)
		;
	if (p)
		p->next = c->next;
	else
		clients = c->next;

	free(c);

	if (clients == NULL)
		gtk_main_quit();
}

static void
destroywin(GtkWidget *w, struct _client *c) {
	destroyclient(c);
}

static void
die(const char *errstr, ...) {
	va_list ap;

	va_start(ap, errstr);
	vfprintf(stderr, errstr, ap);
	va_end(ap);

	exit(EXIT_FAILURE);
}

static void
find(struct _client *c, const union _arg *arg) {
	const char *s;

	s = getatom(c, ATOMFIND);
	gboolean forward = arg->b;

	if (g_strcmp0(webkit_find_controller_get_search_text(c->finder), s))
		webkit_find_controller_search(c->finder, s, findopts, G_MAXUINT);
	else {
		if (forward)
			webkit_find_controller_search_next(c->finder);
		else
			webkit_find_controller_search_previous(c->finder);
	}
}

static const char *
getatom(struct _client *c, enum _atom a) {
	static char buf[BUFSIZ];
	Atom adummy;
	int idummy;
	unsigned long ldummy;
	unsigned char *p = NULL;

	XGetWindowProperty(dpy, c->xwin, atoms[a], 0L, BUFSIZ,
	    False, XA_STRING, &adummy, &idummy, &ldummy, &ldummy, &p);
	if (p)
		strlcpy(buf, (char *)p, LENGTH(buf));
	else
		buf[0] = '\0';

	XFree(p);

	return buf;
}

static WebKitCookieAcceptPolicy
getcookiepolicy(void) {
	WebKitCookieAcceptPolicy policy;

	switch(cookiepolicies[cookiepolicy]) {
	case 'a':
		policy = WEBKIT_COOKIE_POLICY_ACCEPT_NEVER;
		break;
	case '@':
		policy = WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY;
		break;
	case 'A':
	default:
		policy = WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS;
		break;
	}

	return policy;
}

static void
getpagestats(struct _client *c) {
	if (c->ssl)
		pagestats[0] = c->sslfailed ? 'U' : c->insecure ? 'I' : 'T';
	else
		pagestats[0] = '-';
	pagestats[1] = usingproxy ? 'P' : '-';
	pagestats[2] = '\0';
}

static void
gettogglestats(struct _client *c) {
	gboolean enabled;
	int p;
	WebKitSettings *settings;

	settings = webkit_web_view_get_settings(c->view);
	p = 0;

	togglestats[p++] = setcookiepolicy(getcookiepolicy());

	enabled = webkit_settings_get_enable_caret_browsing(settings);
	togglestats[p++] = enabled ? 'C' : 'c';

	togglestats[p++] = allowgeolocation ? 'G': 'g';

	enabled = webkit_settings_get_auto_load_images(settings);
	togglestats[p++] = enabled ? 'I' : 'i';

	enabled = webkit_settings_get_enable_javascript(settings);
	togglestats[p++] = enabled ? 'S' : 's';

	enabled = webkit_settings_get_enable_plugins(settings);
	togglestats[p++] = enabled ? 'V' : 'v';

	togglestats[p++] = c->styled ? 'M': 'm';

	togglestats[p] = '\0';
}

static gboolean
initdownload(struct _client *c, const union _arg *a) {
	union _arg arg;

	arg = (union _arg)DOWNLOAD((char *)a->v, (char *)c->uri);

	updatewinid(c);
	spawn(c, &arg);

	return FALSE;
}

static void
insecurecontent(WebKitWebView *v, WebKitInsecureContentEvent e,
    struct _client *c) {
	c->insecure = e;
	updatetitle(c);
}

static void
inspector(struct _client *c, const union _arg *arg) {
	if (c->inspecting) {
		c->inspecting = FALSE;
		webkit_web_inspector_close(WEBKIT_WEB_INSPECTOR(c->inspector));
	} else {
		c->inspecting = TRUE;
		webkit_web_inspector_show(WEBKIT_WEB_INSPECTOR(c->inspector));
	}
}

static gboolean
keypress(GtkAccelGroup *g, GObject *o, guint key, GdkModifierType mod,
    struct _client *c) {
	guint i;
	gboolean processed;

	processed = FALSE;
	mod = CLEANMASK(mod);
	key = gdk_keyval_to_lower(key);

	for (i = 0; i < LENGTH(keys); i++)
		if (key == keys[i].keyval
		    && mod == keys[i].mod
		    && keys[i].func) {
			updatewinid(c);
			keys[i].func(c, &(keys[i].arg));
			processed = TRUE;
		}

	return processed;
}

static void
loadchanged(WebKitWebView *v, WebKitLoadEvent e, struct _client *c) {
	GTlsCertificateFlags tlsflags;
	switch (e) {
	case WEBKIT_LOAD_STARTED:
		c->progress = 0;
		c->committed = FALSE;
		c->ssl = FALSE;
		c->sslfailed = FALSE;
		c->insecure = FALSE;
		break;
	case WEBKIT_LOAD_REDIRECTED:
		break;
	case WEBKIT_LOAD_COMMITTED:
		c->committed = TRUE;
		if (webkit_web_view_get_tls_info(c->view, NULL, &tlsflags)) {
			c->ssl = TRUE;
			c->sslfailed = tlsflags;
		}
		c->uri = webkit_web_view_get_uri(c->view);
		setatom(c, ATOMURI, c->uri);
		break;
	case WEBKIT_LOAD_FINISHED:
		updatetitle(c);
		break;
	}
}

static void
loadprogressed(WebKitWebView *v, GParamSpec *s, struct _client *c) {
	c->progress = webkit_web_view_get_estimated_load_progress(c->view) * 100;
	updatetitle(c);
}

static void
loaduri(struct _client *c, const union _arg *arg) {
	gchar *u, *rp;
	const char *uri;
	union _arg a;
	struct stat st;

	uri = (char *)arg->v;
	a.b = FALSE;

	if (strcmp(uri, "") == 0)
		return;

	/* In case it's a file path. */
	if (stat(uri, &st) == 0) {
		rp = realpath(uri, NULL);
		u = g_strdup_printf("file://%s", rp);
		free(rp);
	} else
		u = g_strrstr(uri, "://") ? g_strdup(uri)
		    : g_strdup_printf("http://%s", uri);

	setatom(c, ATOMURI, u);

	/* prevents endless loop */
	if (c->uri && strcmp(u, c->uri) == 0)
		reload(c, &a);
	else {
		webkit_web_view_load_uri(c->view, u);
	}
	g_free(u);
}

static void
mousetargetchanged(WebKitWebView *v, WebKitHitTestResult *h, guint mods,
    struct _client *c) {
	WebKitHitTestResultContext hc;

	hc = webkit_hit_test_result_get_context(h);

	if (hc & WEBKIT_HIT_TEST_RESULT_CONTEXT_LINK) {
		c->hoveruri = copystr(&c->hoveruri,
		    webkit_hit_test_result_get_link_uri(h));
		c->hovertitle = copystr(&c->hovertitle,
		    webkit_hit_test_result_get_link_title(h));
		if (c->hovertitle == NULL)
		    c->hovertitle = copystr(&c->hovertitle,
			    webkit_hit_test_result_get_link_label(h));
	} else {
		c->hoveruri = copystr(&c->hoveruri, NULL);
		c->hovertitle = copystr(&c->hovertitle, NULL);
	}

	if (hc & WEBKIT_HIT_TEST_RESULT_CONTEXT_IMAGE)
		c->hovercontent = copystr(&c->hovercontent,
		    webkit_hit_test_result_get_image_uri(h));

	if (hc & WEBKIT_HIT_TEST_RESULT_CONTEXT_MEDIA)
		c->hovercontent = copystr(&c->hovercontent,
		    webkit_hit_test_result_get_media_uri(h));

	if (!(hc & (WEBKIT_HIT_TEST_RESULT_CONTEXT_IMAGE |
		    WEBKIT_HIT_TEST_RESULT_CONTEXT_MEDIA))) {
		c->hovercontent = copystr(&c->hovercontent, NULL);
	}

	updatetitle(c);
}

static void
navigate(struct _client *c, const union _arg *arg) {
	int steps;

	steps = arg->i;

	if (steps < 0)
		webkit_web_view_go_back(c->view);
	else if (steps > 0)
		webkit_web_view_go_forward(c->view);
}

static struct _client *
newclient(void) {
	struct _client *c;
	char *ua;
	WebKitSettings *settings;
	union _arg arg;

	c = calloc(1, sizeof(struct _client));
	if (c == NULL)
		die("newclient(): cannot malloc.\n");

	c->uri = NULL;
	c->title = NULL;
	c->hoveruri = NULL;
	c->hovertitle = NULL;
	c->hovercontent = NULL;
	c->progress = 100;
	c->ssl = FALSE;
	c->sslfailed = FALSE;
	c->insecure = FALSE;
	c->inspecting = FALSE;
	c->styled = FALSE;

	if (embed)
		c->win = gtk_plug_new(embed);
	else
		c->win = gtk_window_new(GTK_WINDOW_TOPLEVEL);

	gtk_window_set_wmclass(GTK_WINDOW(c->win), "surf2", "Surf");
	gtk_window_set_role(GTK_WINDOW(c->win), "Surf");
	gtk_window_set_default_size(GTK_WINDOW(c->win), 800, 600);

	g_signal_connect(c->win,
	    "destroy",
	    G_CALLBACK(destroywin), c);

	gtk_widget_show(c->win);

	c->xwin = GDK_WINDOW_XID(gtk_widget_get_window(GTK_WIDGET(c->win)));

	gdk_window_set_events(gtk_widget_get_window(GTK_WIDGET(c->win)), GDK_ALL_EVENTS_MASK);
	gdk_window_add_filter(gtk_widget_get_window(GTK_WIDGET(c->win)), processx, c);

	addaccelgroup(c);

	c->view = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(
	    webkit_user_content_manager_new()));

	gtk_container_add(GTK_CONTAINER(c->win), GTK_WIDGET(c->view));

	g_signal_connect(c->view, "notify::estimated-load-progress",
	    G_CALLBACK(loadprogressed), c);
	g_signal_connect(c->view, "notify::title",
	    G_CALLBACK(titlechanged), c);
	g_signal_connect(c->view, "create",
	    G_CALLBACK(createwindow), c);
	g_signal_connect(c->view, "decide-policy",
	    G_CALLBACK(decidepolicy), c);
	g_signal_connect(c->view, "insecure-content-detected",
	    G_CALLBACK(insecurecontent), c);
	g_signal_connect(c->view, "load-changed",
	    G_CALLBACK(loadchanged), c);
	g_signal_connect(c->view, "mouse-target-changed",
	    G_CALLBACK(mousetargetchanged), c);
	g_signal_connect(c->view, "permission-request",
	    G_CALLBACK(permissionrequest), c);
	g_signal_connect(c->view, "ready-to-show",
	    G_CALLBACK(show), c);
	g_signal_connect(c->view, "resource-load-started",
	    G_CALLBACK(resourceloadstarted), c);

	settings = webkit_web_view_get_settings(c->view);
	webkit_settings_set_auto_load_images(settings, loadimages);
	webkit_settings_set_default_charset(settings, defaultcharset);
	webkit_settings_set_enable_caret_browsing(settings, enablecaretbrowsing);
	webkit_settings_set_enable_developer_extras(settings, enableinspector);
	webkit_settings_set_enable_dns_prefetching(settings, enablednsprefetch);
	webkit_settings_set_enable_java(settings, enablejava);
	webkit_settings_set_enable_javascript(settings, enablejavascript);
	webkit_settings_set_enable_plugins(settings, enableplugins);
	webkit_settings_set_enable_site_specific_quirks(settings, enablesitequirks);
	webkit_settings_set_enable_smooth_scrolling(settings, enablesmoothscrolling);
	webkit_settings_set_default_font_size(settings, defaultfontsize);
	webkit_settings_set_media_playback_requires_user_gesture(settings, nomediaautoplay);
	if ((ua = getenv("SURF_USERAGENT")) == NULL)
		ua = useragent;
	webkit_settings_set_user_agent(settings, ua);
	/* Read http://webkitgtk.org/reference/webkit2gtk/stable/WebKitSettings.html
	 * for more interesting WebKit settings */

	c->finder = webkit_web_view_get_find_controller(c->view);

	if (enableinspector)
		c->inspector = webkit_web_view_get_inspector(c->view);

	if (zoomlevel != 1) {
		arg.i = zoomlevel;
		zoom(c, &arg);
	}

	if (runinfullscreen)
		togglefullscreen(c, NULL);

	if (showwinid) {
		gdk_display_sync(gtk_widget_get_display(c->win));
		fprintf(stdout, "%lu", c->xwin);
		fflush(NULL);
		if (fclose(stdout))
			die("newclient(): Error closing stdout");
	}

	setatom(c, ATOMFIND, "");
	setatom(c, ATOMURI, "about:blank");

	c->next = clients;
	clients = c;

	return c;
}

static void
newwindow(struct _client *c, const union _arg *arg, bool noembed) {
	int i;
	const char *cmd[16], *uri;
	char tmp[64];
	const union _arg a = { .v = cmd };

	i = 0;

	cmd[i++] = argv0;
	cmd[i++] = "-a";
	cmd[i++] = cookiepolicies;
	if (embed && !noembed) {
		cmd[i++] = "-e";
		snprintf(tmp, LENGTH(tmp), "%lu\n", embed);
		cmd[i++] = tmp;
	}
	if (!loadimages)
		cmd[i++] = "-i";
	if (!enablejava)
		cmd[i++] = "-j";
	if (kioskmode)
		cmd[i++] = "-k";
	if (!enableplugins)
		cmd[i++] = "-p";
	if (!enablejavascript)
		cmd[i++] = "-s";
	if (showwinid)
		cmd[i++] = "-x";
	cmd[i++] = "-c";
	cmd[i++] = cookiefile;
	cmd[i++] = "--";
	uri = arg->v ? (char *)arg->v : c->hoveruri;
	if (uri)
		cmd[i++] = uri;
	cmd[i++] = NULL;
	spawn(NULL, &a);
}

static void
pasteuri(GtkClipboard *cb, const char *uri, gpointer p) {
	struct _client *c;
	union _arg arg;

	c = p;
	arg.v = uri;

	if (arg.v)
		loaduri(c, &arg);
}

static gboolean
permissionrequest(WebKitWebView *v, WebKitPermissionRequest *p,
    struct _client *c) {
	if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(p)) {
		if (allowgeolocation)
			webkit_permission_request_allow(p);
		else
			webkit_permission_request_deny(p);

		return TRUE;
	}

	return FALSE;
}

static void
print(struct _client *c, const union _arg *a) {
	webkit_print_operation_run_dialog(webkit_print_operation_new(c->view),
	    GTK_WINDOW(c->win));
}

static GdkFilterReturn
processx(GdkXEvent *xe, GdkEvent *e, gpointer p) {
	struct _client *c;
	XPropertyEvent *ev;
	union _arg arg;

	c = p;

	if (((XEvent *)xe)->type == PropertyNotify) {
		ev = &((XEvent *)xe)->xproperty;
		if (ev->state == PropertyNewValue) {
			if (ev->atom == atoms[ATOMFIND]) {
				arg.b = TRUE;
				find(c, &arg);

				return GDK_FILTER_REMOVE;
			} else if (ev->atom == atoms[ATOMGO]) {
				arg.v = getatom(c, ATOMGO);
				loaduri(c, &arg);

				return GDK_FILTER_REMOVE;
			}
		}
	}
	return GDK_FILTER_CONTINUE;
}

static void
reload(struct _client *c, const union _arg *arg) {
	gboolean nocache = arg->b;
	if (nocache)
		 webkit_web_view_reload_bypass_cache(c->view);
	else
		 webkit_web_view_reload(c->view);
}

static void
resourceloadstarted(WebKitWebView *v, WebKitWebResource *res,
    WebKitURIRequest *req, struct _client *c) {
	const gchar *uri;

	uri = webkit_uri_request_get_uri(req);

	if (g_str_has_suffix(uri, "/favicon.ico"))
		webkit_uri_request_set_uri(req, "about:blank");
}

static void
runjavascript(WebKitWebView *v, const char *jsstr, ...) {
	va_list ap;
	gchar *script;

	va_start(ap, jsstr);
	script = g_strdup_vprintf(jsstr, ap);
	va_end(ap);
	webkit_web_view_run_javascript(v, script, NULL, NULL, NULL);
	g_free(script);
}

static void
scroll_v(struct _client *c, const union _arg *arg) {
	runjavascript(c->view,
		"window.scrollBy(0, %d * (window.innerHeight / 10))", arg->i);
}

static void
scroll_h(struct _client *c, const union _arg *arg) {
	runjavascript(c->view,
		"window.scrollBy(%d * (window.innerWidth / 10), 0)", arg->i);
}

static void
setatom(struct _client *c, enum _atom a, const char *v) {
	XSync(dpy, false);
	XChangeProperty(dpy, c->xwin, atoms[a], XA_STRING, 8,
	    PropModeReplace, (unsigned char *)v, strlen(v) + 1);
}

static char
setcookiepolicy(const WebKitCookieAcceptPolicy p) {
	char policy;

	switch(p) {
	case WEBKIT_COOKIE_POLICY_ACCEPT_NEVER:
		policy = 'a';
		break;
	case WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY:
		policy = '@';
		break;
	case WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS:
	default:
		policy = 'A';
		break;
	}

	return policy;
}

static void
setup(int *argc, char **argv[]) {
	char *proxy;
	WebKitWebContext *context;
	WebKitCookieManager *cm;

	/* clean up any zombies immediately */
	sigchld(0);

	gtk_init(argc, argv);

	showwinid = false;
	embed = 0;
	cookiepolicy = 0;
	dpy = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());

	atoms[ATOMFIND] = XInternAtom(dpy, "_SURF_FIND", false);
	atoms[ATOMGO]   = XInternAtom(dpy, "_SURF_GO", false);
	atoms[ATOMURI]  = XInternAtom(dpy, "_SURF_URI", false);

	cookiefile = buildpath(cookiefile);
	scriptfile = buildpath(scriptfile);
	stylefile  = buildpath(stylefile);

	context = webkit_web_context_get_default();

	/* cookies */
	cm = webkit_web_context_get_cookie_manager(context);
	webkit_cookie_manager_set_persistent_storage(cm, cookiefile,
	    WEBKIT_COOKIE_PERSISTENT_STORAGE_TEXT);
	webkit_cookie_manager_set_accept_policy(cm, getcookiepolicy());

	/* ssl */
	webkit_web_context_set_tls_errors_policy(context, strictssl ?
	    WEBKIT_TLS_ERRORS_POLICY_FAIL : WEBKIT_TLS_ERRORS_POLICY_IGNORE);

	/* proxy */
	if ((proxy = getenv("http_proxy")) && strcmp(proxy, ""))
		usingproxy = true;
	else
		usingproxy = false;

	clients = NULL;
}

static void
show(WebKitWebView *v, struct _client *c) {
	gtk_widget_show_all(c->win);
	gtk_widget_grab_focus(GTK_WIDGET(c->view));
}

static void
sigchld(int unused) {
	if (signal(SIGCHLD, sigchld) == SIG_ERR)
		die("sigchld(): can't install SIGCHLD handler");

	while(waitpid(-1, NULL, WNOHANG) > 0);
}

static void
spawn(struct _client *c, const union _arg *arg) {
	if (fork() == 0) {
		if (dpy)
			close(ConnectionNumber(dpy));
		setsid();
		execvp(((char **)arg->v)[0], (char **)arg->v);
		fprintf(stderr, "surf: execvp %s", ((char **)arg->v)[0]);
		perror(" failed");
		exit(0);
	}
}

static void
stop(struct _client *c, const union _arg *a) {
	webkit_web_view_stop_loading(c->view);
}

static void
titlechanged(WebKitWebView *v, GParamSpec *s, struct _client *c) {
	const gchar *t;

	t = webkit_web_view_get_title(v);
	c->title = copystr(&c->title, t);
	updatetitle(c);
}

static void
toggle(struct _client *c, const union _arg *arg) {
	WebKitSettings *settings;
	char *name;
	gboolean value;
	union _arg a;

	name = (char *)arg->v;
	a.b = FALSE;

	settings = webkit_web_view_get_settings(c->view);
	g_object_get(G_OBJECT(settings), name, &value, NULL);
	g_object_set(G_OBJECT(settings), name, !value, NULL);

	reload(c, &a);
}

static void
togglecookiepolicy(struct _client *c, const union _arg *arg)
{
	WebKitCookieManager *cm;

	cm = webkit_web_context_get_cookie_manager(webkit_web_context_get_default());
	webkit_cookie_manager_get_accept_policy(cm, NULL, NULL, NULL);

	cookiepolicy++;
	cookiepolicy %= strlen(cookiepolicies);

	webkit_cookie_manager_set_accept_policy(cm, getcookiepolicy());

	updatetitle(c);
	/* Do not reload. */
}

static void
togglefullscreen(struct _client *c, const union _arg *arg) {
	if (c->fullscreen)
		gtk_window_unfullscreen(GTK_WINDOW(c->win));
	else
		gtk_window_fullscreen(GTK_WINDOW(c->win));

	c->fullscreen = !c->fullscreen;
}

static void
togglegeolocation(struct _client *c, const union _arg *arg) {
	union _arg a;

	a.b = FALSE;
	allowgeolocation = !allowgeolocation;

	reload(c, &a);
}

static void
togglestyle(struct _client *c, const union _arg *arg) {
	WebKitUserContentManager *cm;
	WebKitUserStyleSheet *ss;
	gchar *style;

	cm = webkit_web_view_get_user_content_manager(c->view);

	if (c->styled) {
		webkit_user_content_manager_remove_all_style_sheets(cm);
		c->styled = FALSE;
	} else {
		g_file_get_contents(stylefile, &style, NULL, NULL);
		if (style) {
			ss = webkit_user_style_sheet_new(style,
			    WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER,
			    stylewhitelist, styleblacklist);
			webkit_user_content_manager_add_style_sheet(cm, ss);
			g_free(style);
			c->styled = TRUE;
		}
	}

	updatetitle(c);
}

static void
updatetitle(struct _client *c) {
	gchar *t;

	if (showindicators) {
		gettogglestats(c);
		getpagestats(c);

		if (c->progress < 100)
			t = g_strdup_printf("[%i%%] ", c->progress);
		else
			t = g_strdup_printf("");

		t = g_strdup_printf("%s%s:%s", t, togglestats, pagestats);


		if (c->hoveruri) {
			t = g_strdup_printf("%s > %s",
			    t, c->hoveruri);
			if (c->hovertitle)
				t = g_strdup_printf("%s [%s]", t, c->hovertitle);
		} else
			t = g_strdup_printf("%s | %s",
			    t, (c->title == NULL) ? "" : c->title);

		if (c->hovercontent)
			t = g_strdup_printf("%s <%s>", t, c->hovercontent);

		gtk_window_set_title(GTK_WINDOW(c->win), t);
		g_free(t);
	} else
		gtk_window_set_title(GTK_WINDOW(c->win),
		    (c->title == NULL) ? "" : c->title);
}

static void
updatewinid(struct _client *c) {
	snprintf(winid, LENGTH(winid), "%lu", c->xwin);
}

static void
usage(void) {
	die("usage: %s [-fFgGiIjJkKnNpPsSvx]"
	    " [-a cookiepolicies ] "
	    " [-c cookiefile] [-e xid] [-r scriptfile]"
	    " [-t stylefile] [-u useragent] [-z zoomlevel]"
	    " [uri]\n", basename(argv0));
}

static void
zoom(struct _client *c, const union _arg *arg) {
	gdouble zoom;

	zoom = webkit_web_view_get_zoom_level(c->view);
	if (arg->i < 0) {
		/* zoom out */
		webkit_web_view_set_zoom_level(c->view, zoom - 0.1);
	} else if (arg->i > 0) {
		/* zoom in */
		webkit_web_view_set_zoom_level(c->view, zoom + 0.1);
	} else {
		/* reset */
		webkit_web_view_set_zoom_level(c->view, 1.0);
	}
}

int
main(int argc, char *argv[]) {
	union _arg arg;
	struct _client *c;

	memset(&arg, 0, sizeof(arg));

	setup(&argc, &argv);

	ARGBEGIN {
	case 'a':
		cookiepolicies = EARGF(usage());
		break;
	case 'c':
		cookiefile = EARGF(usage());
		break;
	case 'e':
		embed = strtol(EARGF(usage()), NULL, 0);
		break;
	case 'f':
		runinfullscreen = 1;
		break;
	case 'F':
		runinfullscreen = 0;
		break;
	case 'g':
		allowgeolocation = 0;
		break;
	case 'G':
		allowgeolocation = 1;
		break;
	case 'i':
		loadimages = 0;
		break;
	case 'I':
		loadimages = 1;
		break;
	case 'j':
		enablejava = 0;
		break;
	case 'J':
		enablejava = 1;
		break;
	case 'k':
		kioskmode = 0;
		break;
	case 'K':
		kioskmode = 1;
		break;
	case 'n':
		enableinspector = 0;
		break;
	case 'N':
		enableinspector = 1;
		break;
	case 'p':
		enableplugins = 0;
		break;
	case 'P':
		enableplugins = 1;
		break;
	case 'r':
		scriptfile = EARGF(usage());
		break;
	case 's':
		enablejavascript = 0;
		break;
	case 'S':
		enablejavascript = 1;
		break;
	case 't':
		stylefile = EARGF(usage());
		break;
	case 'u':
		useragent = EARGF(usage());
		break;
	case 'v':
		die("surf-"VERSION", ©2009-2014 surf engineers, "
		    "see LICENSE for details\n");
	case 'x':
		showwinid = TRUE;
		break;
	case 'z':
		zoomlevel = strtof(EARGF(usage()), NULL);
		break;
	default:
		usage();
	} ARGEND;

	if (argc > 0)
		arg.v = argv[0];

	c = newclient();
	show(NULL, c);

	if (arg.v)
		loaduri(c, &arg);
	else
		updatetitle(c);

	gtk_main();

	cleanup();

	return EXIT_SUCCESS;
}
