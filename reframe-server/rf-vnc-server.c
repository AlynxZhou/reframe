#include <stdbool.h>
#include <rfb/rfb.h>
#include <xkbcommon/xkbcommon.h>

#include "rf-common.h"
#include "rf-vnc-server.h"

#define KEY_CODE_XKB_TO_EV(key_code) ((key_code) - 8)

struct _RfVNCServer {
	GSocketService parent_instance;
	RfConfig *config;
	rfbScreenInfo *screen;
	GHashTable *connections;
	GIOCondition io_flags;
	unsigned int width;
	unsigned int height;
	char *buf;
	char *passwords[2];
	char *connector;
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	bool running;
};
G_DEFINE_TYPE(RfVNCServer, rf_vnc_server, G_TYPE_SOCKET_SERVICE)

enum { SIG_FIRST_CLIENT, SIG_LAST_CLIENT, SIG_RESIZE_EVENT, SIG_KEYBOARD_EVENT, SIG_POINTER_EVENT, N_SIGS };

static unsigned int sigs[N_SIGS] = { 0 };

struct _iterate_data {
	xkb_keysym_t keysym;
	xkb_keycode_t keycode;
	xkb_level_index_t level;
};

static void _iterate_keys(struct xkb_keymap *map, xkb_keycode_t key, void *data)
{
	struct _iterate_data *idata = data;
	if (idata->keycode != XKB_KEYCODE_INVALID)
		return;

	xkb_level_index_t num_levels = xkb_keymap_num_levels_for_key(map, key, 0);
	for (xkb_level_index_t i = 0; i < num_levels; ++i) {
		const xkb_keysym_t *syms;
		int num_syms = xkb_keymap_key_get_syms_by_level(map, key, 0, i, &syms);
		for (int k = 0; k < num_syms; ++k) {
			if (syms[k] == idata->keysym) {
				idata->keycode = key;
				idata->level = i;
				return;
			}
		}
	}
}

static void _on_keyboard_event(rfbBool direction, rfbKeySym keysym, rfbClientRec *client)
{
	RfVNCServer *this = client->screen->screenData;

	bool down = direction != 0;
	struct _iterate_data idata = {
		.keysym = keysym,
		.keycode = XKB_KEYCODE_INVALID,
		.level = 0,
	};
	xkb_keymap_key_for_each(this->xkb_keymap, _iterate_keys, &idata);
	if (idata.keycode == XKB_KEYCODE_INVALID) {
		g_warning("Failed to find keysym %04x in keymap, will ignore it.", keysym);
		return;
	}
	uint32_t keycode = KEY_CODE_XKB_TO_EV(idata.keycode);
	g_debug("Input: Received key %s for keysym %04x and keycode %u.", down ? "down" : "up", keysym, keycode);
	g_signal_emit(this, sigs[SIG_KEYBOARD_EVENT], 0, keycode, down);
}

static inline char *_true_or_false(bool b)
{
	return b ? "true" : "false";
}

static void _on_pointer_event(int mask, int x, int y, rfbClientRec *client)
{
	RfVNCServer *this = client->screen->screenData;

	bool left = mask & 1;
	bool middle = mask & (1 << 1);
	bool right = mask & (1 << 2);
	bool wup = mask & (1 << 3);
	bool wdown = mask & (1 << 4);
	double rx = (double)x / this->width;
	double ry = (double)y / this->height;
	g_debug("Input: Received pointer at x %f and y %f, left %s, middle %s, right %s, wheel up %s, wheel down %s", rx, ry, _true_or_false(left), _true_or_false(middle), _true_or_false(right), _true_or_false(wup), _true_or_false(wdown));
	g_signal_emit(this, sigs[SIG_POINTER_EVENT], 0, rx, ry, left, middle, right, wup, wdown);
}

static gboolean _on_socket_io(GSocket *socket, GIOCondition condition, gpointer data)
{
	RfVNCServer *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	// Ensure there is no other entry to call processing events except this!
	// Especially don't call processing events while processing events!
	// Otherwise we may get last client disconnection and release resources
	// while processing other events and leads into segmentation fault.
	if (rfbIsActive(this->screen))
		rfbProcessEvents(this->screen, 0);
	return G_SOURCE_CONTINUE;
}

static void _attach_source(RfVNCServer *this, GSocketConnection *connection)
{
	GSocket *socket = g_socket_connection_get_socket(connection);
	GSource *source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(_on_socket_io), this, NULL);
	g_source_attach(source, NULL);
	g_hash_table_insert(this->connections, connection, source);
}

