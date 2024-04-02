/* See LICENSE file for copyright and license details.
 *
 * dtray - dbus tray daemon
 * A minimal StatusNotifierItem (SNI) host that creates XEMBED windows
 * for system tray icons, enabling right-click menus in dwm's systray.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <dbus/dbus.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "config.h"

#define MAX_ITEMS 64
#define SYSTEM_TRAY_REQUEST_DOCK 0

#define WATCHER_PATH "/StatusNotifierWatcher"
#define WATCHER_IFACE "org.kde.StatusNotifierWatcher"
#define ITEM_IFACE "org.kde.StatusNotifierItem"
#define PROP_IFACE "org.freedesktop.DBus.Properties"
#define INTROSPECT_IFACE "org.freedesktop.DBus.Introspectable"

static const char *introspect_xml =
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n"
	"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n"
	"<node>\n"
	"  <interface name=\"org.kde.StatusNotifierWatcher\">\n"
	"    <method name=\"RegisterStatusNotifierItem\">\n"
	"      <arg direction=\"in\" name=\"service\" type=\"s\"/>\n"
	"    </method>\n"
	"    <method name=\"RegisterStatusNotifierHost\">\n"
	"      <arg direction=\"in\" name=\"service\" type=\"s\"/>\n"
	"    </method>\n"
	"    <property name=\"IsStatusNotifierHostRegistered\" type=\"b\" access=\"read\"/>\n"
	"    <property name=\"ProtocolVersion\" type=\"i\" access=\"read\"/>\n"
	"    <property name=\"RegisteredStatusNotifierItems\" type=\"as\" access=\"read\"/>\n"
	"    <signal name=\"StatusNotifierItemRegistered\">\n"
	"      <arg type=\"s\"/>\n"
	"    </signal>\n"
	"    <signal name=\"StatusNotifierItemUnregistered\">\n"
	"      <arg type=\"s\"/>\n"
	"    </signal>\n"
	"    <signal name=\"StatusNotifierHostRegistered\"/>\n"
	"  </interface>\n"
	"  <interface name=\"org.freedesktop.DBus.Properties\">\n"
	"    <method name=\"Get\">\n"
	"      <arg direction=\"in\" name=\"interface\" type=\"s\"/>\n"
	"      <arg direction=\"in\" name=\"property\" type=\"s\"/>\n"
	"      <arg direction=\"out\" name=\"value\" type=\"v\"/>\n"
	"    </method>\n"
	"    <method name=\"GetAll\">\n"
	"      <arg direction=\"in\" name=\"interface\" type=\"s\"/>\n"
	"      <arg direction=\"out\" name=\"properties\" type=\"a{sv}\"/>\n"
	"    </method>\n"
	"  </interface>\n"
	"  <interface name=\"org.freedesktop.DBus.Introspectable\">\n"
	"    <method name=\"Introspect\">\n"
	"      <arg direction=\"out\" name=\"xml\" type=\"s\"/>\n"
	"    </method>\n"
	"  </interface>\n"
	"</node>\n";

typedef struct {
	char *service;
	char *path;
	Window win;
	GC gc;
	Pixmap pixmap;
	int icon_width;
	int icon_height;
} Item;

static Display *dpy;
static int screen;
static Window root;
static Window tray;
static Window last_tray;
static Visual *visual;
static int depth;
static Colormap colormap;
static Atom netatom[2];
static DBusConnection *conn;
static Item items[MAX_ITEMS];
static int nitems = 0;
static int running = 1;

enum { NetSystemTray, NetSystemTrayOpcode };

static void render_icon(Item *item);
static void fetch_icon(Item *item);
static Window get_tray(void);
static Window create_icon_window(GC *gc_out);

static int
xerror(Display *dpy, XErrorEvent *ee)
{
	/* Ignore X errors during redocking */
	return 0;
}

static int
xioerror(Display *dpy)
{
	/* X connection broken - exit cleanly */
	running = 0;
	return 0;
}

static void
die(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	exit(1);
}

static void
sighandler(int sig)
{
	running = 0;
}

