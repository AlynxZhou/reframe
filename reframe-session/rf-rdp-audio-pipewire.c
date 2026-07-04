#include "rf-rdp-audio-pipewire.h"

#include <errno.h>
#include <string.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include "rf-rdp-audio-stream.h"

#define RF_RDP_AUDIO_RECONNECT_MIN_MS 100u
#define RF_RDP_AUDIO_RECONNECT_MAX_MS 2000u

struct audio_capture {
	const char *socket_path;
	GSocketConnection *connection;
	GOutputStream *output;
	GByteArray *frame;
	struct pw_main_loop *loop;
	struct pw_stream *stream;
	unsigned int sample_rate;
	unsigned int channels;
	unsigned int frame_ms;
	size_t frame_bytes;
	unsigned int reconnect_delay_ms;
	uint64_t frames_sent;
};

static void disconnect_socket(struct audio_capture *capture)
{
	g_clear_object(&capture->connection);
	capture->output = NULL;
}

static bool connect_socket(struct audio_capture *capture)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketClient) client = NULL;
	g_autoptr(GSocketAddress) address = NULL;

	if (capture->connection != NULL)
		return true;

	client = g_socket_client_new();
	address = g_unix_socket_address_new(capture->socket_path);
	capture->connection = g_socket_client_connect(
		client,
		G_SOCKET_CONNECTABLE(address),
		NULL,
		&error
	);
	if (capture->connection == NULL) {
		g_message("RDP audio: Failed to connect to %s: %s.",
			capture->socket_path,
			error != NULL ? error->message : "unknown error");
		return false;
	}

	capture->output =
		g_io_stream_get_output_stream(G_IO_STREAM(capture->connection));
	capture->reconnect_delay_ms = RF_RDP_AUDIO_RECONNECT_MIN_MS;
	g_message("RDP audio: Connected to %s.", capture->socket_path);
	return true;
}

static bool write_audio_frame(struct audio_capture *capture)
{
	g_autoptr(GError) error = NULL;
	const bool ok = rf_rdp_audio_stream_write_pcm(
		capture->output,
		capture->sample_rate,
		capture->channels,
		capture->frame_ms,
		(uint64_t)g_get_monotonic_time(),
		capture->frame->data,
		capture->frame->len,
		&error
	);

	if (!ok) {
		g_message("RDP audio: Failed to write PCM frame: %s.",
			error != NULL ? error->message : "unknown error");
		disconnect_socket(capture);
		return false;
	}

	capture->frames_sent++;
	if (capture->frames_sent == 1)
		g_message("RDP audio: Sent first PCM frame %u Hz/%u channel(s), %u ms.",
			capture->sample_rate,
			capture->channels,
			capture->frame_ms);
	return true;
}

static void maybe_send_complete_frames(struct audio_capture *capture)
{
	while (capture->frame->len >= capture->frame_bytes) {
		if (!connect_socket(capture)) {
			g_usleep(capture->reconnect_delay_ms * 1000u);
			capture->reconnect_delay_ms = MIN(
				capture->reconnect_delay_ms * 2u,
				RF_RDP_AUDIO_RECONNECT_MAX_MS
			);
			return;
		}
		if (capture->frame->len > capture->frame_bytes)
			g_byte_array_set_size(capture->frame, capture->frame_bytes);
		if (!write_audio_frame(capture))
			return;
		g_byte_array_set_size(capture->frame, 0);
	}
}

static void append_audio_bytes(
	struct audio_capture *capture,
	const uint8_t *data,
	size_t length
)
{
	if (data == NULL || length == 0)
		return;

	while (length > 0) {
		const size_t needed = capture->frame_bytes - capture->frame->len;
		const size_t chunk = MIN(needed, length);

		g_byte_array_append(capture->frame, data, chunk);
		data += chunk;
		length -= chunk;
		maybe_send_complete_frames(capture);
	}
}

static void on_stream_process(void *data)
{
	struct audio_capture *capture = data;
	struct pw_buffer *buffer = NULL;

	while ((buffer = pw_stream_dequeue_buffer(capture->stream)) != NULL) {
		struct spa_buffer *spa_buffer = buffer->buffer;

		if (spa_buffer != NULL && spa_buffer->n_datas > 0) {
			struct spa_data *spa_data = &spa_buffer->datas[0];
			const uint8_t *audio = spa_data->data;
			size_t size = spa_data->chunk != NULL ?
				spa_data->chunk->size :
				spa_data->maxsize;
			size_t offset = spa_data->chunk != NULL ?
				spa_data->chunk->offset :
				0;

			if (audio != NULL && offset <= spa_data->maxsize &&
			    size <= spa_data->maxsize - offset) {
				audio += offset;
				append_audio_bytes(capture, audio, size);
			}
		}
		pw_stream_queue_buffer(capture->stream, buffer);
	}
}