static gboolean _close_client(GSocketConnection *connection, GSource *source)
{
	g_autoptr(GError) error = NULL;
	g_source_destroy(source);
	g_io_stream_close(G_IO_STREAM(connection), NULL, &error);
	// We can do nothing here.
	if (error != NULL)
		g_warning("Failed to close client connection: %s.", error->message);
	return TRUE;
}

static void _detach_source(RfVNCServer *this, GSocketConnection *connection)
{
	g_autoptr(GError) error = NULL;
	GSource *source = g_hash_table_lookup(this->connections, connection);
	if (source != NULL) {
		_close_client(connection, source);
		g_hash_table_remove(this->connections, connection);
	}
}

static void _on_client_gone(rfbClientRec *client)
{
	RfVNCServer *this = client->screen->screenData;
	GSocketConnection *connection = client->clientData;
	if (g_hash_table_size(this->connections) == 1)
		g_signal_emit(this, sigs[SIG_LAST_CLIENT], 0);
	_detach_source(this, connection);
}

static enum rfbNewClientAction _on_new_client(rfbClientRec *client)
{
	client->clientGoneHook = _on_client_gone;
	return RFB_CLIENT_ACCEPT;
}

// FIXME: The initial client size is not handled.
static int _on_set_desktop_size(int width, int height, int num_screens,
				struct rfbExtDesktopScreen *screens,
				rfbClientRec *client)
{
	RfVNCServer *this = client->screen->screenData;
	if (width != this->width || height != this->height) {
		this->width = width;
		this->height = height;
		g_free(this->buf);
		this->buf = g_malloc0(this->width * this->height *
				      RF_BYTES_PER_PIXEL);
		rfbNewFramebuffer(this->screen, this->buf, this->width,
				  this->height, 8, 3, RF_BYTES_PER_PIXEL);
		g_signal_emit(this, sigs[SIG_RESIZE_EVENT], 0, width, height);
	}
	return rfbExtDesktopSize_Success;
}

static gboolean _incoming(GSocketService *service,
			  GSocketConnection *connection, GObject *source_object)
{
	RfVNCServer *this = RF_VNC_SERVER(service);
	if (this->screen == NULL) {
		this->screen = rfbGetScreen(0, NULL, this->width, this->height,
					    8, 3, RF_BYTES_PER_PIXEL);
		this->screen->port = 0;
		this->buf = g_malloc0(this->width * this->height *
				      RF_BYTES_PER_PIXEL);
		this->screen->frameBuffer = this->buf;
		this->screen->desktopName = this->connector;
		this->screen->versionString = "ReFrame VNC Server";
		this->screen->screenData = this;
		this->screen->newClientHook = _on_new_client;
		this->screen->setDesktopSizeHook = _on_set_desktop_size;
		// TODO: Clipboard event.
		this->screen->ptrAddEvent = _on_pointer_event;
		this->screen->kbdAddEvent = _on_keyboard_event;
		if (this->passwords[0] != NULL && strlen(this->passwords[0]) != 0) {
			this->screen->authPasswdData = this->passwords;
			this->screen->passwordCheck = rfbCheckPasswordByList;
		}
		rfbInitServer(this->screen);
	}
	GSocket *socket = g_socket_connection_get_socket(connection);
	int fd = g_socket_get_fd(socket);
	g_debug("VNC: New connection socket fd: %d.", fd);
	// `rfbClient` owns fd, but we got it from `GSocketConnection`.
	rfbClientRec *client = rfbNewClient(this->screen, dup(fd));
	client->clientData = g_object_ref(connection);
	// Don't attach source on new client hook, because it may be called
	// before we set client data.
	_attach_source(this, connection);
	if (g_hash_table_size(this->connections) == 1)
		g_signal_emit(this, sigs[SIG_FIRST_CLIENT], 0);
	// Just in case client disconnects very soon.
	if (client->sock == -1)
		_on_client_gone(client);

	return G_SOCKET_SERVICE_CLASS(rf_vnc_server_parent_class)
		->incoming(service, connection, source_object);
}

static void _dispose(GObject *o)
{
	RfVNCServer *this = RF_VNC_SERVER(o);

	rf_vnc_server_stop(this);

	G_OBJECT_CLASS(rf_vnc_server_parent_class)->dispose(o);
}

static void _finalize(GObject *o)
{
	RfVNCServer *this = RF_VNC_SERVER(o);

	g_clear_pointer(&this->buf, g_free);
	g_clear_pointer(&this->passwords[0], g_free);
	g_clear_pointer(&this->connector, g_free);
	g_clear_pointer(&this->screen, rfbScreenCleanup);
	g_clear_pointer(&this->connections, g_hash_table_unref);
	g_clear_pointer(&this->xkb_keymap, xkb_keymap_unref);
	g_clear_pointer(&this->xkb_context, xkb_context_unref);

	G_OBJECT_CLASS(rf_vnc_server_parent_class)->finalize(o);
}