static Window
get_tray(void)
{
	char atom_name[64];
	Atom selection;
	Window owner;

	snprintf(atom_name, sizeof(atom_name), "_NET_SYSTEM_TRAY_S%d", screen);
	selection = XInternAtom(dpy, atom_name, False);
	owner = XGetSelectionOwner(dpy, selection);
	return owner;
}

static void
send_tray_message(Window w, long message, long data1, long data2, long data3)
{
	XEvent ev;

	memset(&ev, 0, sizeof(ev));
	ev.xclient.type = ClientMessage;
	ev.xclient.window = tray;
	ev.xclient.message_type = netatom[NetSystemTrayOpcode];
	ev.xclient.format = 32;
	ev.xclient.data.l[0] = CurrentTime;
	ev.xclient.data.l[1] = message;
	ev.xclient.data.l[2] = w;
	ev.xclient.data.l[3] = data1;
	ev.xclient.data.l[4] = data2;

	XSendEvent(dpy, tray, False, NoEventMask, &ev);
	XSync(dpy, False);
}

static void
redock_all(void)
{
	int i;
	struct timespec ts = { 0, 100000000 }; /* 100ms */

	tray = get_tray();
	if (!tray)
		return;


	/* Wait for systray to be ready */
	nanosleep(&ts, NULL);

	for (i = 0; i < nitems; i++) {
		if (items[i].service) {
			/* Destroy old window and create new one */
			if (items[i].win) {
				XDestroyWindow(dpy, items[i].win);
				if (items[i].gc)
					XFreeGC(dpy, items[i].gc);
			}
			items[i].win = create_icon_window(&items[i].gc);

			/* Request dock and map */
			send_tray_message(items[i].win, SYSTEM_TRAY_REQUEST_DOCK, 0, 0, 0);
			XMapWindow(dpy, items[i].win);

			/* Re-fetch and render icon */
			if (items[i].pixmap) {
				XFreePixmap(dpy, items[i].pixmap);
				items[i].pixmap = 0;
			}
			fetch_icon(&items[i]);
			if (items[i].pixmap)
				render_icon(&items[i]);
		}
	}
	XSync(dpy, False);
	last_tray = tray;
}

static Item *
find_item(const char *service)
{
	int i;
	for (i = 0; i < nitems; i++) {
		if (items[i].service && strcmp(items[i].service, service) == 0)
			return &items[i];
	}
	return NULL;
}

static Item *
find_item_by_window(Window w)
{
	int i;
	for (i = 0; i < nitems; i++) {
		if (items[i].win == w)
			return &items[i];
	}
	return NULL;
}

static void
send_dbus_signal(const char *signal_name, const char *arg)
{
	DBusMessage *sig;

	sig = dbus_message_new_signal(WATCHER_PATH, WATCHER_IFACE, signal_name);
	if (sig) {
		if (arg)
			dbus_message_append_args(sig, DBUS_TYPE_STRING, &arg, DBUS_TYPE_INVALID);
		dbus_connection_send(conn, sig, NULL);
		dbus_message_unref(sig);
	}
}

