# surf version
VERSION = 0.6

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

X11INC = /usr/X11R6/include
X11LIB = /usr/X11R6/lib

GTKINC = `pkg-config --cflags gtk+-3.0 webkit2gtk-3.0`
GTKLIB = `pkg-config --libs gtk+-3.0 webkit2gtk-3.0`

# includes and libs
INCS = -I. -I/usr/include -I${X11INC} ${GTKINC}
LIBS = -L/usr/lib -lc -L${X11LIB} -lX11 ${GTKLIB} -lgthread-2.0 -lbsd

# flags
CPPFLAGS = -DVERSION=\"${VERSION}\" -D_POSIX_SOURCE -D_DEFAULT_SOURCE
CFLAGS = -std=c99 -pedantic -Wall -Os -s ${INCS} ${CPPFLAGS}
LDFLAGS = -s ${LIBS}

# Solaris
#CFLAGS = -fast ${INCS} -DVERSION=\"${VERSION}\"
#LDFLAGS = ${LIBS}

# compiler and linker
CC = cc