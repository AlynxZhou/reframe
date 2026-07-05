#include <stdint.h>
#include <stdbool.h>
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <gio/gunixsocketaddress.h>
#include <linux/uinput.h>

#include "rf-common.h"
#include "rf-resize.h"
#include "rf-streamer.h"

#define KEYBOARD_MAX_EVENTS 2
#define POINTER_MAX_EVENTS 10

struct _RfStreamer {
	GSocketClient parent_instance;
	RfConfig *config;
	GSocketAddress *address;
	GSocketConnection *connection;
	GMutex write_lock;
	GMutex frame_lock;
	GCond frame_cond;
	GIOCondition io_flags;
	GSource *source;
	GThread *frame_thread;
	int64_t last_frame_time;
	int64_t next_frame_request_time;
	int64_t max_interval;
	unsigned int desktop_width;
	unsigned int desktop_height;
	int monitor_x;
	int monitor_y;
	bool auto_desktop_layout;
	unsigned int rotation;
	// These are the real size of monitor and have nothing with VNC.
	uint32_t frame_width;
	uint32_t frame_height;
	bool frame_thread_running;
	bool frame_request_pending;
	bool running;
};
G_DEFINE_TYPE(RfStreamer, rf_streamer, G_TYPE_SOCKET_CLIENT)

enum {
	SIG_START,
	SIG_STOP,
	SIG_FRAME,
	SIG_CARD_PATH,
	SIG_CONNECTOR_NAME,
	SIG_AUTH,
	N_SIGS
};

static unsigned int sigs[N_SIGS] = { 0 };

static bool streamer_has_connection(RfStreamer *this)
{
	return this->running && this->connection != NULL;
}

static ssize_t
write_header_locked(RfStreamer *this, char type, size_t length, GError **error)
{
	if (!streamer_has_connection(this))
		return 0;
	return rf_send_header(this->connection, type, length, error);
}

static ssize_t write_data_locked(
	RfStreamer *this,
	const void *data,
	size_t length,
	GError **error
)
{
	if (!streamer_has_connection(this))
		return 0;

	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));
	return g_output_stream_write(os, data, length, NULL, error);
}

static void send_auth_msg(RfStreamer *this, pid_t pid)
{
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;

	g_mutex_lock(&this->write_lock);
	ret = write_header_locked(this, RF_MSG_TYPE_AUTH, 1, &error);
	if (ret <= 0)
		goto out;
	ret = write_data_locked(this, &pid, sizeof(pid), &error);

out:
	g_mutex_unlock(&this->write_lock);
	if (ret < 0) {
		g_warning("Auth: Failed to send auth PID: %s.", error->message);
		rf_streamer_stop(this);
	} else if (ret == 0) {
		g_warning("ReFrame Streamer disconnected.");
		rf_streamer_stop(this);
	} else {
		g_debug("Auth: Sent auth message for PID %d.", pid);
	}
}