static void
activate_item(Item *item, int x, int y)
{
	DBusMessage *msg;

	if (!item || !item->service || !item->path)
		return;

	msg = dbus_message_new_method_call(item->service, item->path, ITEM_IFACE, "Activate");
	if (msg) {
		dbus_message_append_args(msg, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
		dbus_connection_send(conn, msg, NULL);
		dbus_message_unref(msg);
	}
}

static void
context_menu(Item *item, int x, int y)
{
	DBusMessage *msg;

	if (!item || !item->service || !item->path)
		return;

	msg = dbus_message_new_method_call(item->service, item->path, ITEM_IFACE, "ContextMenu");
	if (msg) {
		dbus_message_append_args(msg, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
		dbus_connection_send(conn, msg, NULL);
		dbus_message_unref(msg);
	}
}

static void
secondary_activate(Item *item, int x, int y)
{
	DBusMessage *msg;

	if (!item || !item->service || !item->path)
		return;

	msg = dbus_message_new_method_call(item->service, item->path, ITEM_IFACE, "SecondaryActivate");
	if (msg) {
		dbus_message_append_args(msg, DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y, DBUS_TYPE_INVALID);
		dbus_connection_send(conn, msg, NULL);
		dbus_message_unref(msg);
	}
}

static void
render_icon(Item *item)
{
	if (!item || !item->pixmap || !item->win)
		return;

	int dst_x = (iconsize - item->icon_width) / 2;
	int dst_y = (iconsize - item->icon_height) / 2;
	if (dst_x < 0) dst_x = 0;
	if (dst_y < 0) dst_y = 0;

	XClearWindow(dpy, item->win);
	XCopyArea(dpy, item->pixmap, item->win, item->gc,
		0, 0, item->icon_width, item->icon_height, dst_x, dst_y);
	XFlush(dpy);
}

static void
fetch_icon(Item *item)
{
	DBusMessage *msg, *reply;
	DBusMessageIter iter, variant, arr, st;
	DBusError err;
	int best_w = 0, best_h = 0;
	unsigned char *best_data = NULL;

	if (!item || !item->service || !item->path)
		return;

	dbus_error_init(&err);

	msg = dbus_message_new_method_call(item->service, item->path, PROP_IFACE, "Get");
	if (!msg)
		return;

	const char *iface = ITEM_IFACE;
	const char *prop = "IconPixmap";
	dbus_message_append_args(msg,
		DBUS_TYPE_STRING, &iface,
		DBUS_TYPE_STRING, &prop,
		DBUS_TYPE_INVALID);

	reply = dbus_connection_send_with_reply_and_block(conn, msg, 1000, &err);
	dbus_message_unref(msg);

	if (dbus_error_is_set(&err)) {
		dbus_error_free(&err);
		return;
	}
	if (!reply)
		return;

	if (!dbus_message_iter_init(reply, &iter)) {
		dbus_message_unref(reply);
		return;
	}

	if (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_VARIANT) {
		dbus_message_unref(reply);
		return;
	}

	dbus_message_iter_recurse(&iter, &variant);
	if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_ARRAY) {
		dbus_message_unref(reply);
		return;
	}

	dbus_message_iter_recurse(&variant, &arr);
	while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
		int w, h, len;
		unsigned char *data;
		DBusMessageIter data_iter;

		dbus_message_iter_recurse(&arr, &st);

		if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
			goto next;
		dbus_message_iter_get_basic(&st, &w);
		dbus_message_iter_next(&st);

		if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_INT32)
			goto next;
		dbus_message_iter_get_basic(&st, &h);
		dbus_message_iter_next(&st);

		if (dbus_message_iter_get_arg_type(&st) != DBUS_TYPE_ARRAY)
			goto next;

		dbus_message_iter_recurse(&st, &data_iter);
		dbus_message_iter_get_fixed_array(&data_iter, &data, &len);

		if (w > 0 && h > 0 && len == w * h * 4) {
			int size_diff = abs(w - iconsize);
			int best_diff = abs(best_w - iconsize);
			if (!best_data || size_diff < best_diff ||
			    (size_diff == best_diff && w > best_w)) {
				best_w = w;
				best_h = h;
				best_data = data;
			}
		}
next:
		dbus_message_iter_next(&arr);
	}

	if (best_data && best_w > 0 && best_h > 0) {
		XImage *img;
		char *imgdata;
		int i;

		if (item->pixmap) {
			XFreePixmap(dpy, item->pixmap);
			item->pixmap = 0;
		}

		int target_w = best_w > iconsize ? iconsize : best_w;
		int target_h = best_h > iconsize ? iconsize : best_h;

		imgdata = malloc(target_w * target_h * 4);
		if (!imgdata) {
			dbus_message_unref(reply);
			return;
		}

		for (i = 0; i < target_w * target_h; i++) {
			int src_x = (i % target_w) * best_w / target_w;
			int src_y = (i / target_w) * best_h / target_h;
			int src_i = src_y * best_w + src_x;
			unsigned char a = best_data[src_i * 4];
			unsigned char r = best_data[src_i * 4 + 1];
			unsigned char g = best_data[src_i * 4 + 2];
			unsigned char b = best_data[src_i * 4 + 3];
			/* Premultiply alpha and convert to BGRX for X11 (24/32-bit) */
			if (a == 0) {
				imgdata[i * 4 + 0] = 0;
				imgdata[i * 4 + 1] = 0;
				imgdata[i * 4 + 2] = 0;
				imgdata[i * 4 + 3] = 0;
			} else {
				imgdata[i * 4 + 0] = b;
				imgdata[i * 4 + 1] = g;
				imgdata[i * 4 + 2] = r;
				imgdata[i * 4 + 3] = 0;
			}
		}

		img = XCreateImage(dpy, visual, depth, ZPixmap, 0,
			imgdata, target_w, target_h, 32, 0);
		if (img) {
			item->pixmap = XCreatePixmap(dpy, root, target_w, target_h, depth);
			item->icon_width = target_w;
			item->icon_height = target_h;
			XPutImage(dpy, item->pixmap, item->gc, img, 0, 0, 0, 0, target_w, target_h);
			XDestroyImage(img);
		} else {
			free(imgdata);
		}
	}

	dbus_message_unref(reply);
}

