#include <stdbool.h>
#include <stdint.h>
#include <glib-unix.h>
#include <gio/gunixfdmessage.h>

#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-streamer.h"

struct _RfStreamer {
	GObject parent_instance;
	char *socket_path;
	GSocketClient *client;
	GSocketAddress *address;
	GSocketConnection *connection;
	GSocket *socket;
	bool running;
	unsigned int timer_id;
	int64_t last_frame_time;
	int64_t max_interval;
};
G_DEFINE_TYPE(RfStreamer, rf_streamer, G_TYPE_OBJECT)

enum { SIG_FRAME, N_SIGS };

static unsigned int sigs[N_SIGS] = { 0 };

static gboolean _send_ready(gpointer data)
{
	RfStreamer *this = data;
	char ready = 'R';

	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));
	g_output_stream_write(os, &ready, 1, NULL, NULL);
	g_debug("Sent ready to ReFrame streamer.");

	this->last_frame_time = g_get_monotonic_time();
	this->timer_id = 0;

	return G_SOURCE_REMOVE;
}

static void _schedule_ready(RfStreamer *this)
{
	if (this->timer_id != 0)
		return;

	int64_t current = g_get_monotonic_time();
	int64_t delta = current - this->last_frame_time;
	if (delta < this->max_interval) {
		this->timer_id = g_timeout_add(
			(this->max_interval - delta) / 1000, _send_ready, this);
	} else {
		g_debug("Converting frame too slow.");
		_send_ready(this);
	}
}

static gboolean _on_input(int sfd, GIOCondition condition, gpointer data)
{
	RfStreamer *this = data;
	RfBuffer b;
	GInputVector iov = { &b.md, sizeof(b.md) };
	GSocketControlMessage **msgs = NULL;
	int n_msgs = 0;
	ssize_t ret;

	ret = g_socket_receive_message(this->socket, NULL, &iov, 1, &msgs,
				       &n_msgs, NULL, NULL, NULL);
	if (ret == 0) {
		g_warning("ReFrame streamer disconnected.");
		return G_SOURCE_REMOVE;
	} else if (ret < 0) {
		g_error("Read socket failed.");
		return G_SOURCE_REMOVE;
	}

	for (int i = 0; i < RF_MAX_PLANES; ++i)
		b.fds[i] = -1;

	// We should only receive 1 message each time, but if we get many
	// messages, always use the latest one to reduce lag.
	for (int i = n_msgs - 1; i >= 0; --i) {
		if (G_IS_UNIX_FD_MESSAGE(msgs[i])) {
			GUnixFDMessage *msg = G_UNIX_FD_MESSAGE(msgs[i]);
			GUnixFDList *fds = g_unix_fd_message_get_fd_list(msg);
			for (int j = 0; j < b.md.length; ++j)
				b.fds[j] = g_unix_fd_list_get(fds, j, NULL);
			// We don't need to free GUnixFDList because
			// GUnixFDMessage does not return a reference so we
			// are not taking ownership of it.
			//
			// See <https://docs.gtk.org/gio-unix/type_func.FDMessage.get_fd_list.html>.
			break;
		}
	}
	for (int i = 0; i < n_msgs; ++i)
		g_object_unref(msgs[i]);
	g_free(msgs);

	g_debug("length: %u, width: %u, height: %u, fourcc: %c%c%c%c, modifier: %#lx",
		b.md.length, b.md.width, b.md.height, (b.md.fourcc >> 0) & 0xff,
		(b.md.fourcc >> 8) & 0xff, (b.md.fourcc >> 16) & 0xff,
		(b.md.fourcc >> 24) & 0xff, b.md.modifier);
	g_debug("fds: %d %d %d %d", b.fds[0], b.fds[1], b.fds[2], b.fds[3]);
	g_debug("offsets: %u %u %u %u", b.md.offsets[0], b.md.offsets[1],
		b.md.offsets[2], b.md.offsets[3]);
	g_debug("pitches: %u %u %u %u", b.md.pitches[0], b.md.pitches[1],
		b.md.pitches[2], b.md.pitches[3]);

	g_signal_emit(this, sigs[SIG_FRAME], 0, &b);
	for (int i = 0; i < b.md.length; ++i)
		close(b.fds[i]);
	_schedule_ready(this);
	return G_SOURCE_CONTINUE;
}

// static void _on_connected(GObject *source_object, GAsyncResult *res, gpointer data)
// {
// 	RfStreamer *s = data;
// 	s->connection = g_socket_client_connect_finish(s->client, res, NULL);
// 	// TODO: Error handling.
// 	s->running = true;
// 	s->socket = g_socket_connection_get_socket(s->connection);
// 	s->sfd = g_socket_get_fd(s->socket);
// 	g_unix_fd_add(s->sfd, G_IO_IN, _on_input, s);
// }

static void _dispose(GObject *o)
{
	RfStreamer *this = RF_STREAMER(o);

	rf_streamer_stop(this);

	G_OBJECT_CLASS(rf_streamer_parent_class)->dispose(o);
}

static void rf_streamer_init(RfStreamer *this)
{
	this->running = false;
}

static void rf_streamer_class_init(RfStreamerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->dispose = _dispose;

	sigs[SIG_FRAME] = g_signal_new("frame", RF_TYPE_STREAMER,
				       G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL,
				       G_TYPE_NONE, 1, RF_TYPE_BUFFER);
}

RfStreamer *rf_streamer_new(RfConfig *config)
{
	RfStreamer *this = g_object_new(RF_TYPE_STREAMER, NULL);
	this->max_interval = 1000000 / rf_config_get_fps(config);
	return this;
}

void rf_streamer_set_socket_path(RfStreamer *s, const char *socket_path)
{
	// g_clear_pointer(&s->socket_path, g_free);
	g_clear_object(&s->client);
	g_clear_object(&s->address);
	// s->socket_path = g_strdup(socket_path);
	s->client = g_socket_client_new();
	g_debug("socket path: %s.", socket_path);
	s->address = g_unix_socket_address_new(socket_path);
}

void rf_streamer_start(RfStreamer *s)
{
	if (s->running)
		return;

	// g_socket_client_connect_async(s->client, G_SOCKET_CONNECTABLE(s->address), NULL, _on_connected, s);
	// FIXME: Is it necessary to go async here?
	s->connection = g_socket_client_connect(
		s->client, G_SOCKET_CONNECTABLE(s->address), NULL, NULL);
	if (s->connection == NULL) {
		g_error("Error connecting to socket.\n");
		exit(EXIT_FAILURE);
	}
	// TODO: Error handling.
	s->running = true;
	s->socket = g_socket_connection_get_socket(s->connection);
	int sfd = g_socket_get_fd(s->socket);
	g_unix_fd_add(sfd, G_IO_IN, _on_input, s);
	_send_ready(s);
}

void rf_streamer_stop(RfStreamer *s)
{
	if (!s->running)
		return;

	g_io_stream_close(G_IO_STREAM(s->connection), NULL, NULL);
	s->running = false;
}