static void
send_input_msg(RfStreamer *this, struct input_event *ies, const size_t length)
{
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;

	g_mutex_lock(&this->write_lock);
	ret = write_header_locked(this, RF_MSG_TYPE_INPUT, length, &error);
	if (ret <= 0)
		goto out;
	ret = write_data_locked(this, ies, length * sizeof(*ies), &error);

out:
	g_mutex_unlock(&this->write_lock);
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

static ssize_t send_frame_request(RfStreamer *this, GError **error)
{
	ssize_t ret = 0;

	g_mutex_lock(&this->write_lock);
	ret = write_header_locked(this, RF_MSG_TYPE_FRAME, 0, error);
	g_mutex_unlock(&this->write_lock);
	if (ret > 0)
		this->last_frame_time = g_get_monotonic_time();
	return ret;
}

static gboolean stop_streamer_idle(void *data)
{
	RfStreamer *this = data;

	rf_streamer_stop(this);
	g_object_unref(this);
	return G_SOURCE_REMOVE;
}

static void stop_streamer_from_thread(RfStreamer *this)
{
	g_idle_add(stop_streamer_idle, g_object_ref(this));
}

static gpointer frame_request_thread(void *data)
{
	RfStreamer *this = data;

	g_mutex_lock(&this->frame_lock);
	while (this->frame_thread_running) {
		while (this->frame_thread_running && this->frame_request_pending)
			g_cond_wait(&this->frame_cond, &this->frame_lock);
		if (!this->frame_thread_running)
			break;

		const int64_t now = g_get_monotonic_time();
		if (this->next_frame_request_time < 0)
			this->next_frame_request_time = now;
		if (now < this->next_frame_request_time) {
			g_cond_wait_until(
				&this->frame_cond,
				&this->frame_lock,
				this->next_frame_request_time
			);
			continue;
		}

		this->frame_request_pending = true;
		if (this->next_frame_request_time < now - this->max_interval)
			this->next_frame_request_time = now + this->max_interval;
		else
			this->next_frame_request_time += this->max_interval;
		g_mutex_unlock(&this->frame_lock);

		g_autoptr(GError) error = NULL;
		const ssize_t ret = send_frame_request(this, &error);

		g_mutex_lock(&this->frame_lock);
		if (ret <= 0) {
			this->frame_request_pending = false;
			this->frame_thread_running = false;
			g_cond_signal(&this->frame_cond);
			g_mutex_unlock(&this->frame_lock);
			if (ret < 0)
				g_warning(
					"Frame: Failed to send frame message: %s.",
					error != NULL ? error->message : "unknown error"
				);
			else
				g_warning("ReFrame Streamer disconnected.");
			stop_streamer_from_thread(this);
			return NULL;
		}
	}
	g_mutex_unlock(&this->frame_lock);
	return NULL;
}

static void complete_frame_request(RfStreamer *this)
{
	g_mutex_lock(&this->frame_lock);
	if (this->frame_request_pending) {
		this->frame_request_pending = false;
		g_cond_signal(&this->frame_cond);
	}
	g_mutex_unlock(&this->frame_lock);
}

static ssize_t
on_buffer(GSocketConnection *connection, struct rf_buffer *b, GError **error)
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
	for (unsigned int i = 0; i < b->md.length; ++i) {
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
		g_clear_object(&msgs[i]);

	return ret;
}

static ssize_t on_frame_msg(RfStreamer *this)
{
	g_debug("Frame: Received frame message.");

	struct rf_buffer bufs[RF_MAX_BUFS];
	ssize_t ret = 0;
	size_t length = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0) {
		length = 0;
		goto out;
	}
	complete_frame_request(this);
	// Empty buffer, maybe locked screen and turned monitor off, skip it.
	if (length == 0) {
		g_debug("Frame: Got empty buffer for primary plane.");
		goto out;
	} else if (length > RF_MAX_BUFS) {
		g_warning("Frame: Got invalid buffers length %ld.", length);
		goto out;
	}

	for (size_t i = 0; i < length; ++i) {
		ret = on_buffer(this->connection, &bufs[i], &error);
		if (ret <= 0)
			goto out;
	}

	struct rf_buffer *primary = &bufs[0];
	// Monitor size should be CRTC size.
	uint32_t frame_width = primary->md.crtc_width;
	uint32_t frame_height = primary->md.crtc_height;
	if (!rf_is_landscape(this->rotation)) {
		frame_width = primary->md.crtc_height;
		frame_height = primary->md.crtc_width;
	}
	if (this->frame_width != frame_width ||
	    this->frame_height != frame_height) {
		this->frame_width = frame_width;
		this->frame_height = frame_height;
	}

	g_signal_emit(this, sigs[SIG_FRAME], 0, length, bufs);

out:
	for (size_t i = 0; i < length; ++i)
		for (unsigned int j = 0; j < bufs[i].md.length; ++j)
			close(bufs[i].fds[j]);

	if (ret < 0)
		g_warning("Frame: Failed to receive frame: %s.", error->message);
	return ret;
}

