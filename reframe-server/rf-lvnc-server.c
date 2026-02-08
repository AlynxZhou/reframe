#include <stdbool.h>
#include <rfb/rfb.h>

#include "rf-common.h"
#include "rf-lvnc-server.h"

struct _RfLVNCServer {
	RfVNCServer parent_instance;
	RfConfig *config;
	GSocketService *service;
	GByteArray *buf;
	GIOCondition io_flags;
	rfbScreenInfo *screen;
	unsigned int clients;
	char *passwords[2];
	char *desktop_name;
	unsigned int width;
	unsigned int height;
	bool running;
};
G_DEFINE_TYPE(RfLVNCServer, rf_lvnc_server, RF_TYPE_VNC_SERVER)

static int _on_socket_in(GSocket *socket, GIOCondition condition, void *data)
{
	RfLVNCServer *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	if (rfbIsActive(this->screen) && this->buf != NULL)
		rfbProcessEvents(this->screen, 0);

	return G_SOURCE_CONTINUE;
}

static void _on_client_gone(rfbClientRec *client)
{
	RfLVNCServer *this = client->screen->screenData;
	GSource *source = client->clientData;
	g_source_destroy(source);
	g_source_unref(source);
	if (this->clients-- == 1)
		rf_vnc_server_handle_last_client(RF_VNC_SERVER(this));
}

static enum rfbNewClientAction _on_new_client(rfbClientRec *client)
{
	client->clientGoneHook = _on_client_gone;
	return RFB_CLIENT_ACCEPT;
}

static int _on_set_desktop_size(
	int width,
	int height,
	int num_screens,
	struct rfbExtDesktopScreen *screens,
	rfbClientRec *client
)
{
	RfLVNCServer *this = client->screen->screenData;
	if (width != this->width || height != this->height)
		rf_vnc_server_handle_resize_event(
			RF_VNC_SERVER(this), width, height
		);
	return rfbExtDesktopSize_Success;
}

static void
_on_keysym_event(rfbBool direction, rfbKeySym keysym, rfbClientRec *client)
{
	RfLVNCServer *this = client->screen->screenData;
	bool down = direction != 0;
	rf_vnc_server_handle_keysym_event(RF_VNC_SERVER(this), keysym, down);
}

static void _on_pointer_event(int mask, int x, int y, rfbClientRec *client)
{
	RfLVNCServer *this = client->screen->screenData;
	const double rx = (double)x / this->width;
	const double ry = (double)y / this->height;
	rf_vnc_server_handle_pointer_event(RF_VNC_SERVER(this), rx, ry, mask);
}

static void _on_clipboard_text(char *text, int length, rfbClientRec *client)
{
	RfLVNCServer *this = client->screen->screenData;
	rf_vnc_server_handle_clipboard_text(RF_VNC_SERVER(this), text);
}

static int _on_incoming(
	GSocketService *service,
	GSocketConnection *connection,
	GObject *source_object,
	void *data
)
{
	RfLVNCServer *this = data;

	if (this->screen == NULL) {
		this->screen = rfbGetScreen(
			0,
			NULL,
			this->width,
			this->height,
			8,
			3,
			RF_BYTES_PER_PIXEL
		);
		this->screen->port = 0;
		this->screen->ipv6port = 0;
		this->screen->frameBuffer = NULL;
		if (this->desktop_name != NULL)
			this->screen->desktopName = this->desktop_name;
		else
			this->screen->desktopName = "ReFrame";
		this->screen->versionString = "ReFrame VNC Server";
		this->screen->screenData = this;
		this->screen->newClientHook = _on_new_client;
		this->screen->setDesktopSizeHook = _on_set_desktop_size;
		this->screen->ptrAddEvent = _on_pointer_event;
		this->screen->kbdAddEvent = _on_keysym_event;
		this->screen->setXCutText = _on_clipboard_text;
		this->screen->setXCutTextUTF8 = _on_clipboard_text;
		if (this->passwords[0] != NULL &&
		    this->passwords[0][0] != '\0') {
			this->screen->authPasswdData = this->passwords;
			this->screen->passwordCheck = rfbCheckPasswordByList;
		}
		rfbInitServer(this->screen);
	}
	GSocket *socket = g_socket_connection_get_socket(connection);
	g_debug("VNC: Got new connection %p.", socket);
	if (++this->clients == 1)
		rf_vnc_server_handle_first_client(RF_VNC_SERVER(this));
	// Don't attach source on new client hook, because it may be called
	// before we set client data.
	GSource *source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(_on_socket_in), this, NULL);
	g_source_attach(source, NULL);
	const int fd = g_socket_get_fd(socket);
	// `rfbClient` owns fd, but we got it from `GSocketConnection`.
	rfbClientRec *client = rfbNewClient(this->screen, dup(fd));
	client->clientData = source;
	// Just in case client disconnects very soon.
	if (client->sock == -1) {
		_on_client_gone(client);
		return false;
	}

	return true;
}

static void _dispose(GObject *o)
{
	RfLVNCServer *this = RF_LVNC_SERVER(o);

	rf_vnc_server_stop(RF_VNC_SERVER(this));

	G_OBJECT_CLASS(rf_lvnc_server_parent_class)->dispose(o);
}

static void _finalize(GObject *o)
{
	RfLVNCServer *this = RF_LVNC_SERVER(o);

	g_clear_pointer(&this->screen, rfbScreenCleanup);

	G_OBJECT_CLASS(rf_lvnc_server_parent_class)->finalize(o);
}

