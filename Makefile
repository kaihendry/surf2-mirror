# surf2 - simple browser
# See LICENSE file for copyright and license details.

include config.mk

SRC = surf2.c
OBJ = ${SRC:.c=.o}

all: options surf2

options:
	@echo surf2 build options:
	@echo "CFLAGS   = ${CFLAGS}"
	@echo "LDFLAGS  = ${LDFLAGS}"
	@echo "CC       = ${CC}"

.c.o:
	@echo CC $<
	@${CC} -c ${CFLAGS} $<

${OBJ}: config.h config.mk

config.h:
	@echo creating $@ from config.def.h
	@cp config.def.h $@

surf2: ${OBJ}
	@echo CC -o $@
	@${CC} -o $@ surf2.o ${LDFLAGS}

clean:
	@echo cleaning
	@rm -f surf2 ${OBJ} surf2-${VERSION}.tar.gz

dist: clean
	@echo creating dist tarball
	@mkdir -p surf2-${VERSION}
	@cp -R LICENSE Makefile config.mk config.def.h README \
		surf2-open.sh arg.h TODO.md surf2.png \
		surf2.1 ${SRC} surf2-${VERSION}
	@tar -cf surf2-${VERSION}.tar surf2-${VERSION}
	@gzip surf2-${VERSION}.tar
	@rm -rf surf2-${VERSION}

install: all
	@echo installing executable file to ${DESTDIR}${PREFIX}/bin
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@cp -f surf2 ${DESTDIR}${PREFIX}/bin
	@chmod 755 ${DESTDIR}${PREFIX}/bin/surf2
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@sed "s/VERSION/${VERSION}/g" < surf2.1 > ${DESTDIR}${MANPREFIX}/man1/surf2.1
	@chmod 644 ${DESTDIR}${MANPREFIX}/man1/surf2.1

uninstall:
	@echo removing executable file from ${DESTDIR}${PREFIX}/bin
	@rm -f ${DESTDIR}${PREFIX}/bin/surf2
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/surf2.1

.PHONY: all options clean dist install uninstall
