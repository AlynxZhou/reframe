#include <stdbool.h>
#include <rfb/rfb.h>

#include "rf-common.h"
#include "rf-vnc-server.h"

struct _RfVNCServer {
	GSocketService parent_instance;
	RfConfig *config;
	rfbScreenInfo *screen;
	GHashTable *connections;
	unsigned int width;
	unsigned int height;
	char *buf;
	bool running;
};
G_DEFINE_TYPE(RfVNCServer, rf_vnc_server, G_TYPE_SOCKET_SERVICE)

enum { SIG_FIRST_CLIENT, SIG_LAST_CLIENT, SIG_SIZE_REQUEST, N_SIGS };

static unsigned int sigs[N_SIGS] = { 0 };

static gboolean _on_socket_input(GSocket *socket, GIOCondition condition,
				 gpointer data)
{
	RfVNCServer *this = data;
	if (condition & G_IO_IN)
		if (rfbIsActive(this->screen))
			rfbProcessEvents(this->screen, 0);
	return G_SOURCE_CONTINUE;
}

static void _attach_source(RfVNCServer *this, GSocketConnection *connection)
{
	GSocket *socket = g_socket_connection_get_socket(connection);
	GSource *source =
		g_socket_create_source(socket, G_IO_IN | G_IO_PRI, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(_on_socket_input), this,
			      NULL);
	g_source_attach(source, NULL);
	g_hash_table_insert(this->connections, connection, source);
}

static void _detach_source(RfVNCServer *this, GSocketConnection *connection)
{
	GSource *source = g_hash_table_lookup(this->connections, connection);
	if (source != NULL) {
		g_source_destroy(source);
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

static int _on_set_desktop_size(int width, int height, int num_screens,
				struct rfbExtDesktopScreen *screens,
				rfbClientRec *client)
{
	RfVNCServer *this = client->screen->screenData;
	if (width != this->width || height != this->height) {
		this->width = width;
		this->height = height;
		g_free(this->buf);
		g_signal_emit(this, sigs[SIG_SIZE_REQUEST], 0, width, height);
		this->buf = g_malloc0(this->width * this->height *
				      RF_BYTES_PER_PIXEL);
		rfbNewFramebuffer(this->screen, this->buf, this->width,
				  this->height, 8, 3, RF_BYTES_PER_PIXEL);
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
		g_autofree char *connector_name =
			rf_config_get_connector(this->config);
		this->screen->desktopName = connector_name;
		this->screen->versionString = "ReFrame VNC Server";
		this->screen->screenData = this;
		this->screen->newClientHook = _on_new_client;
		this->screen->setDesktopSizeHook = _on_set_desktop_size;
		// TODO: Input event, clipboard event.
		// this->screen->ptrAddEvent = _on_pointer_event;
		// this->screen->kbdAddEvent = _on_key_event;
		// this->screen->kbdReleaseAllKeys = _on_release_all_keys;
		rfbInitServer(this->screen);
	}
	GSocket *socket = g_socket_connection_get_socket(connection);
	int fd = g_socket_get_fd(socket);
	g_debug("New VNC connection socket fd: %d.", fd);
	rfbClientRec *client = rfbNewClient(this->screen, fd);
	client->clientData = g_object_ref(connection);
	// Don't attach source on new client hook, because it may be called
	// before we set client data.
	_attach_source(this, connection);
	if (g_hash_table_size(this->connections) == 1)
		g_signal_emit(this, sigs[SIG_FIRST_CLIENT], 0);
	rfbProcessEvents(this->screen, 0);

	return G_SOCKET_SERVICE_CLASS(rf_vnc_server_parent_class)
		->incoming(service, connection, source_object);
}

static void rf_vnc_server_init(RfVNCServer *this)
{
	// this->screen = NULL;
	this->running = false;
	this->width = RF_DEFAULT_WIDTH;
	this->height = RF_DEFAULT_HEIGHT;
	this->connections = g_hash_table_new_full(
		g_direct_hash, g_direct_equal, g_object_unref, g_object_unref);
}

static void rf_vnc_server_class_init(RfVNCServerClass *klass)
{
	// GObjectClass *o_class = G_OBJECT_CLASS(klass);
	GSocketServiceClass *s_class = G_SOCKET_SERVICE_CLASS(klass);

	s_class->incoming = _incoming;

	// o_class->dispose = _dispose;
	// o_class->finalize = _finalize;

	sigs[SIG_FIRST_CLIENT] = g_signal_new("first-client",
					      RF_TYPE_VNC_SERVER,
					      G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					      NULL, G_TYPE_NONE, 0);
	sigs[SIG_LAST_CLIENT] = g_signal_new("last-client", RF_TYPE_VNC_SERVER,
					     G_SIGNAL_RUN_LAST, 0, NULL, NULL,
					     NULL, G_TYPE_NONE, 0);
	sigs[SIG_SIZE_REQUEST] = g_signal_new(
		"size-request", RF_TYPE_VNC_SERVER, G_SIGNAL_RUN_LAST, 0, NULL,
		NULL, NULL, G_TYPE_NONE, 2, G_TYPE_INT, G_TYPE_INT);
}

RfVNCServer *rf_vnc_server_new(RfConfig *config)
{
	RfVNCServer *this = g_object_new(RF_TYPE_VNC_SERVER, NULL);
	this->config = config;
	return this;
}

void rf_vnc_server_start(RfVNCServer *this)
{
	if (this->running)
		return;

	unsigned port = rf_config_get_port(this->config);
	g_debug("Listening on %u.", port);
	g_socket_listener_add_inet_port(G_SOCKET_LISTENER(this), port, NULL,
					NULL);
	this->running = true;
}

void rf_vnc_server_stop(RfVNCServer *this)
{
	if (!this->running)
		return;

	this->running = false;
	g_socket_listener_close(G_SOCKET_LISTENER(this));
	// TODO: Disconnect all client connections.
}

void rf_vnc_server_update(RfVNCServer *this, unsigned char *buf)
{
	if (this->screen == NULL || !rfbIsActive(this->screen))
		return;
	memcpy(this->buf, buf, this->width * this->height * RF_BYTES_PER_PIXEL);
	rfbMarkRectAsModified(this->screen, 0, 0, this->width, this->height);
	rfbProcessEvents(this->screen, 0);
}