static Window
create_icon_window(GC *gc_out)
{
	Window win;
	XSetWindowAttributes wa;
	XColor color;
	XGCValues gcv;

	XParseColor(dpy, colormap, bgcolor, &color);
	XAllocColor(dpy, colormap, &color);

	wa.background_pixel = color.pixel;
	wa.colormap = colormap;
	wa.event_mask = ButtonPressMask | ButtonReleaseMask | ExposureMask;
	wa.override_redirect = False;

	win = XCreateWindow(dpy, root, 0, 0, iconsize, iconsize, 0,
		depth, InputOutput, visual,
		CWBackPixel | CWColormap | CWEventMask | CWOverrideRedirect, &wa);

	gcv.graphics_exposures = False;
	*gc_out = XCreateGC(dpy, win, GCGraphicsExposures, &gcv);

	return win;
}

static void
add_item(const char *service, const char *path)
{
	Item *item;
	int i;
	char full_service[256];

	if (nitems >= MAX_ITEMS) {
		fprintf(stderr, "dtray: max items reached\n");
		return;
	}

	/* Check if already exists */
	if (find_item(service))
		return;

	/* Find empty slot */
	for (i = 0; i < MAX_ITEMS; i++) {
		if (!items[i].service)
			break;
	}

	item = &items[i];
	item->service = strdup(service);
	item->path = strdup(path);
	item->win = create_icon_window(&item->gc);
	item->pixmap = 0;
	item->icon_width = 0;
	item->icon_height = 0;

	if (i >= nitems)
		nitems = i + 1;

	/* Request dock in system tray */
	tray = get_tray();
	if (tray) {
		send_tray_message(item->win, SYSTEM_TRAY_REQUEST_DOCK, 0, 0, 0);
		XMapWindow(dpy, item->win);
	}

	/* Fetch and render icon */
	fetch_icon(item);
	if (item->pixmap)
		render_icon(item);

	/* Send signal that item was registered */
	snprintf(full_service, sizeof(full_service), "%s%s", service, path);
	send_dbus_signal("StatusNotifierItemRegistered", full_service);

}

static void
remove_item(const char *service)
{
	Item *item = find_item(service);
	char full_service[256];

	if (!item)
		return;

	snprintf(full_service, sizeof(full_service), "%s%s", item->service, item->path);
	send_dbus_signal("StatusNotifierItemUnregistered", full_service);

	if (item->pixmap)
		XFreePixmap(dpy, item->pixmap);
	if (item->gc)
		XFreeGC(dpy, item->gc);
	if (item->win)
		XDestroyWindow(dpy, item->win);
	free(item->service);
	free(item->path);
	item->service = NULL;
	item->path = NULL;
	item->win = 0;
	item->gc = 0;
	item->pixmap = 0;

}

