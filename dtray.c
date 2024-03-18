/* See LICENSE file for copyright and license details.
 *
 * dtray - dbus tray daemon
 * A minimal StatusNotifierItem (SNI) host for dwm's systray.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <dbus/dbus.h>
#include <X11/Xlib.h>

#include "config.h"

#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"

static Display *dpy;
static DBusConnection *conn;
static int running = 1;

static void
die(const char *fmt, ...)
{
	fprintf(stderr, "dtray: %s\n", fmt);
	exit(1);
}

static void
sighandler(int sig)
{
	running = 0;
}

static int
setup_dbus(void)
{
	DBusError err;
	int ret;

	dbus_error_init(&err);

	conn = dbus_bus_get(DBUS_BUS_SESSION, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "dtray: dbus connection error: %s\n", err.message);
		dbus_error_free(&err);
		return 0;
	}

	ret = dbus_bus_request_name(conn, "org.kde.StatusNotifierWatcher",
		DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "dtray: name request error: %s\n", err.message);
		dbus_error_free(&err);
		return 0;
	}
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "dtray: could not become StatusNotifierWatcher\n");
		return 0;
	}

	return 1;
}

static void
run(void)
{
	while (running) {
		dbus_connection_read_write_dispatch(conn, 1000);
	}
}

int
main(int argc, char *argv[])
{
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("cannot open display");

	if (!setup_dbus()) {
		XCloseDisplay(dpy);
		return 1;
	}

	run();

	XCloseDisplay(dpy);
	dbus_connection_unref(conn);
	return 0;
}
