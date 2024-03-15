# dtray - dbus tray daemon
# See LICENSE file for copyright and license details.

include config.mk

SRC = dtray.c
OBJ = ${SRC:.c=.o}

all: dtray

.c.o:
	${CC} -c ${CFLAGS} $<

dtray: ${OBJ}
	${CC} -o $@ ${OBJ} ${LDFLAGS}

clean:
	rm -f dtray ${OBJ}

install: all
	mkdir -p ${DESTDIR}${PREFIX}/bin
	cp -f dtray ${DESTDIR}${PREFIX}/bin
	chmod 755 ${DESTDIR}${PREFIX}/bin/dtray

uninstall:
	rm -f ${DESTDIR}${PREFIX}/bin/dtray

.PHONY: all clean install uninstall