static DBusHandlerResult
handle_watcher_method(DBusConnection *connection, DBusMessage *msg)
{
	const char *member = dbus_message_get_member(msg);
	DBusMessage *reply;

	if (strcmp(member, "RegisterStatusNotifierItem") == 0) {
		const char *service = NULL;
		const char *path;
		const char *sender;

		dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID);
		sender = dbus_message_get_sender(msg);

		if (service && service[0] == '/') {
			path = service;
			service = sender;
		} else if (service && service[0] == ':') {
			path = "/StatusNotifierItem";
		} else {
			path = "/StatusNotifierItem";
			if (!service || service[0] == '\0')
				service = sender;
		}

		add_item(service, path);

		reply = dbus_message_new_method_return(msg);
		if (reply) {
			dbus_connection_send(connection, reply, NULL);
			dbus_message_unref(reply);
		}
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "RegisterStatusNotifierHost") == 0) {
		reply = dbus_message_new_method_return(msg);
		if (reply) {
			dbus_connection_send(connection, reply, NULL);
			dbus_message_unref(reply);
		}
		send_dbus_signal("StatusNotifierHostRegistered", NULL);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
handle_properties(DBusConnection *connection, DBusMessage *msg)
{
	const char *member = dbus_message_get_member(msg);
	const char *iface = NULL;
	const char *prop = NULL;
	DBusMessage *reply;
	DBusMessageIter iter, variant;

	if (strcmp(member, "Get") == 0) {
		dbus_message_get_args(msg, NULL,
			DBUS_TYPE_STRING, &iface,
			DBUS_TYPE_STRING, &prop,
			DBUS_TYPE_INVALID);

		reply = dbus_message_new_method_return(msg);
		if (!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_iter_init_append(reply, &iter);

		if (strcmp(prop, "IsStatusNotifierHostRegistered") == 0) {
			dbus_bool_t val = TRUE;
			dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "b", &variant);
			dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &val);
			dbus_message_iter_close_container(&iter, &variant);
		} else if (strcmp(prop, "ProtocolVersion") == 0) {
			int val = 0;
			dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "i", &variant);
			dbus_message_iter_append_basic(&variant, DBUS_TYPE_INT32, &val);
			dbus_message_iter_close_container(&iter, &variant);
		} else if (strcmp(prop, "RegisteredStatusNotifierItems") == 0) {
			DBusMessageIter arr;
			int i;
			dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, "as", &variant);
			dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "s", &arr);
			for (i = 0; i < nitems; i++) {
				if (items[i].service) {
					char buf[256];
					snprintf(buf, sizeof(buf), "%s%s", items[i].service, items[i].path);
					const char *p = buf;
					dbus_message_iter_append_basic(&arr, DBUS_TYPE_STRING, &p);
				}
			}
			dbus_message_iter_close_container(&variant, &arr);
			dbus_message_iter_close_container(&iter, &variant);
		} else {
			dbus_message_unref(reply);
			reply = dbus_message_new_error(msg, DBUS_ERROR_UNKNOWN_PROPERTY, "Unknown property");
		}

		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	if (strcmp(member, "GetAll") == 0) {
		DBusMessageIter dict, entry, var;
		dbus_bool_t host_reg = TRUE;
		int proto_ver = 0;

		reply = dbus_message_new_method_return(msg);
		if (!reply)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_iter_init_append(reply, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

		/* IsStatusNotifierHostRegistered */
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *key1 = "IsStatusNotifierHostRegistered";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key1);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &var);
		dbus_message_iter_append_basic(&var, DBUS_TYPE_BOOLEAN, &host_reg);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(&dict, &entry);

		/* ProtocolVersion */
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *key2 = "ProtocolVersion";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key2);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "i", &var);
		dbus_message_iter_append_basic(&var, DBUS_TYPE_INT32, &proto_ver);
		dbus_message_iter_close_container(&entry, &var);
		dbus_message_iter_close_container(&dict, &entry);

		dbus_message_iter_close_container(&iter, &dict);

		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
		return DBUS_HANDLER_RESULT_HANDLED;
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
handle_introspect(DBusConnection *connection, DBusMessage *msg)
{
	DBusMessage *reply;

	reply = dbus_message_new_method_return(msg);
	if (reply) {
		dbus_message_append_args(reply, DBUS_TYPE_STRING, &introspect_xml, DBUS_TYPE_INVALID);
		dbus_connection_send(connection, reply, NULL);
		dbus_message_unref(reply);
	}
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult
message_handler(DBusConnection *connection, DBusMessage *msg, void *data)
{
	const char *iface = dbus_message_get_interface(msg);
	const char *path = dbus_message_get_path(msg);
	const char *member = dbus_message_get_member(msg);
	int type = dbus_message_get_type(msg);

	if (type != DBUS_MESSAGE_TYPE_METHOD_CALL)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!path || strcmp(path, WATCHER_PATH) != 0)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (iface && strcmp(iface, WATCHER_IFACE) == 0)
		return handle_watcher_method(connection, msg);

	if (iface && strcmp(iface, PROP_IFACE) == 0)
		return handle_properties(connection, msg);

	if (iface && strcmp(iface, INTROSPECT_IFACE) == 0 && strcmp(member, "Introspect") == 0)
		return handle_introspect(connection, msg);

	/* Handle method calls without interface (some apps do this) */
	if (!iface && member) {
		if (strcmp(member, "RegisterStatusNotifierItem") == 0 ||
		    strcmp(member, "RegisterStatusNotifierHost") == 0)
			return handle_watcher_method(connection, msg);
		if (strcmp(member, "Get") == 0 || strcmp(member, "GetAll") == 0)
			return handle_properties(connection, msg);
		if (strcmp(member, "Introspect") == 0)
			return handle_introspect(connection, msg);
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusHandlerResult
filter_handler(DBusConnection *connection, DBusMessage *msg, void *data)
{
	const char *iface = dbus_message_get_interface(msg);
	const char *member = dbus_message_get_member(msg);
	const char *sender = dbus_message_get_sender(msg);

	/* Handle NameOwnerChanged for cleanup */
	if (iface && strcmp(iface, "org.freedesktop.DBus") == 0 &&
	    member && strcmp(member, "NameOwnerChanged") == 0) {
		const char *name, *old_owner, *new_owner;
		if (dbus_message_get_args(msg, NULL,
		    DBUS_TYPE_STRING, &name,
		    DBUS_TYPE_STRING, &old_owner,
		    DBUS_TYPE_STRING, &new_owner,
		    DBUS_TYPE_INVALID)) {
			if (new_owner[0] == '\0')
				remove_item(name);
		}
	}

	/* Handle NewIcon signal to refresh icon */
	if (iface && strcmp(iface, ITEM_IFACE) == 0 &&
	    member && strcmp(member, "NewIcon") == 0 && sender) {
		Item *item = find_item(sender);
		if (item) {
			fetch_icon(item);
			render_icon(item);
		}
	}

	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static DBusObjectPathVTable vtable = {
	.message_function = message_handler
};

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

	/* Request the StatusNotifierWatcher names */
	ret = dbus_bus_request_name(conn, "org.kde.StatusNotifierWatcher",
		DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "dtray: name request error: %s\n", err.message);
		dbus_error_free(&err);
		return 0;
	}
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
		fprintf(stderr, "dtray: could not become org.kde.StatusNotifierWatcher\n");
		return 0;
	}

	ret = dbus_bus_request_name(conn, "org.freedesktop.StatusNotifierWatcher",
		DBUS_NAME_FLAG_REPLACE_EXISTING, NULL);

	/* Register object path handler */
	if (!dbus_connection_register_object_path(conn, WATCHER_PATH, &vtable, NULL)) {
		fprintf(stderr, "dtray: failed to register object path\n");
		return 0;
	}

	/* Add filter for NameOwnerChanged and NewIcon */
	dbus_connection_add_filter(conn, filter_handler, NULL, NULL);
	dbus_bus_add_match(conn,
		"type='signal',interface='org.freedesktop.DBus',member='NameOwnerChanged'",
		NULL);
	dbus_bus_add_match(conn,
		"type='signal',interface='org.kde.StatusNotifierItem',member='NewIcon'",
		NULL);

	return 1;
}

