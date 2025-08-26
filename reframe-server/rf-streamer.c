#include <math.h>
#include <stdint.h>
#include <stdbool.h>
#include <gio/gunixfdmessage.h>
#include <linux/uinput.h>

#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-streamer.h"

struct _RfStreamer {
	GObject parent_instance;
	GSocketClient *client;
	GSocketAddress *address;
	GSocketConnection *connection;
	GSource *source;
	bool running;
	unsigned int timer_id;
	int64_t last_frame_time;
	int64_t max_interval;
};
G_DEFINE_TYPE(RfStreamer, rf_streamer, G_TYPE_OBJECT)

enum { SIG_FRAME, N_SIGS };

static unsigned int sigs[N_SIGS] = { 0 };

static void _send_input_request(RfStreamer *this, struct input_event *ies, const size_t length)
{
	char request = RF_REQUEST_TYPE_INPUT;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));
	ssize_t ret = 0;

	ret = g_output_stream_write(os, &request, sizeof(request), NULL, NULL);
	if (ret == 0) {
		goto disconnected;
	} else if (ret < 0) {
		g_warning("Failed to send input request to socket.");
		goto stop;
	}

	ret = g_output_stream_write(os, &length, sizeof(length), NULL, NULL);
	if (ret == 0) {
		goto disconnected;
	} else if (ret < 0) {
		g_warning("Failed to send input events length to socket.");
		goto stop;
	}

	ret = g_output_stream_write(os, ies, length * sizeof(*ies), NULL, NULL);
	if (ret == 0) {
		goto disconnected;
	} else if (ret < 0) {
		g_warning("Failed to send %lu * %ld bytes input events to socket.", length, sizeof(*ies));
		goto stop;
	}

	g_debug("Input: Sent %ld * %ld bytes input request.", length, sizeof(*ies));

	return;

disconnected:
	g_warning("ReFrame streamer disconnected.");
stop:
	rf_streamer_stop(this);
}

static gboolean _send_frame_request(gpointer data)
{
	RfStreamer *this = data;
	char request = RF_REQUEST_TYPE_FRAME;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));
	ssize_t ret = 0;

	ret = g_output_stream_write(os, &request, sizeof(request), NULL, NULL);
	if (ret == 0) {
		g_warning("ReFrame streamer disconnected.");
		goto stop;
	} else if (ret < 0) {
		g_warning("Failed to send frame request to socket.");
		goto stop;
	}

	g_debug("Frame: Sent frame request.");

	this->last_frame_time = g_get_monotonic_time();
	this->timer_id = 0;

	return G_SOURCE_REMOVE;

stop:
	rf_streamer_stop(this);
	return G_SOURCE_REMOVE;
}

static void _schedule_frame_request(RfStreamer *this)
{
	if (this->timer_id != 0)
		return;

	int64_t current = g_get_monotonic_time();
	int64_t delta = current - this->last_frame_time;
	if (delta < this->max_interval) {
		this->timer_id = g_timeout_add(
			(this->max_interval - delta) / 1000, _send_frame_request, this);
	} else {
		g_warning("Converting frame too slow.");
		_send_frame_request(this);
	}
}

static gboolean _on_socket_in(GSocket *socket, GIOCondition condition, gpointer data)
{
	RfStreamer *this = data;
	RfBuffer b;
	GInputVector iov = { &b.md, sizeof(b.md) };
	GSocketControlMessage **msgs = NULL;
	int n_msgs = 0;
	ssize_t ret;

	ret = g_socket_receive_message(socket, NULL, &iov, 1, &msgs,
				       &n_msgs, NULL, NULL, NULL);
	if (ret == 0) {
		g_warning("ReFrame streamer disconnected.");
		goto stop;
	} else if (ret < 0) {
		g_warning("Failed to receive frame from socket.");
		goto stop;
	}

	for (int i = 0; i < RF_MAX_PLANES; ++i)
		b.fds[i] = -1;

	// We should only receive 1 message each time, but if we get many
	// messages, always use the latest one to reduce lag.
	for (int i = n_msgs - 1; i >= 0; --i) {
		if (G_IS_UNIX_FD_MESSAGE(msgs[i])) {
			GUnixFDMessage *msg = G_UNIX_FD_MESSAGE(msgs[i]);
			GUnixFDList *fds = g_unix_fd_message_get_fd_list(msg);
			for (int j = 0; j < b.md.length; ++j) {
				b.fds[j] = g_unix_fd_list_get(fds, j, NULL);
				// Some error happens.
				if (b.fds[j] == -1) {
					g_warning("Failed to receive frame fds from socket, only got %d of %d.", j, b.md.length);
					b.md.length = j;
					goto cleanup;
				}
			}
			// We don't need to free GUnixFDList because
			// GUnixFDMessage does not return a reference so we
			// are not taking ownership of it.
			//
			// See <https://docs.gtk.org/gio-unix/type_func.FDMessage.get_fd_list.html>.
			break;
		}
	}

	g_debug("Frame: Got frame metadata: length %u, width %u, height %u, fourcc %c%c%c%c, modifier %#lx.",
		b.md.length, b.md.width, b.md.height, (b.md.fourcc >> 0) & 0xff,
		(b.md.fourcc >> 8) & 0xff, (b.md.fourcc >> 16) & 0xff,
		(b.md.fourcc >> 24) & 0xff, b.md.modifier);
	g_debug("Frame: Got frame fds: %d %d %d %d.", b.fds[0], b.fds[1], b.fds[2], b.fds[3]);
	g_debug("Frame: Got frame offsets: %u %u %u %u.", b.md.offsets[0], b.md.offsets[1],
		b.md.offsets[2], b.md.offsets[3]);
	g_debug("Frame: Got frame pitches: %u %u %u %u.", b.md.pitches[0], b.md.pitches[1],
		b.md.pitches[2], b.md.pitches[3]);

	g_signal_emit(this, sigs[SIG_FRAME], 0, &b);

cleanup:
	for (int i = 0; i < b.md.length; ++i)
		close(b.fds[i]);

	for (int i = 0; i < n_msgs; ++i)
		g_object_unref(msgs[i]);
	g_free(msgs);

	_schedule_frame_request(this);
	return G_SOURCE_CONTINUE;

stop:
	rf_streamer_stop(this);
	return G_SOURCE_REMOVE;
}

