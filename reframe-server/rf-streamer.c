#include <stdint.h>
#include <stdbool.h>
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <linux/uinput.h>

#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-streamer.h"

#define KEYBOARD_MAX_EVENTS 2
#define POINTER_MAX_EVENTS 10

struct _RfStreamer {
	GObject parent_instance;
	RfConfig *config;
	GSocketClient *client;
	GSocketAddress *address;
	GSocketConnection *connection;
	GIOCondition io_flags;
	GSource *source;
	unsigned int timer_id;
	int64_t last_frame_time;
	int64_t max_interval;
	unsigned int desktop_width;
	unsigned int desktop_height;
	int monitor_x;
	int monitor_y;
	unsigned int rotation;
	// These are the real size of monitor and have nothing with VNC.
	uint32_t frame_width;
	uint32_t frame_height;
	bool running;
};
G_DEFINE_TYPE(RfStreamer, rf_streamer, G_TYPE_OBJECT)

enum {
	SIG_START,
	SIG_STOP,
	SIG_FRAME,
	SIG_CARD_PATH,
	SIG_CONNECTOR_NAME,
	N_SIGS
};

static unsigned int sigs[N_SIGS] = { 0 };

static void
_send_input_msg(RfStreamer *this, struct input_event *ies, const size_t length)
{
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));

	ret = rf_send_header(
		this->connection, RF_MSG_TYPE_INPUT, length, &error
	);
	if (ret <= 0)
		goto out;
	ret = g_output_stream_write(
		os, ies, length * sizeof(*ies), NULL, &error
	);

out:
	if (ret < 0) {
		g_warning(
			"Input: Failed to send input events: %s.",
			error->message
		);
		rf_streamer_stop(this);
	} else if (ret > 0) {
		g_debug("Input: Sent %ld * %ld bytes input events.",
			length,
			sizeof(*ies));
	} else {
		g_warning("ReFrame Streamer disconnected.");
		rf_streamer_stop(this);
	}
}

static gboolean _send_frame_msg(gpointer data)
{
	RfStreamer *this = data;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	ret = rf_send_header(this->connection, RF_MSG_TYPE_FRAME, 0, &error);
	if (ret < 0) {
		g_warning(
			"Frame: Failed to send frame message: %s.",
			error->message
		);
		rf_streamer_stop(this);
	} else if (ret > 0) {
		this->last_frame_time = g_get_monotonic_time();
		this->timer_id = 0;
	} else {
		g_warning("ReFrame Streamer disconnected.");
		rf_streamer_stop(this);
	}
	return G_SOURCE_REMOVE;
}

static void _schedule_frame_msg(RfStreamer *this)
{
	if (this->timer_id != 0)
		return;

	int64_t current = g_get_monotonic_time();
	int64_t delta = current - this->last_frame_time;
	// Give this highest priority to prevent lag.
	if (delta < this->max_interval) {
		this->timer_id = g_timeout_add_full(
			G_PRIORITY_HIGH,
			(this->max_interval - delta) / 1000,
			_send_frame_msg,
			this,
			NULL
		);
	} else {
		if (this->last_frame_time != -1)
			g_warning(
				"Frame: Converted frame too slow, expected %ldms, used %ldms.",
				this->max_interval / 1000,
				delta / 1000
			);
		this->timer_id = g_timeout_add_full(
			G_PRIORITY_HIGH, 1, _send_frame_msg, this, NULL
		);
	}
}