static void
handle_xevent(XEvent *ev)
{
	Item *item;
	int x, y;
	Window child;

	switch (ev->type) {
	case Expose:
		if (ev->xexpose.count == 0) {
			item = find_item_by_window(ev->xexpose.window);
			if (item)
				render_icon(item);
		}
		break;
	case ButtonPress:
		item = find_item_by_window(ev->xbutton.window);
		if (!item)
			break;

		XTranslateCoordinates(dpy, ev->xbutton.window, root,
			ev->xbutton.x, ev->xbutton.y, &x, &y, &child);

		switch (ev->xbutton.button) {
		case 1:
			activate_item(item, x, y);
			break;
		case 2:
			secondary_activate(item, x, y);
			break;
		case 3:
			context_menu(item, x, y);
			break;
		}
		break;
	}
}

static void
run(void)
{
	XEvent ev;
	int xfd, dfd, maxfd;
	fd_set fds;
	struct timeval tv;

	xfd = ConnectionNumber(dpy);
	dbus_connection_get_unix_fd(conn, &dfd);

	while (running) {
		while (XPending(dpy)) {
			XNextEvent(dpy, &ev);
			handle_xevent(&ev);
		}

		while (dbus_connection_dispatch(conn) == DBUS_DISPATCH_DATA_REMAINS)
			;

		FD_ZERO(&fds);
		FD_SET(xfd, &fds);
		maxfd = xfd;
		if (dfd >= 0) {
			FD_SET(dfd, &fds);
			if (dfd > maxfd)
				maxfd = dfd;
		}

		dbus_connection_flush(conn);

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if (select(maxfd + 1, &fds, NULL, NULL, &tv) < 0) {
			if (running)
				perror("dtray: select");
		}

		/* Check if systray owner changed (e.g., dwm restarted) */
		{
			Window new_tray = get_tray();
			if (new_tray != last_tray) {
				if (!new_tray) {
					/* Tray gone - hide windows immediately so new WM doesn't manage them */
					int i;
					for (i = 0; i < nitems; i++) {
						if (items[i].win)
							XUnmapWindow(dpy, items[i].win);
					}
					XSync(dpy, False);
					last_tray = 0;
				} else {
					redock_all();
				}
			}
		}

		if (dfd >= 0 && FD_ISSET(dfd, &fds))
			dbus_connection_read_write(conn, 0);
	}
}