static void _dispose(GObject *o)
{
	RfStreamer *this = RF_STREAMER(o);

	rf_streamer_stop(this);
	g_clear_object(&this->address);
	g_clear_object(&this->client);

	G_OBJECT_CLASS(rf_streamer_parent_class)->dispose(o);
}

static void rf_streamer_init(RfStreamer *this)
{
	this->running = false;
	this->source = NULL;
	this->last_frame_time = g_get_monotonic_time();
	this->timer_id = 0;
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

void rf_streamer_set_socket_path(RfStreamer *this, const char *socket_path)
{
	g_return_if_fail(RF_IS_STREAMER(this));
	g_return_if_fail(socket_path != NULL);

	g_clear_object(&this->address);
	g_clear_object(&this->client);
	this->client = g_socket_client_new();
	this->address = g_unix_socket_address_new(socket_path);
}

int rf_streamer_start(RfStreamer *this)
{
	g_return_val_if_fail(RF_IS_STREAMER(this), -1);

	if (this->running)
		return 0;

	this->connection = g_socket_client_connect(
		this->client, G_SOCKET_CONNECTABLE(this->address), NULL, NULL);
	if (this->connection == NULL) {
		g_warning("Failed to connecting to ReFrame streamer.");
		return -2;
	}
	GSocket *socket = g_socket_connection_get_socket(this->connection);
	this->source = g_socket_create_source(socket, G_IO_IN, NULL);
	g_source_set_callback(this->source, G_SOURCE_FUNC(_on_socket_in), this, NULL);
	g_source_attach(this->source, NULL);
	_send_frame_request(this);

	this->running = true;
	return 0;
}

void rf_streamer_stop(RfStreamer *this)
{
	g_return_if_fail(RF_IS_STREAMER(this));

	g_autoptr(GError) error = NULL;

	if (!this->running)
		return;

	this->running = false;

	if (this->source != NULL) {
		g_source_destroy(this->source);
		g_clear_pointer(&this->source, g_source_unref);
	}
	if (this->timer_id != 0) {
		g_source_remove(this->timer_id);
		this->timer_id = 0;
	}
	g_io_stream_close(G_IO_STREAM(this->connection), NULL, &error);
	if (error != NULL) {
		g_warning("Failed to close ReFrame streamer connection: %s.", error->message);
		return;
	}
	g_clear_object(&this->connection);
}

void rf_streamer_send_keyboard_event(RfStreamer *this, uint32_t keycode, bool down)
{
	g_return_if_fail(RF_IS_STREAMER(this));
	g_return_if_fail(this->running);

#define KEYBOARD_EVENT_LENGTH 2
	struct input_event ies[KEYBOARD_EVENT_LENGTH] = {
		{
			.type = EV_KEY,
			.code = keycode,
			.value = down
		},
		{
			.type = EV_SYN,
			.code = SYN_REPORT,
			.value = 0
		}
	};
	_send_input_request(this, ies, KEYBOARD_EVENT_LENGTH);
}

void rf_streamer_send_pointer_event(RfStreamer *this, double rx, double ry, bool left, bool middle, bool right, bool wup, bool wdown)
{
	g_return_if_fail(RF_IS_STREAMER(this));
	g_return_if_fail(this->running);

	const int x = round(rx * RF_POINTER_MAX);
	const int y = round(ry * RF_POINTER_MAX);

	size_t length = (wup || wdown) ? 7 : 6;
	g_autofree struct input_event *ies = g_malloc0_n(length, sizeof(*ies));

	ies[0].type = EV_ABS;
	ies[0].code = ABS_X;
	ies[0].value = x;

	ies[1].type = EV_ABS;
	ies[1].code = ABS_Y;
	ies[1].value = y;

	ies[2].type = EV_KEY;
	ies[2].code = BTN_LEFT;
	ies[2].value = left;

	ies[3].type = EV_KEY;
	ies[3].code = BTN_MIDDLE;
	ies[3].value = middle;

	ies[4].type = EV_KEY;
	ies[4].code = BTN_RIGHT;
	ies[4].value = right;

	if (wup || wdown) {
		ies[5].type = EV_REL;
		ies[5].code = REL_WHEEL;
		ies[5].value = wup ? 1 : -1;
	}

	ies[length - 1].type = EV_SYN;
	ies[length - 1].code = SYN_REPORT;
	ies[length - 1].value = 0;

	_send_input_request(this, ies, length);
}