static ssize_t
_on_buffer(GSocketConnection *connection, RfBuffer *b, GError **error)
{
	ssize_t ret = 0;
	GSocket *socket = g_socket_connection_get_socket(connection);
	GInputVector iov = { &b->md, sizeof(b->md) };
	g_autofree GSocketControlMessage **msgs = NULL;
	int n_msgs = 0;

	b->md.length = 0;
	for (int i = 0; i < RF_MAX_FDS; ++i)
		b->fds[i] = -1;

	ret = g_socket_receive_message(
		socket, NULL, &iov, 1, &msgs, &n_msgs, NULL, NULL, error
	);
	if (ret <= 0)
		return ret;

	// We should only receive 1 message each time.
	if (n_msgs != 1) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_DATA,
			"Expect 1 fd message but got %d",
			n_msgs
		);
		ret = -2;
		goto out;
	}

	if (!G_IS_UNIX_FD_MESSAGE(msgs[n_msgs - 1])) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_DATA,
			"Failed to get fd message"
		);
		ret = -2;
		goto out;
	}

	// We don't need to free GUnixFDList because
	// GUnixFDMessage does not return a reference so we
	// are not taking ownership of it.
	//
	// See <https://docs.gtk.org/gio-unix/type_func.FDMessage.get_fd_list.html>.
	GUnixFDMessage *msg = G_UNIX_FD_MESSAGE(msgs[n_msgs - 1]);
	GUnixFDList *fds = g_unix_fd_message_get_fd_list(msg);
	for (int i = 0; i < b->md.length; ++i) {
		b->fds[i] = g_unix_fd_list_get(fds, i, NULL);
		// Some error happens.
		if (b->fds[i] == -1) {
			g_set_error(
				error,
				G_IO_ERROR,
				G_IO_ERROR_INVALID_DATA,
				"Expect %d fds but got %d.",
				b->md.length,
				i
			);
			b->md.length = i;
			ret = -2;
			goto out;
		}
	}
	rf_buffer_debug(b);

out:
	for (int i = 0; i < n_msgs; ++i)
		g_object_unref(msgs[i]);

	return ret;
}

static ssize_t _on_frame_msg(RfStreamer *this)
{
	g_debug("Frame: Received frame message.");

	RfBuffer bufs[RF_MAX_BUFS];
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0) {
		length = 0;
		goto out;
	}
	// Empty buffer, maybe locked screen and turned monitor off, skip it.
	if (length == 0) {
		g_debug("Frame: Got empty buffer for primary plane.");
		goto out;
	} else if (length > RF_MAX_BUFS) {
		g_warning("Frame: Got invalid buffers length %ld.", length);
		goto out;
	}

	for (size_t i = 0; i < length; ++i) {
		ret = _on_buffer(this->connection, &bufs[i], &error);
		if (ret <= 0)
			goto out;
	}

	RfBuffer *primary = &bufs[0];
	// Monitor size should be CRTC size, and primary plane is used to store
	// CRTC's framebuffer, so primary plane size should be CRTC size.
	uint32_t frame_width = primary->md.crtc_w;
	uint32_t frame_height = primary->md.crtc_h;
	if (this->rotation % 180 != 0) {
		frame_width = primary->md.crtc_h;
		frame_height = primary->md.crtc_w;
	}
	if (this->frame_width != frame_width ||
	    this->frame_height != frame_height) {
		this->frame_width = frame_width;
		this->frame_height = frame_height;
	}

	g_signal_emit(this, sigs[SIG_FRAME], 0, length, bufs);

out:
	for (size_t i = 0; i < length; ++i)
		for (int j = 0; j < bufs[i].md.length; ++j)
			close(bufs[i].fds[j]);

	if (ret < 0)
		g_warning("Frame: Failed to receive frame: %s.", error->message);
	else if (ret > 0)
		_schedule_frame_msg(this);
	return ret;
}

static ssize_t _on_card_path_msg(RfStreamer *this)
{
	g_autofree char *msg = NULL;
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0)
		goto out;

	msg = g_malloc0(length);
	ret = g_input_stream_read(is, msg, length, NULL, &error);
	if (ret <= 0)
		goto out;

	g_signal_emit(this, sigs[SIG_CARD_PATH], 0, msg);

out:
	if (ret < 0)
		g_warning(
			"DRM: Failed to receive card path: %s.", error->message
		);
	else if (ret > 0)
		g_debug("DRM: Received card path %s.", msg);
	return ret;
}

static ssize_t _on_connector_name_msg(RfStreamer *this)
{
	g_autofree char *msg = NULL;
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0)
		goto out;

	msg = g_malloc0(length);
	ret = g_input_stream_read(is, msg, length, NULL, &error);
	if (ret <= 0)
		goto out;

	g_signal_emit(this, sigs[SIG_CONNECTOR_NAME], 0, msg);

out:
	if (ret < 0)
		g_warning(
			"DRM: Failed to receive connector name: %s.",
			error->message
		);
	else if (ret > 0)
		g_debug("DRM: Received connector name %s.", msg);
	return ret;
}