static void _start(RfVNCServer *super)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	if (this->running)
		return;

	this->passwords[0] = rf_config_get_vnc_password(this->config);
	this->desktop_name = rf_config_get_connector(this->config);
	this->width = rf_config_get_default_width(this->config);
	this->height = rf_config_get_default_height(this->config);
	if (this->width == 0 || this->height == 0) {
		// `0x0` is not a valid size for VNC, so we set a initial value
		// here and wait until it resizes to first frame size in main.
		// Remember this is only for making VNC happy and we keep `0x0`
		// in main so it knows we need to resize later.
		this->width = 800;
		this->height = 600;
	} else {
		g_message(
			"VNC: Got default width %u and height %u.",
			this->width,
			this->height
		);
	}

	g_autoptr(GError) error = NULL;
	const unsigned int port = rf_config_get_vnc_port(this->config);
	g_message("VNC: Listening on port %u.", port);
	this->service = g_socket_service_new();
	g_socket_listener_add_inet_port(
		G_SOCKET_LISTENER(this->service), port, NULL, &error
	);
	if (error != NULL)
		g_error("Failed to listen on port %u: %s.",
			port,
			error->message);
	g_signal_connect(
		this->service, "incoming", G_CALLBACK(_on_incoming), this
	);

	this->running = true;
}

static bool _is_running(RfVNCServer *super)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	return this->running;
}

static void _stop(RfVNCServer *super)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	if (!this->running)
		return;

	this->running = false;

	rf_vnc_server_flush(super);
	this->clients = 0;
	// This must be called before close the listener.
	//
	// See <https://docs.gtk.org/gio/method.SocketService.stop.html#description>.
	g_socket_service_stop(this->service);
	g_socket_listener_close(G_SOCKET_LISTENER(this->service));
	g_clear_object(&this->service);
	g_clear_pointer(&this->buf, g_byte_array_unref);
	g_clear_pointer(&this->desktop_name, g_free);
	g_clear_pointer(&this->passwords[0], g_free);
}

static void _set_desktop_name(RfVNCServer *super, const char *desktop_name)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	g_clear_pointer(&this->desktop_name, g_free);
	this->desktop_name = g_strdup(desktop_name);
	// Well this does not work because VNC does not update desktop name to
	// client after it is inited. Clients will get the previous desktop name
	// which may not be correct.
	// if (this->screen != NULL) {
	// 	this->screen->desktopName = this->desktop_name;
	// 	if (this->running && rfbIsActive(this->screen))
	// 		rfbProcessEvents(this->screen, 0);
	// }
}

static void _send_clipboard_text(RfVNCServer *super, const char *text)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	if (!this->running || this->screen == NULL ||
	    !rfbIsActive(this->screen))
		return;

	g_autofree char *ustr = g_strdup(text);
	g_autofree char *fstr = g_str_to_ascii(text, "C");
	if (fstr == NULL) {
		g_warning("Failed to convert UTF-8 to Latin 1.");
		rfbSendServerCutTextUTF8(
			this->screen, ustr, strlen(ustr) + 1, NULL, 0
		);
	} else {
		rfbSendServerCutTextUTF8(
			this->screen,
			ustr,
			strlen(ustr) + 1,
			fstr,
			strlen(fstr) + 1
		);
	}
	rfbProcessEvents(this->screen, 0);
}

static void
_update(RfVNCServer *super,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	if (!this->running || this->screen == NULL ||
	    !rfbIsActive(this->screen))
		return;

	if (this->buf != buf || this->width != width ||
	    this->height != height) {
		if (this->buf != buf) {
			g_clear_pointer(&this->buf, g_byte_array_unref);
			this->buf = g_byte_array_ref(buf);
		}
		if (this->width != width || this->height != height) {
			this->width = width;
			this->height = height;
		}
		rfbNewFramebuffer(
			this->screen,
			(char *)this->buf->data,
			this->width,
			this->height,
			8,
			3,
			RF_BYTES_PER_PIXEL
		);
	}

	if (damage != NULL)
		rfbMarkRectAsModified(
			this->screen,
			damage->x,
			damage->y,
			damage->x + damage->w,
			damage->y + damage->h
		);
	else
		rfbMarkRectAsModified(
			this->screen, 0, 0, this->width, this->height
		);
	rfbProcessEvents(this->screen, 0);
}

static void _flush(RfVNCServer *super)
{
	RfLVNCServer *this = RF_LVNC_SERVER(super);

	if (!this->running || this->screen == NULL ||
	    !rfbIsActive(this->screen))
		return;

	rfbClientIteratorPtr it = rfbGetClientIterator(this->screen);
	rfbClientRec *cl;
	while ((cl = rfbClientIteratorNext(it)))
		rfbCloseClient(cl);
	rfbReleaseClientIterator(it);
	rfbProcessEvents(this->screen, 0);
}

static void rf_lvnc_server_class_init(RfLVNCServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);
	RfVNCServerClass *v_class = RF_VNC_SERVER_CLASS(klass);

	o_class->dispose = _dispose;
	o_class->finalize = _finalize;

	v_class->start = _start;
	v_class->is_running = _is_running;
	v_class->stop = _stop;
	v_class->set_desktop_name = _set_desktop_name;
	v_class->send_clipboard_text = _send_clipboard_text;
	v_class->update = _update;
	v_class->flush = _flush;
}

static void rf_lvnc_server_init(RfLVNCServer *this)
{
	this->config = NULL;
	this->service = NULL;
	this->buf = NULL;
	this->io_flags = G_IO_IN | G_IO_PRI;
	this->screen = NULL;
	this->passwords[0] = NULL;
	this->passwords[1] = NULL;
	this->desktop_name = NULL;
	this->width = 0;
	this->height = 0;
	this->running = false;
}

G_MODULE_EXPORT RfVNCServer *rf_vnc_server_new(RfConfig *config)
{
	RfLVNCServer *this = g_object_new(RF_TYPE_LVNC_SERVER, NULL);
	this->config = config;
	return RF_VNC_SERVER(this);
}