static ssize_t on_card_path_msg(RfStreamer *this)
{
	g_autofree char *msg = NULL;
	ssize_t ret = 0;
	size_t length = 0;
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

static ssize_t on_connector_name_msg(RfStreamer *this)
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

static ssize_t on_desktop_layout_msg(RfStreamer *this)
{
	struct rf_desktop_layout layout = { 0 };
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0 || length != 1)
		goto out;

	ret = g_input_stream_read(is, &layout, sizeof(layout), NULL, &error);
	if (ret <= 0)
		goto out;
	if (layout.desktop_width == 0 || layout.desktop_height == 0)
		goto out;

	if (this->auto_desktop_layout) {
		this->desktop_width = layout.desktop_width;
		this->desktop_height = layout.desktop_height;
		this->monitor_x = layout.monitor_x;
		this->monitor_y = layout.monitor_y;
		g_message(
			"Input: Auto desktop layout %ux%u, monitor %d,%d.",
			this->desktop_width,
			this->desktop_height,
			this->monitor_x,
			this->monitor_y
		);
	} else {
		g_debug(
			"Input: Ignoring auto desktop layout because explicit input geometry is configured."
		);
	}

out:
	if (ret < 0)
		g_warning(
			"Input: Failed to receive desktop layout: %s.",
			error->message
		);
	return ret;
}

static ssize_t on_auth_msg(RfStreamer *this)
{
	struct rf_auth auth;
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0 || length != 1)
		goto out;

	ret = g_input_stream_read(is, &auth, sizeof(auth), NULL, &error);
	if (ret <= 0 || auth.pid < 0)
		goto out;

	g_debug("Auth: Received auth message for PID %d with result %s.",
		auth.pid,
		auth.ok ? "OK" : "not OK");
	g_signal_emit(this, sigs[SIG_AUTH], 0, auth.pid, auth.ok);

out:
	if (ret < 0)
		g_warning(
			"Auth: Failed to receive auth message: %s.",
			error->message
		);
	return ret;
}

static int on_socket_in(GSocket *socket, GIOCondition condition, void *data)
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
		ret = on_frame_msg(this);
		break;
	case RF_MSG_TYPE_CARD_PATH:
		ret = on_card_path_msg(this);
		break;
	case RF_MSG_TYPE_CONNECTOR_NAME:
		ret = on_connector_name_msg(this);
		break;
	case RF_MSG_TYPE_DESKTOP_LAYOUT:
		ret = on_desktop_layout_msg(this);
		break;
	case RF_MSG_TYPE_AUTH:
		ret = on_auth_msg(this);
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

static void dispose(GObject *o)
{
	RfStreamer *this = RF_STREAMER(o);

	rf_streamer_stop(this);
	g_clear_object(&this->address);

	G_OBJECT_CLASS(rf_streamer_parent_class)->dispose(o);
}

static void finalize(GObject *o)
{
	RfStreamer *this = RF_STREAMER(o);

	g_mutex_clear(&this->write_lock);
	g_mutex_clear(&this->frame_lock);
	g_cond_clear(&this->frame_cond);

	G_OBJECT_CLASS(rf_streamer_parent_class)->finalize(o);
}

static void rf_streamer_class_init(RfStreamerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->dispose = dispose;
	o_class->finalize = finalize;

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
	sigs[SIG_AUTH] = g_signal_new(
		"auth",
		RF_TYPE_STREAMER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		2,
		G_TYPE_INT,
		G_TYPE_BOOLEAN
	);
}

static void rf_streamer_init(RfStreamer *this)
{
	this->config = NULL;
	this->address = NULL;
	this->connection = NULL;
	this->io_flags = G_IO_IN | G_IO_PRI;
	this->source = NULL;
	this->frame_thread = NULL;
	this->last_frame_time = -1;
	this->next_frame_request_time = -1;
	this->max_interval = 1000000 / 30;
	this->desktop_width = 0;
	this->desktop_height = 0;
	this->monitor_x = 0;
	this->monitor_y = 0;
	this->auto_desktop_layout = false;
	this->rotation = 0;
	this->frame_width = 0;
	this->frame_height = 0;
	this->frame_thread_running = false;
	this->frame_request_pending = false;
	this->running = false;
	g_mutex_init(&this->write_lock);
	g_mutex_init(&this->frame_lock);
	g_cond_init(&this->frame_cond);
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
	this->address = g_unix_socket_address_new(socket_path);
}