static gboolean
_on_socket_in(GSocket *socket, GIOCondition condition, gpointer data)
{
	RfStreamer *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));
	char type;
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
	case RF_MSG_TYPE_FRAME:
		ret = _on_frame_msg(this);
		break;
	case RF_MSG_TYPE_CARD_PATH:
		ret = _on_card_path_msg(this);
		break;
	case RF_MSG_TYPE_CONNECTOR_NAME:
		ret = _on_connector_name_msg(this);
		break;
	default:
		break;
	}

out:
	if (ret <= 0) {
		if (ret == 0)
			g_warning("ReFrame Streamer disconnected.");
		rf_streamer_stop(this);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void _dispose(GObject *o)
{
	RfStreamer *this = RF_STREAMER(o);

	rf_streamer_stop(this);
	g_clear_object(&this->address);
	g_clear_object(&this->client);

	G_OBJECT_CLASS(rf_streamer_parent_class)->dispose(o);
}

static void rf_streamer_class_init(RfStreamerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->dispose = _dispose;

	sigs[SIG_START] = g_signal_new(
		"start", RF_TYPE_STREAMER, 0, 0, NULL, NULL, NULL, G_TYPE_NONE, 0
	);
	sigs[SIG_STOP] = g_signal_new(
		"stop", RF_TYPE_STREAMER, 0, 0, NULL, NULL, NULL, G_TYPE_NONE, 0
	);
	sigs[SIG_FRAME] = g_signal_new(
		"frame",
		RF_TYPE_STREAMER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		2,
		G_TYPE_INT,
		G_TYPE_POINTER
	);
	sigs[SIG_CARD_PATH] = g_signal_new(
		"card-path",
		RF_TYPE_STREAMER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		1,
		G_TYPE_STRING
	);
	sigs[SIG_CONNECTOR_NAME] = g_signal_new(
		"connector-name",
		RF_TYPE_STREAMER,
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

static void rf_streamer_init(RfStreamer *this)
{
	this->config = NULL;
	this->client = NULL;
	this->address = NULL;
	this->connection = NULL;
	this->io_flags = G_IO_IN | G_IO_PRI;
	this->source = NULL;
	this->timer_id = 0;
	this->last_frame_time = -1;
	this->max_interval = 1000000 / 30;
	this->desktop_width = 0;
	this->desktop_height = 0;
	this->monitor_x = 0;
	this->monitor_y = 0;
	this->rotation = 0;
	this->frame_width = 0;
	this->frame_height = 0;
	this->running = false;
}

RfStreamer *rf_streamer_new(RfConfig *config)
{
	RfStreamer *this = g_object_new(RF_TYPE_STREAMER, NULL);
	this->config = config;
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

	this->last_frame_time = -1;
	unsigned int fps = rf_config_get_fps(this->config);
	this->max_interval = 1000000 / fps;
	g_message("Frame: Got FPS %u.", fps);
	this->desktop_width = rf_config_get_desktop_width(this->config);
	this->desktop_height = rf_config_get_desktop_height(this->config);
	g_message(
		"Input: Got desktop width %u and height %u.",
		this->desktop_width,
		this->desktop_height
	);
	this->monitor_x = rf_config_get_monitor_x(this->config);
	this->monitor_y = rf_config_get_monitor_y(this->config);
	g_message(
		"Input: Got monitor x %u and y %u.",
		this->monitor_x,
		this->monitor_y
	);
	this->rotation = rf_config_get_rotation(this->config);
	g_message("Frame: Got screen rotation %u.", this->rotation);
	this->frame_width = 0;
	this->frame_height = 0;

	g_autoptr(GError) error = NULL;
	this->connection = g_socket_client_connect(
		this->client, G_SOCKET_CONNECTABLE(this->address), NULL, &error
	);
	if (this->connection == NULL) {
		g_warning(
			"Failed to connecting to ReFrame Streamer: %s.",
			error->message
		);
		return -2;
	}
	GSocket *socket = g_socket_connection_get_socket(this->connection);
	this->source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(
		this->source, G_SOURCE_FUNC(_on_socket_in), this, NULL
	);
	g_source_attach(this->source, NULL);
	_schedule_frame_msg(this);

	this->running = true;
	g_debug("Emitting ReFrame Streamer start signal.");
	g_signal_emit(this, sigs[SIG_START], 0);
	return 0;
}

bool rf_streamer_is_running(RfStreamer *this)
{
	g_return_val_if_fail(RF_IS_STREAMER(this), false);

	return this->running;
}

void rf_streamer_stop(RfStreamer *this)
{
	g_return_if_fail(RF_IS_STREAMER(this));

	if (!this->running)
		return;

	g_debug("Emitting ReFrame Streamer stop signal.");
	g_signal_emit(this, sigs[SIG_STOP], 0);
	this->running = false;

	if (this->source != NULL) {
		g_source_destroy(this->source);
		g_clear_pointer(&this->source, g_source_unref);
	}
	if (this->timer_id != 0) {
		g_source_remove(this->timer_id);
		this->timer_id = 0;
	}
	g_autoptr(GError) error = NULL;
	g_io_stream_close(G_IO_STREAM(this->connection), NULL, &error);
	if (error != NULL) {
		g_warning(
			"Failed to close ReFrame Streamer connection: %s.",
			error->message
		);
		return;
	}
	g_clear_object(&this->connection);
}

void rf_streamer_send_keyboard_event(
	RfStreamer *this,
	uint32_t keycode,
	bool down
)
{
	g_return_if_fail(RF_IS_STREAMER(this));

	if (!this->running)
		return;

	struct input_event ies[KEYBOARD_MAX_EVENTS];
	memset(ies, 0, KEYBOARD_MAX_EVENTS * sizeof(*ies));

	ies[0].type = EV_KEY;
	ies[0].code = keycode;
	ies[0].value = down;

	ies[1].type = EV_SYN;
	ies[1].code = SYN_REPORT;
	ies[1].value = 0;

	_send_input_msg(this, ies, KEYBOARD_MAX_EVENTS);
}

void rf_streamer_send_pointer_event(
	RfStreamer *this,
	double rx,
	double ry,
	bool left,
	bool middle,
	bool right,
	bool back,
	bool forward,
	bool wup,
	bool wdown,
	bool wleft,
	bool wright
)
{
	g_return_if_fail(RF_IS_STREAMER(this));

	if (!this->running)
		return;

	// Assuming user only have 1 monitor when they set desktop size to 0x0.
	const uint32_t desktop_width =
		this->desktop_width > 0 ? this->desktop_width :
					  this->monitor_x + this->frame_width;
	const uint32_t desktop_height =
		this->desktop_height > 0 ? this->desktop_height :
					   this->monitor_y + this->frame_height;
	// Typically desktop environment will map uinput `EV_ABS` max size to
	// the whole virtual desktop, so we need to convert the position to
	// global position in the virtual desktop.
	const double x =
		(this->monitor_x + rx * this->frame_width) / desktop_width;
	const double y =
		(this->monitor_y + ry * this->frame_height) / desktop_height;
	g_debug("Input: Calculated global position x %f and y %f.", x, y);

	size_t length = 0;
	struct input_event ies[POINTER_MAX_EVENTS];
	memset(ies, 0, POINTER_MAX_EVENTS * sizeof(*ies));

	ies[length].type = EV_ABS;
	ies[length].code = ABS_X;
	ies[length].value = RF_POINTER_MAX * x;
	++length;

	ies[length].type = EV_ABS;
	ies[length].code = ABS_Y;
	ies[length].value = RF_POINTER_MAX * y;
	++length;

	ies[length].type = EV_KEY;
	ies[length].code = BTN_LEFT;
	ies[length].value = left;
	++length;

	ies[length].type = EV_KEY;
	ies[length].code = BTN_MIDDLE;
	ies[length].value = middle;
	++length;

	ies[length].type = EV_KEY;
	ies[length].code = BTN_RIGHT;
	ies[length].value = right;
	++length;

	// FIXME: BTN_BACK/FORWARD or BTN_4/5?
	ies[length].type = EV_KEY;
	ies[length].code = BTN_BACK;
	ies[length].value = back;
	++length;

	ies[length].type = EV_KEY;
	ies[length].code = BTN_FORWARD;
	ies[length].value = forward;
	++length;

	if (wup || wdown) {
		ies[length].type = EV_REL;
		ies[length].code = REL_WHEEL;
		ies[length].value = wup ? 1 : -1;
		++length;
	}

	if (wleft || wright) {
		ies[length].type = EV_REL;
		ies[length].code = REL_HWHEEL;
		// FIXME: Which one is positive? Left or right?
		ies[length].value = wleft ? 1 : -1;
		++length;
	}

	ies[length].type = EV_SYN;
	ies[length].code = SYN_REPORT;
	ies[length].value = 0;
	++length;

	_send_input_msg(this, ies, length);
}