static void
cleanup(void)
{
	int i;
	for (i = 0; i < nitems; i++) {
		if (items[i].service)
			free(items[i].service);
		if (items[i].path)
			free(items[i].path);
		if (items[i].pixmap)
			XFreePixmap(dpy, items[i].pixmap);
		if (items[i].gc)
			XFreeGC(dpy, items[i].gc);
		if (items[i].win)
			XDestroyWindow(dpy, items[i].win);
	}
	if (dpy)
		XCloseDisplay(dpy);
	if (conn)
		dbus_connection_unref(conn);
}

int
main(int argc, char *argv[])
{
	if (argc > 1 && strcmp(argv[1], "-v") == 0)
		die("dtray-" VERSION "\n");

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	dpy = XOpenDisplay(NULL);
	if (!dpy)
		die("dtray: cannot open display\n");

	screen = DefaultScreen(dpy);
	root = RootWindow(dpy, screen);

	/* Use default visual to match dwm's systray */
	visual = DefaultVisual(dpy, screen);
	depth = DefaultDepth(dpy, screen);
	colormap = DefaultColormap(dpy, screen);

	XSetErrorHandler(xerror);
	XSetIOErrorHandler(xioerror);

	netatom[NetSystemTray] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_S0", False);
	netatom[NetSystemTrayOpcode] = XInternAtom(dpy, "_NET_SYSTEM_TRAY_OPCODE", False);

	if (!setup_dbus()) {
		XCloseDisplay(dpy);
		return 1;
	}

	last_tray = get_tray();

	run();

	cleanup();
	return 0;
}