static void rf_vnc_server_init(RfVNCServer *this)
{
	this->running = false;
	this->screen = NULL;
	this->passwords[0] = NULL;
	this->passwords[1] = NULL;
	this->connector = NULL;
	this->width = RF_DEFAULT_WIDTH;
	this->height = RF_DEFAULT_HEIGHT;
	// Updating buffer to VNC clients requires to call processing events,
	// otherwise the screen content won't be updated, however we cannot call
	// processing events in `rf_vnc_server_update()` (for details read the
	// comments in `_on_socket_io()`), so we also run callback here for
	// output to also processing events for updating buffer.
	this->io_flags = G_IO_IN | G_IO_PRI | G_IO_OUT;
	this->connections = g_hash_table_new_full(
		g_direct_hash, g_direct_equal, g_object_unref, (GDestroyNotify)g_source_unref);
	this->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (this->xkb_context == NULL)
		g_error("Failed to create XKB context.");
	struct xkb_rule_names names = {NULL, NULL, NULL, NULL, NULL};
	this->xkb_keymap = xkb_keymap_new_from_names(
		this->xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (this->xkb_keymap == NULL)
		g_error("Failed to create XKB context.");
}

static void rf_vnc_server_class_init(RfVNCServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);
	GSocketServiceClass *s_class = G_SOCKET_SERVICE_CLASS(klass);

	s_class->incoming = _incoming;

	o_class->dispose = _dispose;
	o_class->finalize = _finalize;

	sigs[SIG_FIRST_CLIENT] = g_signal_new("first-client",
					      RF_TYPE_VNC_SERVER,
					      0, 0, NULL, NULL,
					      NULL, G_TYPE_NONE, 0);
	sigs[SIG_LAST_CLIENT] = g_signal_new("last-client", RF_TYPE_VNC_SERVER,
					     0, 0, NULL, NULL,
					     NULL, G_TYPE_NONE, 0);
	sigs[SIG_RESIZE_EVENT] = g_signal_new(
		"resize-event", RF_TYPE_VNC_SERVER, 0, 0, NULL,
		NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
	sigs[SIG_KEYBOARD_EVENT] = g_signal_new(
		"keyboard-event", RF_TYPE_VNC_SERVER, 0, 0, NULL,
		NULL, NULL, G_TYPE_NONE, 2, G_TYPE_UINT, G_TYPE_BOOLEAN);
	sigs[SIG_POINTER_EVENT] = g_signal_new(
		"pointer-event", RF_TYPE_VNC_SERVER, 0, 0, NULL,
		NULL, NULL, G_TYPE_NONE, 7, G_TYPE_DOUBLE, G_TYPE_DOUBLE, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN, G_TYPE_BOOLEAN);
}

RfVNCServer *rf_vnc_server_new(RfConfig *config)
{
	RfVNCServer *this = g_object_new(RF_TYPE_VNC_SERVER, NULL);
	this->config = config;
	this->passwords[0] = rf_config_get_password(this->config);
	this->connector = rf_config_get_connector(this->config);
	return this;
}

void rf_vnc_server_start(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (this->running)
		return;

	g_autoptr(GError) error = NULL;
	unsigned int port = rf_config_get_port(this->config);
	g_debug("VNC: Listening on port %u.", port);
	g_socket_listener_add_inet_port(G_SOCKET_LISTENER(this), port, NULL,
					&error);
	if (error != NULL)
		g_error("Failed to listen on port %u: %s.", port, error->message);

	this->running = true;
}

void rf_vnc_server_stop(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (!this->running)
		return;

	this->running = false;

	rf_vnc_server_flush(this);
	g_socket_listener_close(G_SOCKET_LISTENER(this));
}

void rf_vnc_server_update(RfVNCServer *this, const unsigned char *buf)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (buf == NULL || !this->running || this->screen == NULL || !rfbIsActive(this->screen))
		return;

	memcpy(this->buf, buf, this->width * this->height * RF_BYTES_PER_PIXEL);
	rfbMarkRectAsModified(this->screen, 0, 0, this->width, this->height);
}

void rf_vnc_server_flush(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	g_signal_emit(this, sigs[SIG_LAST_CLIENT], 0);
	g_hash_table_foreach_remove(this->connections, (GHRFunc)_close_client, NULL);
}