static void on_stream_state_changed(
	void *data,
	enum pw_stream_state old,
	enum pw_stream_state state,
	const char *error
)
{
	(void)data;
	(void)old;

	if (state == PW_STREAM_STATE_ERROR)
		g_warning("RDP audio: PipeWire stream error: %s.",
			error != NULL ? error : "unknown error");
	else
		g_message("RDP audio: PipeWire stream state is %s.",
			pw_stream_state_as_string(state));
}

static bool valid_audio_options(
	unsigned int sample_rate,
	unsigned int channels,
	unsigned int frame_ms
)
{
	return (sample_rate == 44100 || sample_rate == 48000) &&
	       (channels == 1 || channels == 2) &&
	       (frame_ms == 10 || frame_ms == 20 || frame_ms == 40);
}

bool rf_rdp_audio_pipewire_run(
	const char *socket_path,
	unsigned int sample_rate,
	unsigned int channels,
	unsigned int frame_ms
)
{
	struct audio_capture capture = {
		.socket_path = socket_path,
		.sample_rate = sample_rate,
		.channels = channels,
		.frame_ms = frame_ms,
		.reconnect_delay_ms = RF_RDP_AUDIO_RECONNECT_MIN_MS
	};
	struct pw_stream_events stream_events = {
		PW_VERSION_STREAM_EVENTS,
		.state_changed = on_stream_state_changed,
		.process = on_stream_process
	};
	uint8_t buffer[1024] = { 0 };
	struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_audio_info_raw audio_info = SPA_AUDIO_INFO_RAW_INIT(
		.format = SPA_AUDIO_FORMAT_S16,
		.rate = sample_rate,
		.channels = channels
	);
	const struct spa_pod *params[1] = { NULL };

	if (socket_path == NULL || !valid_audio_options(sample_rate, channels, frame_ms)) {
		g_warning("RDP audio: Invalid options.");
		return false;
	}

	capture.frame_bytes = (size_t)sample_rate * channels * 2u * frame_ms / 1000u;
	capture.frame = g_byte_array_sized_new(capture.frame_bytes);
	if (capture.frame_bytes == 0)
		goto fail;

	pw_init(NULL, NULL);
	capture.loop = pw_main_loop_new(NULL);
	if (capture.loop == NULL)
		goto fail;

	capture.stream = pw_stream_new_simple(
		pw_main_loop_get_loop(capture.loop),
		"reframe-rdp-audio",
		pw_properties_new(
			PW_KEY_MEDIA_TYPE, "Audio",
			PW_KEY_MEDIA_CATEGORY, "Capture",
			PW_KEY_MEDIA_ROLE, "Screen",
			PW_KEY_NODE_NAME, "reframe-rdp-audio",
			PW_KEY_NODE_DESCRIPTION, "ReFrame RDP Audio",
			NULL
		),
		&stream_events,
		&capture
	);
	if (capture.stream == NULL)
		goto fail;

	if (channels == 1)
		audio_info.position[0] = SPA_AUDIO_CHANNEL_MONO;
	else {
		audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
		audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;
	}

	params[0] = spa_format_audio_raw_build(
		&builder,
		SPA_PARAM_EnumFormat,
		&audio_info
	);
	if (params[0] == NULL ||
	    pw_stream_connect(
		    capture.stream,
		    PW_DIRECTION_INPUT,
		    PW_ID_ANY,
		    PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
		    params,
		    1
	    ) < 0) {
		g_warning("RDP audio: Failed to connect PipeWire stream: %s.",
			g_strerror(errno));
		goto fail;
	}

	g_message(
		"RDP audio: Capturing PipeWire monitor as %u Hz/%u channel(s), %u ms frames.",
		sample_rate,
		channels,
		frame_ms
	);
	pw_main_loop_run(capture.loop);

	if (capture.stream != NULL)
		pw_stream_destroy(capture.stream);
	if (capture.loop != NULL)
		pw_main_loop_destroy(capture.loop);
	disconnect_socket(&capture);
	g_clear_pointer(&capture.frame, g_byte_array_unref);
	pw_deinit();
	return true;

fail:
	if (capture.stream != NULL)
		pw_stream_destroy(capture.stream);
	if (capture.loop != NULL)
		pw_main_loop_destroy(capture.loop);
	disconnect_socket(&capture);
	g_clear_pointer(&capture.frame, g_byte_array_unref);
	pw_deinit();
	return false;
}