int rf_streamer_start(RfStreamer *this)
{
	g_return_val_if_fail(RF_IS_STREAMER(this), -1);

	if (this->running)
		return 0;

	this->last_frame_time = -1;
	const unsigned int fps = rf_config_get_fps(this->config);
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
	this->auto_desktop_layout =
		this->desktop_width == 0 && this->desktop_height == 0 &&
		this->monitor_x == 0 && this->monitor_y == 0;
	g_message(
		"Input: Got monitor x %u and y %u.",
		this->monitor_x,
		this->monitor_y
	);
	this->rotation = rf_config_get_rotation(this->config);
	g_message("Frame: Got screen rotation %u.", this->rotation);
	this->frame_width = 0;
	this->frame_height = 0;
	this->next_frame_request_time = -1;

	g_autoptr(GError) error = NULL;
	this->connection = g_socket_client_connect(
		G_SOCKET_CLIENT(this),
		G_SOCKET_CONNECTABLE(this->address),
		NULL,
		&error
	);
	if (this->connection == NULL) {
		g_warning(
			"Failed to connecting to ReFrame Streamer: %s.",
			error->message
		);
		return -2;
	}
	this->running = true;
	GSocket *socket = g_socket_connection_get_socket(this->connection);
	this->source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(
		this->source, G_SOURCE_FUNC(on_socket_in), this, NULL
	);
	g_source_attach(this->source, NULL);
	g_mutex_lock(&this->frame_lock);
	this->frame_thread_running = true;
	this->frame_request_pending = false;
	this->next_frame_request_time = g_get_monotonic_time();
	g_mutex_unlock(&this->frame_lock);
	this->frame_thread = g_thread_new(
		"rf-frame-request",
		frame_request_thread,
		this
	);

	g_debug("Signal: Emitting ReFrame Streamer start signal.");
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

	g_debug("Signal: Emitting ReFrame Streamer stop signal.");
	g_signal_emit(this, sigs[SIG_STOP], 0);

	g_mutex_lock(&this->frame_lock);
	this->frame_thread_running = false;
	this->frame_request_pending = false;
	g_cond_signal(&this->frame_cond);
	g_mutex_unlock(&this->frame_lock);
	if (this->frame_thread != NULL) {
		g_thread_join(this->frame_thread);
		this->frame_thread = NULL;
	}

	if (this->source != NULL)
		g_source_destroy(this->source);
	g_clear_pointer(&this->source, g_source_unref);
	// Dropping the last reference of it will automatically close IO streams
	// and socket.
	g_mutex_lock(&this->write_lock);
	this->running = false;
	g_clear_object(&this->connection);
	g_mutex_unlock(&this->write_lock);
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

	send_input_msg(this, ies, KEYBOARD_MAX_EVENTS);
}

void rf_streamer_send_pointer_event(
	RfStreamer *this,
	double rx,
	double ry,
	unsigned int surface_width,
	unsigned int surface_height,
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

	// Input coordinates are relative to the remote surface, which can differ
	// from the physical DRM CRTC size on scaled outputs.
	if (surface_width == 0)
		surface_width = this->frame_width;
	if (surface_height == 0)
		surface_height = this->frame_height;
	int abs_x = 0;
	int abs_y = 0;
	if (rf_map_point_to_absolute(
		    rx,
		    ry,
		    surface_width,
		    surface_height,
		    this->desktop_width,
		    this->desktop_height,
		    this->monitor_x,
		    this->monitor_y,
		    RF_POINTER_MAX,
		    &abs_x,
		    &abs_y
	    ) < 0)
		return;
	g_debug("Input: Calculated absolute pointer position x %d and y %d.",
		abs_x,
		abs_y);

	size_t length = 0;
	struct input_event ies[POINTER_MAX_EVENTS];
	memset(ies, 0, POINTER_MAX_EVENTS * sizeof(*ies));

	ies[length].type = EV_ABS;
	ies[length].code = ABS_X;
	ies[length].value = abs_x;
	++length;

	ies[length].type = EV_ABS;
	ies[length].code = ABS_Y;
	ies[length].value = abs_y;
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

	// The back/forward side buttons on mouse are neither BTN_BACK/FORWARD or
	// BTN_4/5, they actually send BTN_SIDE and BTN_EXTRA.
	ies[length].type = EV_KEY;
	ies[length].code = BTN_SIDE;
	ies[length].value = back;
	++length;

	ies[length].type = EV_KEY;
	ies[length].code = BTN_EXTRA;
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

	send_input_msg(this, ies, length);
}

void rf_streamer_auth(RfStreamer *this, pid_t pid)
{
	g_return_if_fail(RF_IS_STREAMER(this));
	g_return_if_fail(pid >= 0);

	send_auth_msg(this, pid);
}
