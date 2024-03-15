# dtray version
VERSION = 0.1

# paths
PREFIX = /usr/local
MANPREFIX = ${PREFIX}/share/man

# includes and libs
INCS = -I/usr/include/dbus-1.0 -I/usr/lib/dbus-1.0/include
LIBS = -lX11 -ldbus-1

# flags
CPPFLAGS = -D_DEFAULT_SOURCE -DVERSION=\"${VERSION}\"
CFLAGS   = -std=c99 -pedantic -Wall -Os ${INCS} ${CPPFLAGS}
LDFLAGS  = ${LIBS}

# compiler
CC = cc
