#include <stdbool.h>
#include <glib.h>
#include <glib-unix.h>

#include "rf-common.h"
#include "rf-session.h"

struct _RfSession {
	GSocketService parent_instance;
	GSocketAddress *address;
	GHashTable *sockets;
	GIOCondition io_flags;
	bool running;
};
G_DEFINE_TYPE(RfSession, rf_session, G_TYPE_SOCKET_SERVICE)

enum { SIG_START, SIG_STOP, SIG_CLIPBOARD_TEXT, N_SIGS };

static unsigned int sigs[N_SIGS] = { 0 };

static ssize_t
_on_clipboard_text_msg(RfSession *this, GSocketConnection *connection)
{
	g_autofree char *msg = NULL;
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0)
		goto out;

	msg = g_malloc0(length);
	ret = g_input_stream_read(is, msg, length, NULL, &error);
	if (ret <= 0)
		goto out;

	g_signal_emit(this, sigs[SIG_CLIPBOARD_TEXT], 0, msg);

out:
	if (ret < 0)
		g_warning(
			"Failed to receive clipboard text: %s.", error->message
		);
	else if (ret > 0)
		g_debug("Clipboard: Received text %s.", msg);
	return ret;
}

static int _on_socket_in(GSocket *socket, GIOCondition condition, void *data)
{
	RfSession *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	char type;
	g_autoptr(GSocketConnection) connection =
		g_socket_connection_factory_create_connection(socket);
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));
	ret = g_input_stream_read(is, &type, sizeof(type), NULL, &error);
	if (ret <= 0) {
		if (ret < 0)
			g_warning(
				"Failed to read message type: %s.",
				error->message
			);
		goto out;
	}

	switch (type) {
	case RF_MSG_TYPE_CLIPBOARD_TEXT:
		ret = _on_clipboard_text_msg(this, connection);
		break;
	default:
		break;
	}

out:
	if (ret <= 0) {
		if (ret == 0)
			g_message("ReFrame Session disconnected.");
		g_hash_table_remove(this->sockets, socket);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static int _incoming(
	GSocketService *service,
	GSocketConnection *connection,
	GObject *source_object
)
{
	RfSession *this = RF_SESSION(service);

	GSocket *socket = g_socket_connection_get_socket(connection);
	g_debug("Socket: Got new ReFrame Session %p.", socket);
	GSource *source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(_on_socket_in), this, NULL);
	g_source_attach(source, NULL);
	g_hash_table_insert(this->sockets, g_object_ref(socket), source);

	return G_SOCKET_SERVICE_CLASS(rf_session_parent_class)
		->incoming(service, connection, source_object);
}

static void _dispose(GObject *o)
{
	RfSession *this = RF_SESSION(o);

	rf_session_stop(this);
	g_clear_object(&this->address);

	G_OBJECT_CLASS(rf_session_parent_class)->dispose(o);
}

static void _finalize(GObject *o)
{
	RfSession *this = RF_SESSION(o);

	g_clear_pointer(&this->sockets, g_hash_table_unref);

	G_OBJECT_CLASS(rf_session_parent_class)->finalize(o);
}

static void rf_session_class_init(RfSessionClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);
	GSocketServiceClass *s_class = G_SOCKET_SERVICE_CLASS(klass);

	s_class->incoming = _incoming;

	o_class->dispose = _dispose;
	o_class->finalize = _finalize;

	sigs[SIG_START] = g_signal_new(
		"start", RF_TYPE_SESSION, 0, 0, NULL, NULL, NULL, G_TYPE_NONE, 0
	);
	sigs[SIG_STOP] = g_signal_new(
		"stop", RF_TYPE_SESSION, 0, 0, NULL, NULL, NULL, G_TYPE_NONE, 0
	);

	sigs[SIG_CLIPBOARD_TEXT] = g_signal_new(
		"clipboard-text",
		RF_TYPE_SESSION,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		1,
		G_TYPE_STRING
	);
}

static void rf_session_init(RfSession *this)
{
	this->address = NULL;
	this->sockets = g_hash_table_new_full(
		g_direct_hash,
		g_direct_equal,
		g_object_unref,
		(GDestroyNotify)g_source_unref
	);
	this->io_flags = G_IO_IN | G_IO_PRI;
	this->running = false;
}

RfSession *rf_session_new(void)
{
	RfSession *this = g_object_new(RF_TYPE_SESSION, NULL);
	return this;
}

void rf_session_set_socket_path(RfSession *this, const char *socket_path)
{
	g_return_if_fail(RF_IS_SESSION(this));
	g_return_if_fail(socket_path != NULL);

	g_clear_object(&this->address);
	g_remove(socket_path);
	this->address = g_unix_socket_address_new(socket_path);
}

int rf_session_start(RfSession *this)
{
	g_return_val_if_fail(RF_IS_SESSION(this), -1);

	if (this->running)
		return 0;

	g_autoptr(GError) error = NULL;
	g_socket_listener_add_address(
		G_SOCKET_LISTENER(this),
		this->address,
		G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_DEFAULT,
		NULL,
		NULL,
		&error
	);
	const char *socket_path = g_unix_socket_address_get_path(
		G_UNIX_SOCKET_ADDRESS(this->address)
	);
	rf_set_group(socket_path);
	g_chmod(socket_path, 0660);
	if (error != NULL) {
		g_warning(
			"Failed to listen to session socket: %s", error->message
		);
		return -2;
	}

	this->running = true;
	g_debug("Signal: Emitting ReFrame Session start signal.");
	g_signal_emit(this, sigs[SIG_START], 0);
	return 0;
}

bool rf_session_is_running(RfSession *this)
{
	g_return_val_if_fail(RF_IS_SESSION(this), false);

	return this->running;
}

void rf_session_stop(RfSession *this)
{
	g_return_if_fail(RF_IS_SESSION(this));

	if (!this->running)
		return;

	g_debug("Signal: Emitting ReFrame Session stop signal.");
	g_signal_emit(this, sigs[SIG_STOP], 0);
	this->running = false;

	GHashTableIter it;
	void *value;
	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, NULL, &value))
		g_source_destroy(value);
	g_hash_table_remove_all(this->sockets);
	g_socket_listener_close(G_SOCKET_LISTENER(this));
}

void rf_session_send_clipboard_text_msg(RfSession *this, const char *text)
{
	g_return_if_fail(RF_IS_SESSION(this));
	g_return_if_fail(text != NULL);

	if (!this->running)
		return;

	size_t length = strlen(text) + 1;
	GHashTableIter it;
	void *key;
	void *value;
	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		ssize_t ret = 0;
		g_autoptr(GError) error = NULL;
		GSocketConnection *connection =
			g_socket_connection_factory_create_connection(key);
		GOutputStream *os =
			g_io_stream_get_output_stream(G_IO_STREAM(connection));
		ret = rf_send_header(
			connection, RF_MSG_TYPE_CLIPBOARD_TEXT, length, &error
		);
		if (ret <= 0)
			goto next;
		ret = g_output_stream_write(os, text, length, NULL, &error);
	next:
		if (ret <= 0) {
			if (ret < 0)
				g_warning(
					"Failed to send clipboard text: %s.",
					error->message
				);
			else
				g_message("ReFrame Session disconnected.");
			g_source_destroy(value);
			g_hash_table_iter_remove(&it);
		}
	}
}
