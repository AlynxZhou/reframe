#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib/gstdio.h>

#include "rf-clipboard-rich.h"
#include "rf-rdp-audio-stream.h"
#include "rf-rdp-av1.h"
#include "rf-rdp-avc.h"
#include "rf-rdp-cliprdr.h"
#include "rf-rdp-core.h"
#include "rf-rdp-dvc.h"
#include "rf-rdp-gfx.h"
#include "rf-rdp-input.h"
#include "rf-rdp-mcs.h"
#include "rf-rdp-nsc.h"
#include "rf-rdp-planar.h"
#include "rf-rdp-proto.h"
#include "rf-rdp-rfx.h"
#include "rf-rdp-rdpsnd.h"
#include "rf-rdp-server.h"

#define RDP_MAX_MCS_PAYLOAD 0x7fffu
#define RDP_BITMAP_PDU_OVERHEAD (6u + 12u + 4u + 18u)
#define RDP_BITMAP_PACKET_CAPACITY (15u + RDP_MAX_MCS_PAYLOAD)
#define RDP_STATS_INTERVAL_US (5 * G_USEC_PER_SEC)
#define RDP_BITMAP_TARGET_BYTES_PER_SECOND (6ull * 1024ull * 1024ull)
#define RDP_RDPGFX_DEFAULT_TARGET_BANDWIDTH_MBPS 20u
#define RDP_BITMAP_MIN_FPS 3u
#define RDP_RDPGFX_MAX_VIDEO_QUALITY_LEVEL 3u
#define RDP_RDPGFX_AVC_MIN_CODED_WIDTH 160u
#define RDP_RDPGFX_AVC_MIN_CODED_HEIGHT 64u
#define RDP_RDPGFX_DVC_CHANNEL_ID 1u
#define RDP_RDPGFX_SURFACE_ID 1u
#define RDP_RDPGFX_PLANAR_RLE_MAX_PIXELS (64u * 1024u)
#define RDP_RDPGFX_SUSPEND_FRAME_ACKNOWLEDGEMENT 0xffffffffu
#define RDP_RDPGFX_AVC444_LC_STAT_COUNT 3u
#define RDP_RDPGFX_CODEC_PROBE_WIDTH 320u
#define RDP_RDPGFX_CODEC_PROBE_HEIGHT 240u
#define RDP_AUDIO_STATS_INTERVAL_US (10 * G_USEC_PER_SEC)
#define RDP_AUDIO_QUEUE_TARGET_MS 120u
#define RDP_AUDIO_SILENCE_THRESHOLD 256
#define RDP_AUDIO_SILENCE_HOLD_MS 120u

static unsigned int align16(unsigned int value)
{
	return (value + 15u) & ~15u;
}

static const char *yes_no(bool value)
{
	return value ? "yes" : "no";
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = (value >> 24) & 0xff;
}

static const char *av1_mode_name(enum rf_rdp_av1_mode mode)
{
	switch (mode) {
	case RF_RDP_AV1_MODE_I444:
		return "i444";
	case RF_RDP_AV1_MODE_I420:
	default:
		return "i420";
	}
}

struct _RfRDPServer {
	RfRemoteServer parent_instance;
	RfConfig *config;
	GSocketService *service;
	GSocketService *clipboard_service;
	GSocketService *audio_service;
	GSocketAddress *clipboard_address;
	GTlsCertificate *certificate;
	GMutex lock;
	GMutex audio_lock;
	GCond audio_cond;
	GThread *audio_sender_thread;
	GList *clients;
	GHashTable *clipboard_sockets;
	struct client *resize_owner;
	char **ips;
	char *tls_private_key_file;
	char *tls_certificate_file;
	char *username;
	char *domain;
	char *password;
	char *graphics;
	char *avc_encoder;
	char *audio_codec;
	char *clipboard_socket_path;
	char *rdp_audio_socket_path;
	char *clipboard_text;
	struct rf_clipboard_rich_payload rich_clipboard;
	GByteArray *clipboard_wire;
	GQueue *audio_queue;
	GByteArray *last_frame;
	unsigned int port;
	unsigned int desktop_width;
	unsigned int desktop_height;
	unsigned int width;
	unsigned int height;
	unsigned int last_frame_width;
	unsigned int last_frame_height;
	unsigned int updates_sent;
	unsigned int max_fps;
	unsigned int adaptive_fps;
	int64_t next_render_time_us;
	int64_t stats_last_log_time_us;
	uint64_t stats_frames_sent;
	uint64_t stats_frames_skipped;
	uint64_t stats_bytes_sent;
	uint64_t stats_send_time_us;
	uint64_t stats_avc444_lc[RDP_RDPGFX_AVC444_LC_STAT_COUNT];
	uint64_t audio_stats_frames;
	uint64_t audio_stats_bytes;
	uint64_t audio_stats_dropped;
	uint64_t audio_stats_latency_us;
	uint64_t audio_stats_latency_samples;
	uint64_t audio_stats_max_latency_us;
	uint64_t audio_stats_skipped_frames;
	uint64_t audio_stats_wave_confirms;
	uint64_t audio_stats_first_confirm_us;
	int64_t audio_stats_last_log_time_us;
	unsigned int stats_min_target_fps;
	unsigned int audio_stats_clients;
	unsigned int rdpgfx_video_quality_level;
	int64_t rdpgfx_video_quality_last_change_time_us;
	int configured_video_quality;
	unsigned int max_video_quality_level;
	unsigned int rdpgfx_target_bandwidth_mbps;
	unsigned int audio_sample_rate;
	unsigned int audio_channels;
	unsigned int audio_frame_ms;
	uint64_t rdpgfx_target_bytes_per_second;
	uint64_t audio_sequence;
	uint64_t audio_queue_dropped;
	uint64_t audio_stats_silence_suppressed;
	unsigned int audio_silent_frames;
	uint32_t stats_max_rdpgfx_inflight;
	uint32_t stats_max_rdpgfx_ack_queue_depth;
	uint16_t stats_max_rdpgfx_qoe_time_diff_se;
	uint16_t stats_max_rdpgfx_qoe_time_diff_edr;
	uint64_t stats_rdpgfx_zgfx_payloads;
	uint64_t stats_rdpgfx_zgfx_saved_bytes;
	bool nla;
	bool clipboard;
	bool audio;
	bool audio_silence_suppressed;
	bool running;
};
G_DEFINE_TYPE(RfRDPServer, rf_rdp_server, RF_TYPE_REMOTE_SERVER)

struct client {
	RfRDPServer *server;
	GSocketConnection *connection;
	GIOStream *stream;
	GThread *thread;
	GMutex write_lock;
	gint ref_count;
	uint32_t selected_protocol;
	uint32_t rdpgfx_channel_id;
	uint16_t desktop_width;
	uint16_t desktop_height;
	uint16_t channel_count;
	uint16_t rdpsnd_channel_id;
	uint16_t cliprdr_channel_id;
	uint16_t drdynvc_channel_id;
	uint32_t rdpsnd_channel_options;
	uint32_t cliprdr_channel_options;
	uint32_t drdynvc_channel_options;
	uint32_t cliprdr_requested_format_id;
	uint32_t cliprdr_client_html_format_id;
	uint32_t cliprdr_client_png_format_id;
	uint32_t cliprdr_client_tiff_format_id;
	uint32_t cliprdr_client_jpeg_format_id;
	uint32_t cliprdr_client_webp_format_id;
	uint32_t cliprdr_client_bmp_format_id;
	struct rf_rdp_cliprdr_format_list cliprdr_client_formats;
	struct rf_rdp_rdpsnd_client_formats rdpsnd_client_formats;
	uint16_t drdynvc_version;
	uint16_t rdpgfx_surface_width;
	uint16_t rdpgfx_surface_height;
	uint32_t rdpgfx_caps_version;
	uint32_t rdpgfx_caps_flags;
	uint32_t rdpgfx_frame_id;
	uint32_t rdpgfx_last_ack_frame_id;
	uint32_t rdpgfx_ack_queue_depth;
	uint32_t rdpgfx_total_frames_decoded;
	uint32_t rdpgfx_qoe_frame_id;
	uint32_t rdpgfx_qoe_timestamp;
	uint16_t rdpgfx_qoe_time_diff_se;
	uint16_t rdpgfx_qoe_time_diff_edr;
	struct rf_rdp_pointer_state pointer;
	struct rf_rdp_core_capabilities caps;
	enum rf_rdp_graphics_mode graphics_mode;
	enum rf_rdp_gfx_codec rdpgfx_policy_codec;
	RfRdpNscContext *nsc;
	RfRdpRfxContext *rfx;
	RfRdpAv1Encoder *av1;
	RfRdpAvcEncoder *avc;
	RfRdpAvcEncoder *avc_chroma;
	struct rf_rdp_cliprdr_channel_reassembler cliprdr_channel_reassembler;
	int64_t av1_bit_rate;
	int64_t avc_bit_rate;
	int64_t rdpsnd_first_audio_sent_time_us;
	unsigned int av1_gop_size;
	unsigned int avc_gop_size;
	unsigned int av1_quality_level;
	unsigned int avc_quality_level;
	enum rf_rdp_av1_mode av1_mode;
	int rdpsnd_selected_format;
	uint8_t av1_qp;
	uint8_t avc_qp;
	uint8_t rdpsnd_block_no;
	uint8_t rdpsnd_first_audio_block_no;
	uint16_t av1_width;
	uint16_t av1_height;
	uint16_t avc_width;
	uint16_t avc_height;
	uint16_t avc444_signature_x;
	uint16_t avc444_signature_y;
	uint16_t avc444_signature_width;
	uint16_t avc444_signature_height;
	uint64_t avc444_luma_signature;
	uint64_t avc444_chroma_signature;
	bool counted;
	bool needs_full_frame;
	bool avc444_have_signature;
	bool surface_fallback_logged;
	bool bitmap_rle_logged;
	bool cliprdr_caps_sent;
	bool cliprdr_ready;
	bool cliprdr_client_caps_received;
	bool cliprdr_client_has_unicode_text;
	bool rdpsnd_ready;
	bool rdpsnd_formats_sent;
	bool rdpsnd_client_formats_received;
	bool rdpsnd_training_confirmed;
	bool rdpsnd_quality_mode_received;
	bool rdpsnd_first_audio_sent;
	bool rdpsnd_first_audio_confirmed;
	bool drdynvc_capability_sent;
	bool drdynvc_ready;
	bool rdpgfx_create_sent;
	bool rdpgfx_channel_open;
	bool rdpgfx_caps_confirmed;
	bool rdpgfx_surface_ready;
	bool rdpgfx_disabled;
	bool rdpgfx_av1_available;
	bool rdpgfx_avc420_available;
	bool rdpgfx_progressive_available;
	bool rdpgfx_remotefx_available;
	bool rdpgfx_update_logged;
	bool rdpgfx_wait_logged;
	bool rdpgfx_zgfx_compressed_logged;
	bool rdpgfx_fragment_logged;
	bool rdpgfx_frame_ack_received;
	bool rdpgfx_frame_ack_logged;
	bool rdpgfx_qoe_ack_logged;
	bool rdpgfx_avc_client_disabled_logged;
	bool rdpgfx_av1_unavailable_logged;
	bool rdpgfx_avc_unavailable_logged;
	bool rdpgfx_avc444_software_fallback_logged;
};

struct audio_client_target {
	struct client *client;
	struct rf_rdp_rdpsnd_audio_format format;
	bool has_format;
};

struct audio_frame {
	struct rf_rdp_audio_pcm_header header;
	GByteArray *pcm;
	uint64_t sequence;
};

static void clear_queued_audio_frames(RfRDPServer *this);

struct audio_connection {
	RfRDPServer *server;
	GSocketConnection *connection;
};

static bool rdpgfx_avc444_supported(RfRDPServer *this)
{
	RfRdpAvcEncoder *probe = NULL;
	bool supported = false;

	probe = rf_rdp_avc_hardware_encoder_new_with_rate(
		RDP_RDPGFX_CODEC_PROBE_WIDTH,
		RDP_RDPGFX_CODEC_PROBE_HEIGHT,
		30,
		2000000,
		28,
		60,
		this->avc_encoder
	);
	supported = probe != NULL && rf_rdp_avc_encoder_is_hardware(probe);
	rf_rdp_avc_encoder_free(probe);
	return supported;
}

static struct rf_rdp_gfx_server_codecs rdpgfx_server_codecs(RfRDPServer *this)
{
	return (struct rf_rdp_gfx_server_codecs){
	#ifdef RF_HAVE_RDP_AVC
		.av1 = true,
		.avc420 = true,
		.avc444 = rdpgfx_avc444_supported(this),
	#endif
		.progressive = true,
		.remotefx = true,
		.planar = true
	};
}

static void log_rdpgfx_caps_advertise(const struct rf_rdp_gfx_caps *caps)
{
	g_message(
		"RDP: GFX caps advertised sets=%u selected=0x%08x flags=0x%08x client-codecs avc420=%s avc444=%s avc444v2=%s progressive=%s progressivev2=%s remotefx=%s planar=%s av1=%s av1-i444=%s.",
		caps->count,
		caps->selected_version,
		caps->selected_flags,
		yes_no(caps->avc420),
		yes_no(caps->avc444),
		yes_no(caps->avc444_v2),
		yes_no(caps->progressive),
		yes_no(caps->progressive_v2),
		yes_no(caps->remotefx),
		yes_no(caps->planar),
		yes_no(caps->av1),
		yes_no(caps->av1_i444)
	);
}

static bool rdpgfx_av1_mode_supported(
	RfRDPServer *this,
	enum rf_rdp_av1_mode mode
)
{
	RfRdpAv1Encoder *probe = NULL;
	bool supported = false;

	if (mode == RF_RDP_AV1_MODE_I420)
		return true;

	probe = rf_rdp_av1_hardware_encoder_new_with_rate_and_mode(
		RDP_RDPGFX_CODEC_PROBE_WIDTH,
		RDP_RDPGFX_CODEC_PROBE_HEIGHT,
		30,
		2000000,
		28,
		60,
		mode,
		this->avc_encoder
	);
	supported = probe != NULL && rf_rdp_av1_encoder_mode(probe) == mode;
	rf_rdp_av1_encoder_free(probe);
	return supported;
}

static void log_rdpgfx_codec_policy(
	const struct rf_rdp_gfx_caps *caps,
	const struct rf_rdp_gfx_server_codecs *codecs,
	enum rf_rdp_gfx_codec policy_codec
)
{
	g_message(
		"RDP: GFX confirmed caps version=0x%08x flags=0x%08x policy-codec=%s server-codecs avc420=%s avc444=%s progressive=%s remotefx=%s planar=%s av1=%s; WireToSurface updates enabled.",
		caps->selected_version,
		caps->selected_flags,
		rf_rdp_gfx_codec_name(policy_codec),
		yes_no(codecs->avc420),
		yes_no(codecs->avc444),
		yes_no(codecs->progressive),
		yes_no(codecs->remotefx),
		yes_no(codecs->planar),
		yes_no(codecs->av1)
	);
}

static bool send_bitmap_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
);
static bool send_graphics_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent,
	bool *deferred
);

static void client_reset_avc(struct client *client)
{
	if (client == NULL)
		return;

	rf_rdp_avc_encoder_free(client->avc);
	rf_rdp_avc_encoder_free(client->avc_chroma);
	rf_rdp_av1_encoder_free(client->av1);
	client->avc = NULL;
	client->avc_chroma = NULL;
	client->av1 = NULL;
	client->av1_width = 0;
	client->av1_height = 0;
	client->av1_bit_rate = 0;
	client->av1_gop_size = 0;
	client->av1_quality_level = 0;
	client->av1_qp = 0;
	client->avc_width = 0;
	client->avc_height = 0;
	client->avc_bit_rate = 0;
	client->avc_gop_size = 0;
	client->avc_quality_level = 0;
	client->avc_qp = 0;
	client->avc444_signature_x = 0;
	client->avc444_signature_y = 0;
	client->avc444_signature_width = 0;
	client->avc444_signature_height = 0;
	client->avc444_luma_signature = 0;
	client->avc444_chroma_signature = 0;
	client->avc444_have_signature = false;
}

static struct client *client_ref(struct client *client)
{
	if (client == NULL)
		return NULL;
	g_atomic_int_inc(&client->ref_count);
	return client;
}

static void client_free(struct client *client)
{
	if (client == NULL)
		return;

	g_clear_object(&client->stream);
	g_clear_object(&client->connection);
	client_reset_avc(client);
	rf_rdp_nsc_context_free(client->nsc);
	rf_rdp_rfx_context_free(client->rfx);
	rf_rdp_cliprdr_channel_reassembler_clear(
		&client->cliprdr_channel_reassembler
	);
	g_mutex_clear(&client->write_lock);
	g_free(client);
}

static void client_unref(struct client *client)
{
	if (client == NULL)
		return;
	if (g_atomic_int_dec_and_test(&client->ref_count))
		client_free(client);
}

static bool read_exact(GInputStream *input, void *buffer, size_t length)
{
	gsize read = 0;
	g_autoptr(GError) error = NULL;

	return g_input_stream_read_all(
		input, buffer, length, &read, NULL, &error
	) && read == length;
}

static bool write_exact(GOutputStream *output, const void *buffer, size_t length)
{
	gsize written = 0;
	g_autoptr(GError) error = NULL;

	if (g_output_stream_write_all(
		    output,
		    buffer,
		    length,
		    &written,
		    NULL,
		    &error
	    ) && written == length)
		return true;

	g_warning("RDP: Failed to write %zu bytes: %s.",
		length,
		error != NULL ? error->message : "short write");
	return false;
}

static bool client_write_exact(
	struct client *client,
	const void *buffer,
	size_t length
)
{
	GOutputStream *output = g_io_stream_get_output_stream(client->stream);
	bool ok = false;

	g_mutex_lock(&client->write_lock);
	ok = write_exact(output, buffer, length);
	g_mutex_unlock(&client->write_lock);
	return ok;
}

static void set_client_desktop_size(
	struct client *client,
	unsigned int requested_width,
	unsigned int requested_height
)
{
	RfRDPServer *this = client->server;
	unsigned int width = 0;
	unsigned int height = 0;
	unsigned int current_width = 0;
	unsigned int current_height = 0;
	bool accepted = false;

	width = requested_width;
	height = requested_height;
	if (width > 0xffff)
		width = 0xffff;
	if (height > 0xffff)
		height = 0xffff;

	client->desktop_width = width;
	client->desktop_height = height;
	g_mutex_lock(&this->lock);
	if (rf_rdp_core_should_accept_desktop_resize(
		    this->resize_owner != NULL,
		    this->resize_owner == client,
		    this->desktop_width,
		    this->desktop_height,
		    width,
		    height
	    )) {
		if (this->resize_owner == NULL)
			this->resize_owner = client;
		else if (this->resize_owner != client)
			g_message(
				"RDP: Desktop resize ownership moved from %ux%u to larger client %ux%u.",
				this->desktop_width,
				this->desktop_height,
				width,
				height
			);
		this->resize_owner = client;
		this->desktop_width = width;
		this->desktop_height = height;
		accepted = true;
	}
	current_width = this->desktop_width;
	current_height = this->desktop_height;
	g_mutex_unlock(&this->lock);
	if (!accepted && width > 0 && height > 0)
		g_message(
			"RDP: Keeping desktop size %ux%u; secondary client requested %ux%u.",
			current_width,
			current_height,
			width,
			height
		);
}

static GByteArray *read_tpkt(GInputStream *input)
{
	uint8_t header[4] = { 0 };
	uint16_t tpkt_length = 0;

	if (!read_exact(input, header, sizeof(header)))
		return NULL;
	if (!rf_rdp_read_tpkt_header(header, sizeof(header), &tpkt_length))
		return NULL;

	GByteArray *pdu = g_byte_array_sized_new(tpkt_length);
	g_byte_array_append(pdu, header, sizeof(header));
	g_byte_array_set_size(pdu, tpkt_length);
	if (!read_exact(input, pdu->data + sizeof(header), tpkt_length - sizeof(header))) {
		g_byte_array_unref(pdu);
		return NULL;
	}

	return pdu;
}

static struct client *largest_desktop_client(GList *clients)
{
	struct client *largest = NULL;
	uint64_t largest_area = 0;

	for (GList *l = clients; l != NULL; l = l->next) {
		struct client *candidate = l->data;
		const uint64_t area =
			(uint64_t)candidate->desktop_width * candidate->desktop_height;

		if (area > largest_area) {
			largest = candidate;
			largest_area = area;
		}
	}
	return largest;
}

static void remove_client(struct client *client)
{
	RfRDPServer *this = client->server;
	bool last_client = false;
	bool resize_changed = false;
	unsigned int resize_width = 0;
	unsigned int resize_height = 0;

	g_mutex_lock(&this->lock);
	if (client->counted) {
		this->clients = g_list_remove(this->clients, client);
		client->counted = false;
		if (this->clients == NULL) {
			last_client = true;
			g_clear_pointer(&this->last_frame, g_byte_array_unref);
			this->last_frame_width = 0;
			this->last_frame_height = 0;
			this->next_render_time_us = -1;
		}
	}
	if (this->resize_owner == client) {
		this->resize_owner = NULL;
		if (this->clients != NULL) {
			struct client *next_owner = largest_desktop_client(this->clients);

			this->resize_owner = next_owner;
			if (next_owner->desktop_width > 0 &&
			    next_owner->desktop_height > 0 &&
			    (this->desktop_width != next_owner->desktop_width ||
			     this->desktop_height != next_owner->desktop_height)) {
				this->desktop_width = next_owner->desktop_width;
				this->desktop_height = next_owner->desktop_height;
				resize_width = this->desktop_width;
				resize_height = this->desktop_height;
				resize_changed = true;
			}
		}
	}
	g_mutex_unlock(&this->lock);

	if (last_client)
		rf_remote_server_handle_last_client(RF_REMOTE_SERVER(this));
	else if (resize_changed)
		rf_remote_server_handle_resize_event(
			RF_REMOTE_SERVER(this),
			resize_width,
			resize_height
		);
}

static GList *replace_existing_clients_locked(
	RfRDPServer *this,
	const struct client *new_client,
	unsigned int *replaced_clients
)
{
	GList *streams = NULL;

	for (GList *l = this->clients; l != NULL;) {
		GList *next = l->next;
		struct client *old_client = l->data;
		GIOStream *stream = old_client->stream;

		if (old_client == new_client) {
			l = next;
			continue;
		}

		if (stream == NULL && old_client->connection != NULL)
			stream = G_IO_STREAM(old_client->connection);
		if (stream != NULL)
			streams = g_list_prepend(streams, g_object_ref(stream));

		this->clients = g_list_delete_link(this->clients, l);
		old_client->counted = false;
		old_client->needs_full_frame = false;
		if (this->resize_owner == old_client)
			this->resize_owner = NULL;
		if (replaced_clients != NULL)
			(*replaced_clients)++;
		l = next;
	}
	return streams;
}

static void close_replaced_client_streams(GList *streams)
{
	for (GList *l = streams; l != NULL; l = l->next)
		g_io_stream_close(G_IO_STREAM(l->data), NULL, NULL);
	g_list_free_full(streams, g_object_unref);
}

static void add_client(struct client *client)
{
	RfRDPServer *this = client->server;
	GList *replaced_streams = NULL;
	unsigned int replaced_clients = 0;
	bool first_client = false;
	bool resize_owner = false;

	g_mutex_lock(&this->lock);
	if (!client->counted) {
		const bool had_clients = this->clients != NULL;
		const bool replace_clients =
			rf_rdp_core_should_replace_existing_client(
				client->counted,
				(unsigned int)g_list_length(this->clients)
			);

		if (replace_clients) {
			replaced_streams = replace_existing_clients_locked(
				this,
				client,
				&replaced_clients
			);
			g_clear_pointer(&this->last_frame, g_byte_array_unref);
			this->last_frame_width = 0;
			this->last_frame_height = 0;
			this->next_render_time_us = -1;
		}

		first_client = !had_clients && this->clients == NULL;
		this->clients = g_list_prepend(this->clients, client);
		client->counted = true;
		client->needs_full_frame = true;
		if (first_client || replaced_clients > 0) {
			this->updates_sent = 0;
			this->next_render_time_us = -1;
			this->rdpgfx_video_quality_last_change_time_us = 0;
			this->stats_last_log_time_us = 0;
			this->stats_frames_sent = 0;
			this->stats_frames_skipped = 0;
			this->stats_bytes_sent = 0;
			this->stats_send_time_us = 0;
			this->stats_min_target_fps = 0;
			this->stats_max_rdpgfx_inflight = 0;
			this->stats_max_rdpgfx_ack_queue_depth = 0;
			this->stats_rdpgfx_zgfx_payloads = 0;
			this->stats_rdpgfx_zgfx_saved_bytes = 0;
		} else if (this->last_frame != NULL &&
				 this->last_frame_width == client->desktop_width &&
				 this->last_frame_height == client->desktop_height) {
			bool deferred = false;

			if (send_graphics_update(
				    client,
				    this->last_frame,
				    this->last_frame_width,
				    this->last_frame_height,
				    0,
				    0,
				    this->last_frame_width,
				    this->last_frame_height,
				    NULL,
				    &deferred
			    )) {
				if (!deferred) {
					client->needs_full_frame = false;
					g_message(
						"RDP: Sent cached full frame %ux%u to new client.",
						this->last_frame_width,
						this->last_frame_height
					);
				}
			} else {
				g_warning("RDP: Failed to send cached full frame; closing client.");
				g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
			}
		}
		if (replaced_clients > 0 &&
		    client->desktop_width > 0 && client->desktop_height > 0) {
			this->resize_owner = client;
			this->desktop_width = client->desktop_width;
			this->desktop_height = client->desktop_height;
		}
		resize_owner = this->resize_owner == client;
	}
	g_mutex_unlock(&this->lock);

	if (replaced_clients > 0) {
		g_message(
			"RDP: Replacing %u existing client(s) with the new connection.",
			replaced_clients
		);
		close_replaced_client_streams(replaced_streams);
	}
	if (first_client)
		rf_remote_server_handle_first_client(RF_REMOTE_SERVER(this));
	if (resize_owner && client->desktop_width > 0 && client->desktop_height > 0)
		rf_remote_server_handle_resize_event(
			RF_REMOTE_SERVER(this),
			client->desktop_width,
			client->desktop_height
		);
}

static bool negotiate_security(
	struct client *client,
	const struct rf_rdp_connection_request *request,
	GOutputStream *output
)
{
	RfRDPServer *this = client->server;
	const uint32_t requested = request->has_negotiation ?
		request->requested_protocols :
		RF_RDP_PROTOCOL_RDP;
	uint32_t selected = RF_RDP_PROTOCOL_RDP;

	if (this->nla) {
		if (requested & (RF_RDP_PROTOCOL_HYBRID | RF_RDP_PROTOCOL_HYBRID_EX)) {
			g_warning("RDP: Native CredSSP/NLA is not implemented yet.");
			uint8_t failure[19] = { 0 };
			size_t length = rf_rdp_write_negotiation_failure(
				failure,
				sizeof(failure),
				RF_RDP_NEG_FAILURE_HYBRID_REQUIRED
			);
			write_exact(output, failure, length);
			return false;
		}
		g_warning("RDP: Client did not request NLA.");
		uint8_t failure[19] = { 0 };
		size_t length = rf_rdp_write_negotiation_failure(
			failure,
			sizeof(failure),
			RF_RDP_NEG_FAILURE_HYBRID_REQUIRED
		);
		write_exact(output, failure, length);
		return false;
	}

	if (requested & RF_RDP_PROTOCOL_SSL)
		selected = RF_RDP_PROTOCOL_SSL;
	else if (request->has_negotiation) {
		uint8_t failure[19] = { 0 };
		size_t length = rf_rdp_write_negotiation_failure(
			failure,
			sizeof(failure),
			RF_RDP_NEG_FAILURE_SSL_REQUIRED
		);
		write_exact(output, failure, length);
		return false;
	}

	uint8_t response[19] = { 0 };
	size_t length = rf_rdp_write_negotiation_response(
		response,
		sizeof(response),
		selected
	);
	if (!write_exact(output, response, length))
		return false;

	client->selected_protocol = selected;
	if (selected == RF_RDP_PROTOCOL_SSL) {
		g_autoptr(GError) error = NULL;
		GIOStream *tls = g_tls_server_connection_new(
			G_IO_STREAM(client->connection),
			this->certificate,
			&error
		);
		if (tls == NULL) {
			g_warning("RDP: Failed to create TLS connection: %s.", error->message);
			return false;
		}
		if (!g_tls_connection_handshake(
			    G_TLS_CONNECTION(tls), NULL, &error
		    )) {
			g_warning("RDP: TLS handshake failed: %s.", error->message);
			g_object_unref(tls);
			return false;
		}
		g_set_object(&client->stream, tls);
		g_object_unref(tls);
	}

	return true;
}

static void get_desktop_size(
	struct client *client,
	uint16_t *width,
	uint16_t *height
)
{
	RfRDPServer *this = client->server;
	unsigned int current_width = client->desktop_width;
	unsigned int current_height = client->desktop_height;

	if (current_width == 0 || current_height == 0) {
		g_mutex_lock(&this->lock);
		if (current_width == 0)
			current_width = this->desktop_width;
		if (current_height == 0)
			current_height = this->desktop_height;
		if (current_width == 0)
			current_width = this->width;
		if (current_height == 0)
			current_height = this->height;
		g_mutex_unlock(&this->lock);
	}

	if (current_width == 0)
		current_width = 1920;
	if (current_height == 0)
		current_height = 1080;
	if (current_width > 0xffff)
		current_width = 0xffff;
	if (current_height > 0xffff)
		current_height = 0xffff;

	*width = current_width;
	*height = current_height;
}

static bool get_client_frame_size(
	const struct client *client,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t *width,
	uint16_t *height
)
{
	unsigned int client_width = client->desktop_width;
	unsigned int client_height = client->desktop_height;

	if (width == NULL || height == NULL || frame_width == 0 || frame_height == 0)
		return false;
	if (client_width == 0)
		client_width = frame_width;
	if (client_height == 0)
		client_height = frame_height;
	if (client_width > UINT16_MAX || client_height > UINT16_MAX)
		return false;

	*width = (uint16_t)client_width;
	*height = (uint16_t)client_height;
	return *width > 0 && *height > 0;
}

static GByteArray *render_client_surface_frame(
	const GByteArray *source,
	unsigned int source_width,
	unsigned int source_height,
	uint16_t client_width,
	uint16_t client_height
)
{
	uint16_t viewport_x = 0;
	uint16_t viewport_y = 0;
	uint16_t viewport_width = 0;
	uint16_t viewport_height = 0;
	const size_t source_stride = (size_t)source_width * 4;
	const size_t client_stride = (size_t)client_width * 4;
	const size_t client_length = client_stride * client_height;
	GByteArray *frame = NULL;

	if (source == NULL || source->data == NULL || source_width == 0 ||
	    source_height == 0 || client_width == 0 || client_height == 0 ||
	    (size_t)source_width > SIZE_MAX / 4 ||
	    (size_t)source_height > SIZE_MAX / source_stride ||
	    source->len < source_stride * source_height ||
	    client_stride == 0 || client_height > SIZE_MAX / client_stride)
		return NULL;
	if (!rf_rdp_core_fit_frame_to_client_surface(
		    source_width,
		    source_height,
		    client_width,
		    client_height,
		    &viewport_x,
		    &viewport_y,
		    &viewport_width,
		    &viewport_height
	    ))
		return NULL;

	frame = g_byte_array_sized_new(client_length);
	g_byte_array_set_size(frame, client_length);
	for (uint16_t y = 0; y < client_height; ++y) {
		uint8_t *row = frame->data + (size_t)y * client_stride;

		for (uint16_t x = 0; x < client_width; ++x) {
			row[(size_t)x * 4 + 0] = 0;
			row[(size_t)x * 4 + 1] = 0;
			row[(size_t)x * 4 + 2] = 0;
			row[(size_t)x * 4 + 3] = 0xff;
		}
	}

	for (uint16_t y = 0; y < viewport_height; ++y) {
		const uint32_t src_y =
			(uint32_t)((uint64_t)y * source_height / viewport_height);
		const uint8_t *src_row =
			source->data + (size_t)src_y * source_stride;
		uint8_t *dst_row =
			frame->data + (size_t)(viewport_y + y) * client_stride +
			(size_t)viewport_x * 4;

		for (uint16_t x = 0; x < viewport_width; ++x) {
			const uint32_t src_x =
				(uint32_t)((uint64_t)x * source_width / viewport_width);
			memcpy(dst_row + (size_t)x * 4, src_row + (size_t)src_x * 4, 4);
		}
	}
	return frame;
}

static bool write_core_packet(
	size_t (*writer)(uint8_t *, size_t, uint16_t),
	GOutputStream *output
)
{
	uint8_t packet[4096] = { 0 };
	const size_t length = writer(
		packet,
		sizeof(packet),
		RF_RDP_MCS_BASE_CHANNEL_ID
	);

	return length > 0 && write_exact(output, packet, length);
}

static bool write_data_packet(
	size_t (*writer)(uint8_t *, size_t, uint16_t, uint32_t),
	GOutputStream *output
)
{
	uint8_t packet[4096] = { 0 };
	const size_t length = writer(
		packet,
		sizeof(packet),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID
	);

	return length > 0 && write_exact(output, packet, length);
}

static bool write_demand_active_packet(
	struct client *client,
	GOutputStream *output
)
{
	uint8_t packet[4096] = { 0 };
	uint16_t width = 0;
	uint16_t height = 0;

	get_desktop_size(client, &width, &height);
	const size_t length = rf_rdp_core_write_demand_active(
		packet,
		sizeof(packet),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID,
		width,
		height
	);
	return length > 0 && write_exact(output, packet, length);
}

static bool send_drdynvc_payload_counted(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length,
	size_t *bytes_sent
)
{
	uint8_t channel[8 + RF_RDP_DVC_CHANNEL_CHUNK_LENGTH] = { 0 };
	const uint32_t extra_flags =
		(client->drdynvc_channel_options &
		 RF_RDP_MCS_CHANNEL_OPTION_SHOW_PROTOCOL) != 0 ?
			RF_RDP_DVC_CHANNEL_FLAG_SHOW_PROTOCOL :
			0;
	const size_t channel_length = rf_rdp_dvc_write_channel_pdu_with_flags(
		channel,
		sizeof(channel),
		payload,
		payload_length,
		extra_flags
	);

	if (client->drdynvc_channel_id == 0 || channel_length == 0 ||
	    channel_length > RDP_MAX_MCS_PAYLOAD)
		return false;

	g_autofree uint8_t *packet = g_malloc0(15 + channel_length);
	const size_t packet_length = rf_rdp_mcs_write_send_data_indication(
		packet,
		15 + channel_length,
		RF_RDP_MCS_BASE_CHANNEL_ID,
		client->drdynvc_channel_id,
		channel,
		channel_length
	);

	if (packet_length == 0 || !client_write_exact(client, packet, packet_length))
		return false;
	if (bytes_sent != NULL)
		*bytes_sent += packet_length;
	return true;
}

static bool send_drdynvc_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	return send_drdynvc_payload_counted(client, payload, payload_length, NULL);
}

static bool send_static_channel_payload(
	struct client *client,
	uint16_t channel_id,
	uint32_t channel_options,
	const uint8_t *payload,
	size_t payload_length
)
{
	const uint32_t extra_flags =
		(channel_options & RF_RDP_MCS_CHANNEL_OPTION_SHOW_PROTOCOL) != 0 ?
			RF_RDP_DVC_CHANNEL_FLAG_SHOW_PROTOCOL :
			0;
	const size_t chunk_capacity = MIN(
		(size_t)RF_RDP_DVC_CHANNEL_CHUNK_LENGTH,
		(size_t)RDP_MAX_MCS_PAYLOAD - 8
	);
	size_t offset = 0;

	if (channel_id == 0 || (payload == NULL && payload_length > 0) ||
	    payload_length > UINT32_MAX || chunk_capacity == 0)
		return false;

	do {
		uint8_t channel[8 + RF_RDP_DVC_CHANNEL_CHUNK_LENGTH] = { 0 };
		const size_t chunk_length = MIN(
			chunk_capacity,
			payload_length - offset
		);
		uint32_t flags = extra_flags;

		if (offset == 0)
			flags |= RF_RDP_DVC_CHANNEL_FLAG_FIRST;
		if (offset + chunk_length == payload_length)
			flags |= RF_RDP_DVC_CHANNEL_FLAG_LAST;
		write_u32_le(channel, payload_length);
		write_u32_le(channel + 4, flags);
		if (chunk_length > 0)
			memcpy(channel + 8, payload + offset, chunk_length);

		const size_t channel_length = 8 + chunk_length;
		g_autofree uint8_t *packet = g_malloc0(15 + channel_length);
		const size_t packet_length = rf_rdp_mcs_write_send_data_indication(
			packet,
			15 + channel_length,
			RF_RDP_MCS_BASE_CHANNEL_ID,
			channel_id,
			channel,
			channel_length
		);

		if (packet_length == 0 ||
		    !client_write_exact(client, packet, packet_length))
			return false;
		offset += chunk_length;
	} while (offset < payload_length);

	return true;
}

static bool send_rdpsnd_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (!client->server->audio || client->rdpsnd_channel_id == 0)
		return false;
	return send_static_channel_payload(
		client,
		client->rdpsnd_channel_id,
		client->rdpsnd_channel_options,
		payload,
		payload_length
	);
}

static bool send_cliprdr_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (!client->server->clipboard || client->cliprdr_channel_id == 0)
		return false;
	return send_static_channel_payload(
		client,
		client->cliprdr_channel_id,
		client->cliprdr_channel_options,
		payload,
		payload_length
	);
}

static bool send_cliprdr_caps(struct client *client)
{
	uint8_t pdu[64] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_caps(pdu, sizeof(pdu));

	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	client->cliprdr_caps_sent = true;
	g_message(
		"RDP: cliprdr sent capabilities on static channel %u options=0x%08x.",
		client->cliprdr_channel_id,
		client->cliprdr_channel_options
	);
	return true;
}

static bool send_cliprdr_monitor_ready(struct client *client)
{
	uint8_t pdu[16] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_monitor_ready(
		pdu,
		sizeof(pdu)
	);

	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	client->cliprdr_ready = true;
	g_message("RDP: cliprdr sent monitor ready.");
	return true;
}

static bool clipboard_formats_locked(
	RfRDPServer *this,
	struct rf_rdp_cliprdr_format_list *formats
)
{
	memset(formats, 0, sizeof(*formats));
	if (this->clipboard_text != NULL ||
	    this->rich_clipboard.text != NULL) {
		formats->unicode_text = true;
		formats->text = true;
		formats->oem_text = true;
		formats->locale = true;
	}
	if (this->rich_clipboard.html != NULL) {
		formats->html = true;
		formats->html_format_id = RF_RDP_CLIPRDR_FORMAT_HTML;
	}
	if (this->rich_clipboard.image_rgba != NULL) {
		rf_rdp_cliprdr_set_server_image_formats(formats);
	}
	return formats->unicode_text || formats->html ||
	       formats->dib || formats->dibv5;
}

static bool send_cliprdr_format_list_with_formats(
	struct client *client,
	const struct rf_rdp_cliprdr_format_list *formats
)
{
	uint8_t pdu[512] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_list_for_formats(
		pdu,
		sizeof(pdu),
		formats
	);

	if (!client->cliprdr_ready)
		return false;
	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	g_message(
		"RDP: cliprdr advertised formats text=%s html=%s dib=%s dibv5=%s.",
		yes_no(formats->unicode_text),
		yes_no(formats->html),
		yes_no(formats->dib),
		yes_no(formats->dibv5)
	);
	return true;
}

static bool send_cliprdr_format_list_response(struct client *client, bool ok)
{
	uint8_t pdu[16] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_list_response(
		pdu,
		sizeof(pdu),
		ok
	);

	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	g_message("RDP: cliprdr sent format list response ok=%s.", yes_no(ok));
	return true;
}

static bool send_cliprdr_format_data_request(
	struct client *client,
	uint32_t format_id
)
{
	uint8_t pdu[16] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_data_request(
		pdu,
		sizeof(pdu),
		format_id
	);

	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	client->cliprdr_requested_format_id = format_id;
	g_message("RDP: cliprdr requested format id %u.", format_id);
	return true;
}

static bool request_next_cliprdr_format(struct client *client, const char *reason)
{
	const uint32_t previous_format_id = client->cliprdr_requested_format_id;
	const uint32_t next_format_id =
		rf_rdp_cliprdr_choose_request_format_after(
			&client->cliprdr_client_formats,
			previous_format_id
		);

	if (next_format_id == 0) {
		g_message(
			"RDP: cliprdr has no fallback format after %u (%s).",
			previous_format_id,
			reason != NULL ? reason : "unknown reason"
		);
		return false;
	}
	g_message(
		"RDP: cliprdr falling back from format %u to %u after %s.",
		previous_format_id,
		next_format_id,
		reason != NULL ? reason : "unknown reason"
	);
	if (!send_cliprdr_format_data_request(client, next_format_id)) {
		g_warning(
			"RDP: Failed to request fallback cliprdr format %u.",
			next_format_id
		);
		return false;
	}
	return true;
}

static bool send_cliprdr_format_data_response_text(
	struct client *client,
	const char *text
)
{
	if (text == NULL) {
		uint8_t fail[16] = { 0 };
		const size_t fail_length =
			rf_rdp_cliprdr_write_format_data_response_fail(
				fail,
				sizeof(fail)
			);

		if (fail_length == 0 || !send_cliprdr_payload(client, fail, fail_length))
			return false;
		g_message("RDP: cliprdr sent text data response failure.");
		return true;
	}

	const size_t capacity =
		RF_RDP_CLIPRDR_HEADER_SIZE + (strlen(text) + 1) * 2;
	g_autofree uint8_t *pdu = g_malloc0(capacity);
	const size_t length = rf_rdp_cliprdr_write_format_data_response_text(
		pdu,
		capacity,
		text
	);

	if (length == 0)
		return false;
	if (length > RF_RDP_DVC_CHANNEL_CHUNK_LENGTH) {
		g_warning(
			"RDP: cliprdr text response is too large for single static channel PDU."
		);
		return false;
	}
	if (!send_cliprdr_payload(client, pdu, length))
		return false;
	g_message(
		"RDP: cliprdr sent Unicode text response length %zu.",
		strlen(text)
	);
	return true;
}

static bool send_cliprdr_format_data_response_utf8_text(
	struct client *client,
	const char *text
)
{
	if (text == NULL)
		return send_cliprdr_format_data_response_text(client, NULL);

	const size_t capacity = RF_RDP_CLIPRDR_HEADER_SIZE + strlen(text) + 1;
	g_autofree uint8_t *pdu = g_malloc0(capacity);
	const size_t length =
		rf_rdp_cliprdr_write_format_data_response_utf8_text(
			pdu,
			capacity,
			text
		);

	if (length == 0)
		return false;
	if (!send_cliprdr_payload(client, pdu, length))
		return false;
	g_message(
		"RDP: cliprdr sent UTF-8 text response length %zu.",
		strlen(text)
	);
	return true;
}

static bool send_cliprdr_format_data_response_locale(
	struct client *client,
	uint32_t locale_id
)
{
	uint8_t pdu[16] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_data_response_locale(
		pdu,
		sizeof(pdu),
		locale_id
	);

	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	g_message("RDP: cliprdr sent locale response 0x%08x.", locale_id);
	return true;
}

static bool send_cliprdr_format_data_response_bytes(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length,
	const char *name
)
{
	const size_t capacity = RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;
	g_autofree uint8_t *pdu = NULL;
	size_t length = 0;

	if (payload == NULL)
		return send_cliprdr_format_data_response_text(client, NULL);
	pdu = g_malloc0(capacity);
	length = rf_rdp_cliprdr_write_format_data_response_bytes(
		pdu,
		capacity,
		payload,
		payload_length
	);
	if (length == 0 || !send_cliprdr_payload(client, pdu, length))
		return false;
	g_message("RDP: cliprdr sent %s response length %zu.", name, payload_length);
	return true;
}

static bool send_rdp_clipboard_wire(
	RfRDPServer *this,
	const GByteArray *wire
)
{
	unsigned int helpers = 0;

	if (wire == NULL || wire->len == 0 || this->clipboard_sockets == NULL)
		return false;

	GHashTableIter it;
	void *key;
	void *value;
	g_hash_table_iter_init(&it, this->clipboard_sockets);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		GSocket *socket = key;
		GSource *source = value;
		g_autoptr(GError) error = NULL;
		g_autoptr(GSocketConnection) connection =
			g_socket_connection_factory_create_connection(socket);
		GOutputStream *output =
			g_io_stream_get_output_stream(G_IO_STREAM(connection));
		gsize written = 0;
		ssize_t ret = rf_send_header(
			connection,
			RF_MSG_TYPE_RDP_CLIPBOARD_RICH,
			wire->len,
			&error
		);

		if (ret <= 0 ||
		    !g_output_stream_write_all(
			    output,
			    wire->data,
			    wire->len,
			    &written,
			    NULL,
			    &error
		    ) || written != wire->len) {
			if (ret < 0 || error != NULL)
				g_warning(
					"RDP: Failed to send rich clipboard to helper: %s.",
					error != NULL ? error->message : "short write"
				);
			g_source_destroy(source);
			g_hash_table_iter_remove(&it);
			continue;
		}
		helpers++;
	}
	g_message("RDP: Sent rich clipboard payload length %u to %u helper(s).",
		wire->len,
		helpers);
	return helpers > 0;
}

static bool send_rdp_clipboard_payload(RfRDPServer *this, bool force)
{
	g_autoptr(GByteArray) wire = NULL;
	bool duplicate = false;

	g_mutex_lock(&this->lock);
	wire = rf_clipboard_rich_payload_serialize(&this->rich_clipboard);
	duplicate = !force && rf_clipboard_rich_wire_equal(this->clipboard_wire, wire);
	if (wire == NULL) {
		g_clear_pointer(&this->clipboard_wire, g_byte_array_unref);
	} else if (!duplicate) {
		g_clear_pointer(&this->clipboard_wire, g_byte_array_unref);
		this->clipboard_wire = g_byte_array_ref(wire);
	}
	g_mutex_unlock(&this->lock);
	if (duplicate) {
		g_message("RDP: Skipping duplicate rich clipboard payload.");
		return true;
	}
	return send_rdp_clipboard_wire(this, wire);
}

static bool send_rdpgfx_dvc_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length,
	size_t *bytes_sent
)
{
	uint8_t dvc[RF_RDP_DVC_CHANNEL_CHUNK_LENGTH] = { 0 };
	size_t offset = 0;
	size_t dvc_length = 0;

	if (client->rdpgfx_channel_id == 0 || payload == NULL ||
	    payload_length == 0 || payload_length > UINT32_MAX)
		return false;

	dvc_length = rf_rdp_dvc_write_data(
		dvc,
		sizeof(dvc),
		client->rdpgfx_channel_id,
		payload,
		payload_length
	);
	if (dvc_length > 0)
		return send_drdynvc_payload_counted(
			client,
			dvc,
			dvc_length,
			bytes_sent
		);

	dvc_length = rf_rdp_dvc_write_data_first(
		dvc,
		sizeof(dvc),
		client->rdpgfx_channel_id,
		(uint32_t)payload_length,
		payload,
		0
	);
	if (dvc_length == 0 || dvc_length >= sizeof(dvc))
		return false;
	const size_t first_payload_length = MIN(payload_length, sizeof(dvc) - dvc_length);
	dvc_length = rf_rdp_dvc_write_data_first(
		dvc,
		sizeof(dvc),
		client->rdpgfx_channel_id,
		(uint32_t)payload_length,
		payload,
		first_payload_length
	);
	if (dvc_length == 0 || !send_drdynvc_payload_counted(
		    client,
		    dvc,
		    dvc_length,
		    bytes_sent
	    ))
		return false;

	if (!client->rdpgfx_fragment_logged) {
		g_message(
			"RDP: RDPGFX DVC fragmentation active for %zu-byte payloads.",
			payload_length
		);
		client->rdpgfx_fragment_logged = true;
	}

	offset = first_payload_length;
	while (offset < payload_length) {
		dvc_length = rf_rdp_dvc_write_data(
			dvc,
			sizeof(dvc),
			client->rdpgfx_channel_id,
			payload + offset,
			0
		);
		if (dvc_length == 0 || dvc_length >= sizeof(dvc))
			return false;
		const size_t chunk_length = MIN(
			payload_length - offset,
			sizeof(dvc) - dvc_length
		);
		dvc_length = rf_rdp_dvc_write_data(
			dvc,
			sizeof(dvc),
			client->rdpgfx_channel_id,
			payload + offset,
			chunk_length
		);
		if (dvc_length == 0 || !send_drdynvc_payload_counted(
			    client,
			    dvc,
			    dvc_length,
			    bytes_sent
		    ))
			return false;
		offset += chunk_length;
	}

	return true;
}

static size_t rdpgfx_zgfx_capacity(size_t payload_length)
{
	if (payload_length > UINT32_MAX)
		return 0;
	if (payload_length <= RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE)
		return payload_length + 2;

	const size_t segment_count =
		(payload_length + RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE - 1) /
		RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE;
	if (segment_count > UINT16_MAX ||
	    segment_count > (SIZE_MAX - 7 - payload_length) / 5)
		return 0;
	return 7 + payload_length + segment_count * 5;
}

static bool send_rdpgfx_gfx_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length,
	size_t *bytes_sent,
	bool allow_compression
)
{
	bool compressed = false;
	const size_t zgfx_capacity = rdpgfx_zgfx_capacity(payload_length);
	if (zgfx_capacity == 0)
		return false;

	g_autofree uint8_t *zgfx = g_malloc(zgfx_capacity);
	const size_t zgfx_length = rf_rdp_gfx_write_zgfx_payload(
		zgfx,
		zgfx_capacity,
		payload,
		payload_length,
		allow_compression,
		&compressed
	);

	if (zgfx_length == 0 ||
	    !send_rdpgfx_dvc_payload(client, zgfx, zgfx_length, bytes_sent))
		return false;

	if (compressed) {
		client->server->stats_rdpgfx_zgfx_payloads++;
		if (payload_length > zgfx_length)
			client->server->stats_rdpgfx_zgfx_saved_bytes +=
				payload_length - zgfx_length;
		if (!client->rdpgfx_zgfx_compressed_logged) {
			g_message(
				"RDP: RDPGFX ZGFX compression active for bitmap/PLANAR fallback payloads."
			);
			client->rdpgfx_zgfx_compressed_logged = true;
		}
	}
	return true;
}

static bool send_rdpgfx_create_request(struct client *client)
{
	uint8_t pdu[128] = { 0 };
	const size_t length = rf_rdp_dvc_write_create_request(
		pdu,
		sizeof(pdu),
		client->rdpgfx_channel_id,
		RF_RDP_DVC_RDPGFX_CHANNEL_NAME
	);

	if (length == 0 || !send_drdynvc_payload(client, pdu, length))
		return false;

	client->rdpgfx_create_sent = true;
	g_message("RDP: DVC sent RDPGFX create request channel-id=%u.",
		client->rdpgfx_channel_id);
	return true;
}

static void start_drdynvc(struct client *client)
{
	uint8_t pdu[32] = { 0 };
	const size_t length = rf_rdp_dvc_write_capability_request(
		pdu,
		sizeof(pdu),
		RF_RDP_DVC_VERSION_3
	);

	if (client->drdynvc_channel_id == 0) {
		g_message("RDP: Client did not advertise drdynvc; RDPGFX disabled.");
		return;
	}
	if (length == 0 || !send_drdynvc_payload(client, pdu, length)) {
		g_warning("RDP: Failed to send drdynvc capability request.");
		return;
	}

	client->drdynvc_capability_sent = true;
	g_message("RDP: DVC sent capability request on static channel %u.",
		client->drdynvc_channel_id);
}

static struct rf_rdp_rdpsnd_audio_format rdpsnd_pcm_format(
	unsigned int sample_rate,
	unsigned int channels
)
{
	return (struct rf_rdp_rdpsnd_audio_format){
		.tag = RF_RDP_RDPSND_WAVE_FORMAT_PCM,
		.channels = channels,
		.samples_per_sec = sample_rate,
		.avg_bytes_per_sec = sample_rate * channels * 2,
		.block_align = channels * 2,
		.bits_per_sample = 16
	};
}

static bool rdp_audio_prefers_adpcm(const RfRDPServer *this)
{
	return this != NULL && g_strcmp0(this->audio_codec, "pcm") != 0;
}

static bool rdp_audio_advertises_adpcm(const RfRDPServer *this)
{
	return this != NULL &&
	       (g_strcmp0(this->audio_codec, "auto") == 0 ||
		g_strcmp0(this->audio_codec, "adpcm") == 0);
}

static char *rdpsnd_format_array_summary(
	const struct rf_rdp_rdpsnd_audio_format *formats,
	size_t format_count
)
{
	GString *summary = g_string_new(NULL);

	for (size_t i = 0; i < format_count; ++i) {
		const struct rf_rdp_rdpsnd_audio_format *format = &formats[i];

		if (i > 0)
			g_string_append(summary, "; ");
		g_string_append_printf(
			summary,
			"%zu:%s tag=0x%04x %uHz/%uch/%ubit align=%u avg=%u extra=%u",
			i,
			rf_rdp_rdpsnd_format_name(format->tag),
			format->tag,
			format->samples_per_sec,
			format->channels,
			format->bits_per_sample,
			format->block_align,
			format->avg_bytes_per_sec,
			format->extra_size
		);
	}
	return g_string_free(summary, false);
}

static char *rdpsnd_client_formats_summary(
	const struct rf_rdp_rdpsnd_client_formats *formats
)
{
	if (formats == NULL)
		return g_strdup("");
	return rdpsnd_format_array_summary(
		formats->formats,
		formats->format_count
	);
}

static bool start_rdpsnd(struct client *client)
{
	RfRDPServer *this = client->server;
	uint8_t pdu[256] = { 0 };
	struct rf_rdp_rdpsnd_audio_format formats[4] = { 0 };
	g_autofree char *summary = NULL;
	size_t format_count = 0;
	size_t length = 0;

	if (!this->audio)
		return false;
	if (client->rdpsnd_channel_id == 0) {
		g_message("RDP: Client did not advertise rdpsnd; audio disabled.");
		return false;
	}

	if (rdp_audio_advertises_adpcm(this))
		formats[format_count++] =
			rf_rdp_rdpsnd_make_dvi_adpcm_format(
				this->audio_sample_rate,
				this->audio_channels,
				rf_rdp_rdpsnd_dvi_adpcm_samples_per_block(
					this->audio_sample_rate,
					this->audio_frame_ms
				)
			);
	formats[format_count++] = rdpsnd_pcm_format(
		this->audio_sample_rate,
		this->audio_channels
	);
	if (this->audio_sample_rate != 44100)
		formats[format_count++] = rdpsnd_pcm_format(44100, this->audio_channels);

	length = rf_rdp_rdpsnd_write_server_formats(
		pdu,
		sizeof(pdu),
		formats,
		format_count,
		client->rdpsnd_block_no
	);
	if (length == 0 || !send_rdpsnd_payload(client, pdu, length)) {
		g_warning("RDP: Failed to send rdpsnd server formats.");
		return false;
	}

	client->rdpsnd_formats_sent = true;
	summary = rdpsnd_format_array_summary(formats, format_count);
	g_message(
		"RDP: rdpsnd sent %zu server audio format(s), preferred=%u Hz/%u channel(s), codec=%s, formats=[%s].",
		format_count,
		this->audio_sample_rate,
		this->audio_channels,
		this->audio_codec,
		summary
	);
	return true;
}

static bool send_rdpsnd_audio(
	struct client *client,
	const uint8_t *pcm,
	size_t pcm_length,
	uint16_t timestamp,
	const struct rf_rdp_rdpsnd_audio_format *format,
	size_t *payload_length_sent
)
{
	uint8_t info[RF_RDP_RDPSND_WAVE_INFO_LENGTH] = { 0 };
	const uint8_t *payload = pcm;
	size_t payload_length = pcm_length;
	g_autofree uint8_t *wave2 = NULL;
	g_autofree uint8_t *wave_data = NULL;
	g_autofree uint8_t *encoded = NULL;
	size_t info_length = 0;
	size_t data_length = 0;
	const bool use_wave2 =
		client->rdpsnd_client_formats.version >=
		RF_RDP_RDPSND_CHANNEL_VERSION_WIN_MAX;

	if (!client->rdpsnd_ready || client->rdpsnd_selected_format < 0 ||
	    pcm == NULL || pcm_length < 4 || format == NULL)
		return false;
	if (payload_length_sent != NULL)
		*payload_length_sent = 0;

	if (format->tag == RF_RDP_RDPSND_WAVE_FORMAT_DVI_ADPCM) {
		encoded = g_malloc0(format->block_align);
		payload_length = rf_rdp_rdpsnd_encode_dvi_adpcm(
			encoded,
			format->block_align,
			pcm,
			pcm_length,
			format
		);
		if (payload_length == 0)
			return false;
		payload = encoded;
	}

	if (use_wave2) {
		wave2 = g_malloc(16 + payload_length);
		data_length = rf_rdp_rdpsnd_write_wave2(
			wave2,
			16 + payload_length,
			client->rdpsnd_block_no,
			(uint16_t)client->rdpsnd_selected_format,
			timestamp,
			timestamp,
			payload,
			payload_length
		);
		if (data_length == 0 ||
		    !send_rdpsnd_payload(client, wave2, data_length))
			return false;
		goto sent;
	}

	info_length = rf_rdp_rdpsnd_write_wave_info(
		info,
		sizeof(info),
		client->rdpsnd_block_no,
		(uint16_t)client->rdpsnd_selected_format,
		timestamp,
		payload,
		payload_length
	);
	if (info_length == 0 || !send_rdpsnd_payload(client, info, info_length))
		return false;

	if (payload_length > 4) {
		wave_data = g_malloc(payload_length);
		data_length = rf_rdp_rdpsnd_write_wave_data(
			wave_data,
			payload_length,
			payload,
			payload_length
		);
		if (data_length == 0 ||
		    !send_rdpsnd_payload(client, wave_data, data_length))
			return false;
	}

sent:
	if (!client->rdpsnd_first_audio_sent) {
		client->rdpsnd_first_audio_sent = true;
		client->rdpsnd_first_audio_block_no = client->rdpsnd_block_no;
		client->rdpsnd_first_audio_sent_time_us = g_get_monotonic_time();
		g_message(
			"RDP: rdpsnd first audio frame sent using %s block=%u timestamp=%u format=%d codec=%s bytes=%zu pcm-bytes=%zu.",
			use_wave2 ? "Wave2" : "Wave",
			client->rdpsnd_block_no,
			timestamp,
			client->rdpsnd_selected_format,
			rf_rdp_rdpsnd_format_name(format->tag),
			payload_length,
			pcm_length
		);
	}
	client->rdpsnd_block_no++;
	if (payload_length_sent != NULL)
		*payload_length_sent = payload_length;
	return true;
}

static const struct rf_rdp_rdpsnd_audio_format *client_rdpsnd_selected_format(
	const struct client *client
)
{
	if (client == NULL || client->rdpsnd_selected_format < 0 ||
	    (size_t)client->rdpsnd_selected_format >=
		    client->rdpsnd_client_formats.format_count)
		return NULL;
	return &client->rdpsnd_client_formats.formats[client->rdpsnd_selected_format];
}

static bool audio_frame_matches_client_format(
	const struct rf_rdp_audio_pcm_header *header,
	const struct rf_rdp_rdpsnd_audio_format *format
)
{
	if (header == NULL || format == NULL)
		return false;
	return format->tag == RF_RDP_RDPSND_WAVE_FORMAT_PCM &&
	       header->sample_rate == format->samples_per_sec &&
	       header->channels == format->channels &&
	       format->bits_per_sample == 16 &&
	       format->block_align == header->channels * 2;
}

static bool audio_frame_can_encode_client_format(
	const struct rf_rdp_audio_pcm_header *header,
	const struct rf_rdp_rdpsnd_audio_format *format
)
{
	if (header == NULL || format == NULL)
		return false;
	if (format->tag == RF_RDP_RDPSND_WAVE_FORMAT_PCM)
		return audio_frame_matches_client_format(header, format);
	if (format->tag == RF_RDP_RDPSND_WAVE_FORMAT_DVI_ADPCM)
		return header->sample_rate == format->samples_per_sec &&
		       header->channels == format->channels &&
		       format->bits_per_sample == 4 &&
		       format->block_align > 0;
	return false;
}

static void audio_client_target_clear(struct audio_client_target *target)
{
	if (target == NULL)
		return;
	g_clear_pointer(&target->client, client_unref);
	target->has_format = false;
}

static void update_audio_stats(
	RfRDPServer *this,
	const struct rf_rdp_audio_pcm_header *header,
	unsigned int clients,
	unsigned int sent,
	unsigned int dropped,
	uint64_t skipped,
	uint64_t latency_us,
	uint64_t payload_bytes
)
{
	const int64_t now = g_get_monotonic_time();

	g_mutex_lock(&this->audio_lock);
	this->audio_stats_frames += sent;
	this->audio_stats_bytes += payload_bytes;
	this->audio_stats_dropped += dropped;
	this->audio_stats_skipped_frames += skipped;
	if (latency_us > 0) {
		this->audio_stats_latency_us += latency_us;
		this->audio_stats_latency_samples++;
		this->audio_stats_max_latency_us =
			MAX(this->audio_stats_max_latency_us, latency_us);
	}
	this->audio_stats_clients = clients;
	if (this->audio_stats_last_log_time_us == 0)
		this->audio_stats_last_log_time_us = now;
	if (now - this->audio_stats_last_log_time_us >=
	    RDP_AUDIO_STATS_INTERVAL_US) {
		const uint64_t avg_latency_us =
			this->audio_stats_latency_samples > 0 ?
				this->audio_stats_latency_us /
					this->audio_stats_latency_samples :
				0;
		g_message(
			"RDP: Audio stats frames=%" G_GUINT64_FORMAT ", bytes=%" G_GUINT64_FORMAT ", dropped=%" G_GUINT64_FORMAT ", skipped=%" G_GUINT64_FORMAT ", silence-suppressed=%" G_GUINT64_FORMAT ", clients=%u, latency-avg=%" G_GUINT64_FORMAT "ms latency-max=%" G_GUINT64_FORMAT "ms, confirms=%" G_GUINT64_FORMAT ", first-confirm=%" G_GUINT64_FORMAT "ms, format=%uHz/%uch frame=%ums.",
			this->audio_stats_frames,
			this->audio_stats_bytes,
			this->audio_stats_dropped,
			this->audio_stats_skipped_frames,
			this->audio_stats_silence_suppressed,
			this->audio_stats_clients,
			avg_latency_us / 1000u,
			this->audio_stats_max_latency_us / 1000u,
			this->audio_stats_wave_confirms,
			this->audio_stats_first_confirm_us / 1000u,
			header->sample_rate,
			header->channels,
			header->frame_ms
		);
		this->audio_stats_frames = 0;
		this->audio_stats_bytes = 0;
		this->audio_stats_dropped = 0;
		this->audio_stats_latency_us = 0;
		this->audio_stats_latency_samples = 0;
		this->audio_stats_max_latency_us = 0;
		this->audio_stats_skipped_frames = 0;
		this->audio_stats_silence_suppressed = 0;
		this->audio_stats_wave_confirms = 0;
		this->audio_stats_first_confirm_us = 0;
		this->audio_stats_last_log_time_us = now;
	}
	g_mutex_unlock(&this->audio_lock);
}

static void send_audio_to_clients(
	RfRDPServer *this,
	const struct rf_rdp_audio_pcm_header *header,
	const GByteArray *pcm,
	uint64_t skipped
)
{
	g_autoptr(GArray) targets = NULL;
	unsigned int attempted = 0;
	unsigned int sent = 0;
	unsigned int dropped = 0;
	uint64_t payload_bytes = 0;
	const int64_t now = g_get_monotonic_time();
	const uint64_t latency_us =
		header->timestamp_us > 0 && (uint64_t)now > header->timestamp_us ?
			(uint64_t)now - header->timestamp_us :
			0;
	const uint16_t timestamp = (uint16_t)(header->timestamp_us / 1000u);

	if (pcm == NULL || header == NULL)
		return;

	targets = g_array_new(false, false, sizeof(struct audio_client_target));
	g_array_set_clear_func(targets, (GDestroyNotify)audio_client_target_clear);
	g_mutex_lock(&this->lock);
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;
		const struct rf_rdp_rdpsnd_audio_format *format = NULL;
		struct audio_client_target target = { 0 };

		if (!client->rdpsnd_ready)
			continue;
		format = client_rdpsnd_selected_format(client);
		attempted++;
		if (format == NULL ||
		    !audio_frame_can_encode_client_format(header, format)) {
			dropped++;
			if (format != NULL)
			g_debug(
				"RDP: Dropping audio frame %u Hz/%u ch; selected client format is %s %u Hz/%u ch.",
				header->sample_rate,
				header->channels,
				rf_rdp_rdpsnd_format_name(format->tag),
				format->samples_per_sec,
				format->channels
				);
			continue;
		}
		target.client = client_ref(client);
		target.format = *format;
		target.has_format = true;
		g_array_append_val(targets, target);
	}
	g_mutex_unlock(&this->lock);

	for (guint i = 0; i < targets->len; ++i) {
		struct audio_client_target *target =
			&g_array_index(targets, struct audio_client_target, i);

		if (target->client == NULL || !target->has_format)
			continue;
		size_t sent_payload_length = 0;
		if (send_rdpsnd_audio(
			    target->client,
			    pcm->data,
			    pcm->len,
			    timestamp,
			    &target->format,
			    &sent_payload_length
		    )) {
			sent++;
			payload_bytes += sent_payload_length;
		}
		else
			dropped++;
	}
	update_audio_stats(
		this,
		header,
		attempted,
		sent,
		dropped,
		skipped,
		latency_us,
		payload_bytes
	);

	if (attempted > 0 && sent == 0)
		g_debug("RDP: Failed to send audio frame to %u ready client(s).",
			attempted);
}

static void maybe_send_cliprdr_cached_format_list(struct client *client)
{
	RfRDPServer *this = client->server;
	struct rf_rdp_cliprdr_format_list formats = { 0 };
	bool has_formats = false;

	if (!client->cliprdr_ready || !client->cliprdr_client_caps_received)
		return;

	g_mutex_lock(&this->lock);
	has_formats = clipboard_formats_locked(this, &formats);
	g_mutex_unlock(&this->lock);

	if (has_formats && !send_cliprdr_format_list_with_formats(client, &formats))
		g_warning("RDP: Failed to send cliprdr format list.");
}

static void start_cliprdr(struct client *client)
{
	RfRDPServer *this = client->server;

	if (!this->clipboard)
		return;
	if (client->cliprdr_channel_id == 0) {
		g_message("RDP: Client did not advertise cliprdr; clipboard disabled.");
		return;
	}
	if (!send_cliprdr_caps(client)) {
		g_warning("RDP: Failed to send cliprdr capabilities.");
		return;
	}
	if (!send_cliprdr_monitor_ready(client))
		g_warning("RDP: Failed to send cliprdr monitor ready.");
	else
		maybe_send_cliprdr_cached_format_list(client);
}

static bool send_rdpgfx_caps_confirm(
	struct client *client,
	const struct rf_rdp_gfx_caps *caps
)
{
	RfRDPServer *this = client->server;
	const struct rf_rdp_gfx_server_codecs codecs = rdpgfx_server_codecs(this);
	const enum rf_rdp_gfx_codec policy_codec =
		rf_rdp_gfx_select_codec_policy(
			caps,
			&codecs,
			this->rdpgfx_video_quality_level > 0
		);
	const bool use_av1 = policy_codec == RF_RDP_GFX_CODEC_AV1;
	const enum rf_rdp_av1_mode av1_mode =
		use_av1 && caps->av1_i444 &&
			rdpgfx_av1_mode_supported(this, RF_RDP_AV1_MODE_I444) ?
			RF_RDP_AV1_MODE_I444 :
			RF_RDP_AV1_MODE_I420;
	const uint32_t confirm_version = use_av1 ?
		RF_RDP_GFX_CAPVERSION_FRDP_1 :
		caps->selected_version;
	const uint32_t confirm_flags = use_av1 ?
		(av1_mode == RF_RDP_AV1_MODE_I444 ?
		 (caps->av1_flags & ~RF_RDP_GFX_CAPS_FLAG_AV1_I444_DISABLED) :
		 (caps->av1_flags | RF_RDP_GFX_CAPS_FLAG_AV1_I444_DISABLED)) :
		caps->selected_flags;
	struct rf_rdp_gfx_caps confirmed_caps = *caps;
	uint8_t gfx[64] = { 0 };
	const size_t length = rf_rdp_gfx_write_caps_confirm(
		gfx,
		sizeof(gfx),
		confirm_version,
		confirm_flags
	);

	if (length == 0 ||
	    !send_rdpgfx_gfx_payload(client, gfx, length, NULL, false))
		return false;

	client->rdpgfx_caps_confirmed = true;
	client->rdpgfx_caps_version = confirm_version;
	client->rdpgfx_caps_flags = confirm_flags;
	client->rdpgfx_policy_codec = policy_codec;
	client->rdpgfx_av1_available = use_av1;
	client->av1_mode = av1_mode;
	client->rdpgfx_avc420_available = caps->avc420;
	client->rdpgfx_progressive_available = caps->progressive;
	client->rdpgfx_remotefx_available = caps->remotefx;
	confirmed_caps.selected_version = confirm_version;
	confirmed_caps.selected_flags = confirm_flags;
	confirmed_caps.av1_i444 =
		use_av1 && av1_mode == RF_RDP_AV1_MODE_I444;
	log_rdpgfx_codec_policy(&confirmed_caps, &codecs, policy_codec);
	if (use_av1)
		g_message(
			"RDP: RDPGFX AV1 negotiated mode=%s client-i444=%s flags=0x%08x.",
			av1_mode_name(client->av1_mode),
			yes_no(caps->av1_i444),
			confirm_flags
		);
	return true;
}

static void handle_rdpgfx_frame_acknowledge(
	struct client *client,
	const struct rf_rdp_gfx_frame_ack *ack
)
{
	RfRDPServer *this = client->server;

	g_mutex_lock(&this->lock);
	client->rdpgfx_frame_ack_received = true;
	client->rdpgfx_ack_queue_depth = ack->queue_depth;
	client->rdpgfx_last_ack_frame_id = ack->frame_id;
	client->rdpgfx_total_frames_decoded = ack->total_frames_decoded;
	if (!client->rdpgfx_frame_ack_logged) {
		g_message(
			"RDP: RDPGFX frame acknowledge active queue-depth=%u frame-id=%u decoded=%u.",
			ack->queue_depth,
			ack->frame_id,
			ack->total_frames_decoded
		);
		client->rdpgfx_frame_ack_logged = true;
	}
	g_mutex_unlock(&this->lock);
}

static void handle_rdpgfx_qoe_frame_acknowledge(
	struct client *client,
	const struct rf_rdp_gfx_qoe_frame_ack *ack
)
{
	RfRDPServer *this = client->server;

	g_mutex_lock(&this->lock);
	client->rdpgfx_qoe_frame_id = ack->frame_id;
	client->rdpgfx_qoe_timestamp = ack->timestamp;
	client->rdpgfx_qoe_time_diff_se = ack->time_diff_se;
	client->rdpgfx_qoe_time_diff_edr = ack->time_diff_edr;
	this->stats_max_rdpgfx_qoe_time_diff_se = MAX(
		this->stats_max_rdpgfx_qoe_time_diff_se,
		ack->time_diff_se
	);
	this->stats_max_rdpgfx_qoe_time_diff_edr = MAX(
		this->stats_max_rdpgfx_qoe_time_diff_edr,
		ack->time_diff_edr
	);
	if (!client->rdpgfx_qoe_ack_logged) {
		g_message(
			"RDP: RDPGFX QoE frame acknowledge active frame-id=%u se=%u edr=%u.",
			ack->frame_id,
			ack->time_diff_se,
			ack->time_diff_edr
		);
		client->rdpgfx_qoe_ack_logged = true;
	}
	g_mutex_unlock(&this->lock);
}

static void handle_rdpgfx_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	struct rf_rdp_gfx_caps caps = { 0 };
	struct rf_rdp_gfx_frame_ack frame_ack = { 0 };
	struct rf_rdp_gfx_qoe_frame_ack qoe_ack = { 0 };

	if (rf_rdp_gfx_parse_caps_advertise(payload, payload_length, &caps)) {
		log_rdpgfx_caps_advertise(&caps);
		if (!send_rdpgfx_caps_confirm(client, &caps))
			g_warning("RDP: Failed to send GFX CapsConfirm.");
		return;
	}
	if (rf_rdp_gfx_parse_frame_acknowledge(
		    payload,
		    payload_length,
		    &frame_ack
	    )) {
		handle_rdpgfx_frame_acknowledge(client, &frame_ack);
		return;
	}
	if (rf_rdp_gfx_parse_qoe_frame_acknowledge(
		    payload,
		    payload_length,
		    &qoe_ack
	    )) {
		handle_rdpgfx_qoe_frame_acknowledge(client, &qoe_ack);
		return;
	}

	g_debug("RDP: Ignoring unsupported RDPGFX payload length %zu.",
		payload_length);
}

static bool client_can_use_rdpgfx(const struct client *client)
{
	return client->rdpgfx_channel_open &&
	       client->rdpgfx_caps_confirmed &&
	       !client->rdpgfx_disabled;
}

static bool client_can_use_rdpgfx_avc420(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_AVC420 &&
	       client->rdpgfx_caps_version != RF_RDP_GFX_CAPVERSION_FRDP_1 &&
	       client->rdpgfx_avc420_available &&
	       (client->rdpgfx_caps_flags & RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED) == 0;
}

static bool client_can_use_rdpgfx_av1(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_AV1 &&
	       client->rdpgfx_av1_available &&
	       client->rdpgfx_caps_version == RF_RDP_GFX_CAPVERSION_FRDP_1;
}

static bool client_can_use_rdpgfx_avc444(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       (client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_AVC444 ||
		client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_AVC444_V2) &&
	       client->rdpgfx_caps_version != RF_RDP_GFX_CAPVERSION_FRDP_1 &&
	       client->rdpgfx_caps_version >= RF_RDP_GFX_CAPVERSION_10 &&
	       (client->rdpgfx_caps_flags & RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED) == 0;
}

static bool client_can_use_rdpgfx_rfx(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       client->rdpgfx_caps_version != RF_RDP_GFX_CAPVERSION_FRDP_1 &&
	       client->rdpgfx_remotefx_available;
}

static bool client_can_use_rdpgfx_progressive(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       client->rdpgfx_caps_version != RF_RDP_GFX_CAPVERSION_FRDP_1 &&
	       client->rdpgfx_progressive_available;
}

static bool client_needs_rdpgfx_surface(
	const struct client *client,
	unsigned int width,
	unsigned int height
)
{
	return client_can_use_rdpgfx(client) &&
	       (!client->rdpgfx_surface_ready ||
		client->rdpgfx_surface_width != width ||
			client->rdpgfx_surface_height != height);
}

static uint32_t client_rdpgfx_inflight_frames(const struct client *client)
{
	if (!client_can_use_rdpgfx(client) ||
	    !client->rdpgfx_frame_ack_received ||
	    client->rdpgfx_ack_queue_depth ==
		    RDP_RDPGFX_SUSPEND_FRAME_ACKNOWLEDGEMENT ||
	    client->rdpgfx_frame_id < client->rdpgfx_last_ack_frame_id)
		return 0;
	return client->rdpgfx_frame_id - client->rdpgfx_last_ack_frame_id;
}

static unsigned int client_video_encoder_quality_level(
	const struct client *client,
	bool encoder_ready,
	unsigned int encoder_quality_level,
	unsigned int requested_quality_level
)
{
	if (!encoder_ready)
		return requested_quality_level;
	if (rf_rdp_core_should_rebuild_video_encoder_for_quality(
		    encoder_quality_level,
		    requested_quality_level,
		    client->server->max_video_quality_level
	    ))
		return requested_quality_level;
	return encoder_quality_level;
}

static unsigned int rdpgfx_ack_limited_fps(
	RfRDPServer *this,
	unsigned int base_fps
)
{
	unsigned int fps = base_fps;

	if (fps == 0)
		return 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		const struct client *client = l->data;
		const uint32_t inflight = client_rdpgfx_inflight_frames(client);
		const bool ack_depth_valid =
			client_can_use_rdpgfx(client) &&
			client->rdpgfx_frame_ack_received &&
			client->rdpgfx_ack_queue_depth !=
				RDP_RDPGFX_SUSPEND_FRAME_ACKNOWLEDGEMENT;
		const unsigned int client_fps =
			rf_rdp_core_rdpgfx_ack_limited_fps(
				base_fps,
				ack_depth_valid ? client->rdpgfx_ack_queue_depth : 0,
				ack_depth_valid,
				inflight
			);

		if (client_fps < fps)
			fps = client_fps;
	}
	return fps;
}

static uint32_t max_rdpgfx_inflight_frames(RfRDPServer *this)
{
	uint32_t max_inflight = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		const struct client *client = l->data;
		const uint32_t inflight = client_rdpgfx_inflight_frames(client);

		if (inflight > max_inflight)
			max_inflight = inflight;
	}
	return max_inflight;
}

static uint32_t max_rdpgfx_ack_queue_depth(RfRDPServer *this)
{
	uint32_t max_depth = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		const struct client *client = l->data;

		if (client_can_use_rdpgfx(client) &&
		    client->rdpgfx_frame_ack_received &&
		    client->rdpgfx_ack_queue_depth !=
			    RDP_RDPGFX_SUSPEND_FRAME_ACKNOWLEDGEMENT &&
		    client->rdpgfx_ack_queue_depth > max_depth)
			max_depth = client->rdpgfx_ack_queue_depth;
	}
	return max_depth;
}

static const char *graphics_mode_name(enum rf_rdp_graphics_mode mode)
{
	switch (mode) {
	case RF_RDP_GRAPHICS_MODE_SURFACE_NSC:
		return "surface-nsc";
	case RF_RDP_GRAPHICS_MODE_BITMAP:
	default:
		return "bitmap";
	}
}

static const char *client_graphics_mode_name(const struct client *client)
{
	if (client_can_use_rdpgfx(client))
		return "rdpgfx";
	return graphics_mode_name(client->graphics_mode);
}

struct finalization_state {
	bool client_synchronize;
	bool client_control_cooperate;
	bool client_control_request;
	bool client_font_list;
};

static bool handle_client_control(
	GOutputStream *output,
	struct finalization_state *state,
	const uint8_t *body,
	size_t body_length
)
{
	struct rf_rdp_core_control_pdu control = { 0 };

	if (!rf_rdp_core_parse_control_body(body, body_length, &control))
		return false;

	switch (control.action) {
	case RF_RDP_CONTROL_ACTION_COOPERATE:
		if (!state->client_synchronize) {
			g_warning("RDP: Client Control Cooperate arrived before Synchronize.");
			return false;
		}
		if (control.grant_id != 0 || control.control_id != 0)
			return false;
		state->client_control_cooperate = true;
		return true;
	case RF_RDP_CONTROL_ACTION_REQUEST_CONTROL:
		if (!state->client_control_cooperate) {
			g_warning("RDP: Client requested control before Cooperate.");
			return false;
		}
		if (control.grant_id != 0 || control.control_id != 0)
			return false;
		state->client_control_request = true;
		return write_data_packet(
			rf_rdp_core_write_server_control_granted,
			output
		);
	default:
		g_warning("RDP: Unsupported client control action %u.", control.action);
		return false;
	}
}

static bool handle_client_finalization_pdu(
	GOutputStream *output,
	const GByteArray *pdu,
	struct finalization_state *state
)
{
	struct rf_rdp_core_pdu core_pdu = { 0 };
	const uint8_t *body = NULL;
	uint16_t target_user = 0;
	uint16_t font_flags = 0;

	if (!rf_rdp_core_parse_client_pdu(
		    pdu->data,
		    pdu->len,
		    &core_pdu
	    )) {
		g_warning("RDP: Failed to parse finalization client PDU.");
		return false;
	}
	if (core_pdu.share_type != RF_RDP_PDU_TYPE_DATA) {
		g_warning(
			"RDP: Expected finalization Data PDU, got share type %u.",
			core_pdu.share_type
		);
		return false;
	}

	body = pdu->data + core_pdu.payload_offset;
	switch (core_pdu.data_type) {
	case RF_RDP_DATA_PDU_TYPE_SYNCHRONIZE:
		if (!rf_rdp_core_parse_synchronize_body(
			    body,
			    core_pdu.payload_length,
			    &target_user
		    ))
			return false;
		state->client_synchronize = true;
		g_debug("RDP: Client Synchronize target user %u.", target_user);
		return true;
	case RF_RDP_DATA_PDU_TYPE_CONTROL:
		return handle_client_control(
			output,
			state,
			body,
			core_pdu.payload_length
		);
	case RF_RDP_DATA_PDU_TYPE_FONT_LIST:
		if (!state->client_control_request) {
			g_warning("RDP: Client Font List arrived before control grant.");
			return false;
		}
		if (!rf_rdp_core_parse_font_list_body(
			    body,
			    core_pdu.payload_length,
			    &font_flags
		    ))
			return false;
		state->client_font_list = true;
		g_debug("RDP: Client Font List flags 0x%04x.", font_flags);
		return write_data_packet(rf_rdp_core_write_server_font_map, output);
	default:
		g_debug(
			"RDP: Ignoring finalization Data PDU type 0x%02x.",
			core_pdu.data_type
		);
		return true;
	}
}

static bool handle_client_finalization(
	struct client *client,
	GOutputStream *output
)
{
	GInputStream *input = g_io_stream_get_input_stream(client->stream);
	struct finalization_state state = { 0 };

	if (!write_data_packet(
		    rf_rdp_core_write_server_synchronize,
		    output
	    ) || !write_data_packet(
		    rf_rdp_core_write_server_control_cooperate,
		    output
	    ))
		return false;

	while (!state.client_font_list) {
		g_autoptr(GByteArray) pdu = read_tpkt(input);

		if (pdu == NULL ||
		    !handle_client_finalization_pdu(output, pdu, &state))
			return false;
	}

	if (!write_data_packet(
		    rf_rdp_core_write_server_default_pointer,
		    output
	    ))
		return false;

	g_message("RDP: Activation finalization complete.");
	return true;
}

static bool handle_client_info_and_activation(
	struct client *client,
	GOutputStream *output,
	const GByteArray *client_info_pdu
)
{
	RfRDPServer *this = client->server;
	GInputStream *input = g_io_stream_get_input_stream(client->stream);
	struct rf_rdp_core_pdu core_pdu = { 0 };

	if (!rf_rdp_core_parse_client_pdu(
		    client_info_pdu->data,
		    client_info_pdu->len,
		    &core_pdu
	    ) || !(core_pdu.security_flags & RF_RDP_SEC_INFO_PKT)) {
		g_warning("RDP: Expected Client Info PDU after MCS channel joins.");
		return false;
	}

	if (!write_core_packet(
		    rf_rdp_core_write_license_valid_client,
		    output
	    ) || !write_demand_active_packet(client, output))
		return false;

	g_autoptr(GByteArray) confirm_pdu = read_tpkt(input);
	if (confirm_pdu == NULL ||
	    !rf_rdp_core_parse_client_pdu(
		    confirm_pdu->data,
		    confirm_pdu->len,
		    &core_pdu
	    ))
		return false;
	if (core_pdu.share_type != RF_RDP_PDU_TYPE_CONFIRM_ACTIVE) {
		g_warning(
			"RDP: Expected Confirm Active PDU, got share type %u.",
			core_pdu.share_type
		);
		return false;
	}

	if (!rf_rdp_core_parse_confirm_active_capabilities(
		    confirm_pdu->data + core_pdu.payload_offset,
		    core_pdu.payload_length,
		    &client->caps
	    )) {
		memset(&client->caps, 0, sizeof(client->caps));
		client->graphics_mode = RF_RDP_GRAPHICS_MODE_BITMAP;
		g_warning("RDP: Failed to parse Confirm Active capabilities; using bitmap graphics.");
	} else {
		client->graphics_mode = rf_rdp_core_select_graphics_mode(
			this->graphics,
			&client->caps,
			true
		);
		g_message(
			"RDP: Confirm Active received: surface set=%d stream=%d nsc=%d(id=%u) rfx=%d(id=%u).",
			client->caps.surface_set_bits,
			client->caps.surface_stream_bits,
			client->caps.nscodec,
			client->caps.nscodec_id,
			client->caps.remotefx,
			client->caps.remotefx_id
		);
		if (client->graphics_mode == RF_RDP_GRAPHICS_MODE_BITMAP &&
		    g_strcmp0(this->graphics, "bitmap") != 0 &&
		    client->caps.surface_set_bits && client->caps.nscodec &&
		    client->caps.nscodec_id != 0)
			g_message(
				"RDP: NSCodec SurfaceBits advertised, but bitmap fallback was selected."
			);
	}
	g_message("RDP: Negotiated graphics mode %s.",
		graphics_mode_name(client->graphics_mode));
	return handle_client_finalization(client, output);
}

static bool handle_mcs_connect_sequence(struct client *client)
{
	GInputStream *input = g_io_stream_get_input_stream(client->stream);
	GOutputStream *output = g_io_stream_get_output_stream(client->stream);
	g_autoptr(GByteArray) connect_initial = read_tpkt(input);
	struct rf_rdp_mcs_client_info client_info = { 0 };
	uint8_t response[1024] = { 0 };
	size_t response_length = 0;
	unsigned int join_count = 0;

	if (connect_initial == NULL)
		return false;
	if (rf_rdp_mcs_parse_connect_initial(
		    connect_initial->data,
		    connect_initial->len,
		    &client_info
	    )) {
		set_client_desktop_size(
			client,
			client_info.desktop_width,
			client_info.desktop_height
			);
			client->channel_count = client_info.channel_count;
			client->rdpsnd_channel_id = client_info.rdpsnd_channel_id;
			client->cliprdr_channel_id = client_info.cliprdr_channel_id;
			client->drdynvc_channel_id = client_info.drdynvc_channel_id;
			client->rdpsnd_channel_options =
				client_info.rdpsnd_channel_options;
			client->cliprdr_channel_options =
				client_info.cliprdr_channel_options;
			client->drdynvc_channel_options =
				client_info.drdynvc_channel_options;
			g_message("RDP: Client desktop is %ux%u with %u static channels.",
				client->desktop_width,
				client->desktop_height,
				client->channel_count);
			if (client->rdpsnd_channel_id != 0)
				g_message(
					"RDP: Client advertised rdpsnd static channel %u options=0x%08x.",
					client->rdpsnd_channel_id,
					client->rdpsnd_channel_options
				);
			if (client->cliprdr_channel_id != 0)
				g_message(
					"RDP: Client advertised cliprdr static channel %u options=0x%08x.",
					client->cliprdr_channel_id,
					client->cliprdr_channel_options
				);
			if (client->drdynvc_channel_id != 0)
				g_message(
					"RDP: Client advertised drdynvc static channel %u options=0x%08x.",
					client->drdynvc_channel_id,
					client->drdynvc_channel_options
				);
		} else {
			g_warning("RDP: Failed to parse client GCC data; using defaults.");
		}

	response_length = rf_rdp_mcs_write_connect_response_with_channels(
		response,
		sizeof(response),
		client->selected_protocol,
		client->channel_count
	);
	if (response_length == 0 ||
	    !write_exact(output, response, response_length))
		return false;

	for (;;) {
		g_autoptr(GByteArray) pdu = read_tpkt(input);
		struct rf_rdp_mcs_domain_pdu domain_pdu = { 0 };

		if (pdu == NULL ||
		    !rf_rdp_mcs_parse_domain_pdu(
			    pdu->data,
			    pdu->len,
			    &domain_pdu
		    ))
			return false;

		switch (domain_pdu.type) {
		case RF_RDP_MCS_PDU_ERECT_DOMAIN_REQUEST:
			break;
		case RF_RDP_MCS_PDU_ATTACH_USER_REQUEST:
			response_length = rf_rdp_mcs_write_attach_user_confirm(
				response,
				sizeof(response),
				RF_RDP_MCS_BASE_CHANNEL_ID
			);
			if (response_length == 0 ||
			    !write_exact(output, response, response_length))
				return false;
			break;
		case RF_RDP_MCS_PDU_CHANNEL_JOIN_REQUEST:
			response_length = rf_rdp_mcs_write_channel_join_confirm(
				response,
				sizeof(response),
				domain_pdu.user_id,
				domain_pdu.channel_id
			);
			if (response_length == 0 ||
			    !write_exact(output, response, response_length))
				return false;
			join_count++;
			break;
		case RF_RDP_MCS_PDU_SEND_DATA_REQUEST:
			g_message(
				"RDP: MCS connected after %u channel joins.",
				join_count
			);
			return handle_client_info_and_activation(
				client,
				output,
				pdu
			);
		default:
			g_warning(
				"RDP: Unsupported MCS domain PDU type %u.",
				domain_pdu.type
			);
			return false;
		}
	}
}

static void handle_keyboard_input(
	struct client *client,
	const struct rf_rdp_input_event *event
)
{
	const bool extended = event->flags & RF_RDP_KBD_FLAGS_EXTENDED;
	const bool down = !(event->flags & RF_RDP_KBD_FLAGS_RELEASE);
	const uint32_t keycode = rf_rdp_scancode_to_ev_key(
		event->param1 & 0xff,
		extended
	);

	if (keycode == 0)
		return;
	rf_remote_server_handle_keycode_event(
		RF_REMOTE_SERVER(client->server),
		keycode,
		down
	);
}

static void handle_pointer_input(
	struct client *client,
	const struct rf_rdp_input_event *event
)
{
	uint16_t width = 0;
	uint16_t height = 0;

	get_desktop_size(client, &width, &height);
	if (!rf_rdp_pointer_update(
		    &client->pointer,
		    event->flags,
		    event->param1,
		    event->param2,
		    width,
		    height
	    ))
		return;
	rf_remote_server_handle_pointer_state(
		RF_REMOTE_SERVER(client->server),
		client->pointer.rx,
		client->pointer.ry,
		client->pointer.left,
		client->pointer.middle,
		client->pointer.right,
		client->pointer.back,
		client->pointer.forward,
		client->pointer.wup,
		client->pointer.wdown,
		client->pointer.wleft,
		client->pointer.wright
	);
}

static void handle_input_pdu(
	struct client *client,
	const GByteArray *pdu,
	const struct rf_rdp_core_pdu *core_pdu
)
{
	const uint8_t *body = pdu->data + core_pdu->payload_offset;
	uint16_t count = 0;

	if (!rf_rdp_input_event_count(body, core_pdu->payload_length, &count))
		return;

	for (uint16_t i = 0; i < count; ++i) {
		struct rf_rdp_input_event event = { 0 };

		if (!rf_rdp_input_parse_event(
			    body,
			    core_pdu->payload_length,
			    i,
			    &event
		    ))
			return;

		switch (event.message_type) {
		case RF_RDP_INPUT_EVENT_SCANCODE:
			handle_keyboard_input(client, &event);
			break;
		case RF_RDP_INPUT_EVENT_MOUSE:
		case RF_RDP_INPUT_EVENT_MOUSEX:
			handle_pointer_input(client, &event);
			break;
		default:
			g_debug(
				"RDP: Ignoring input event type 0x%04x.",
				event.message_type
			);
			break;
		}
	}
}

static void handle_drdynvc_channel_pdu(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	struct rf_rdp_dvc_channel_pdu channel = { 0 };
	uint16_t version = 0;
	struct rf_rdp_dvc_create_response create = { 0 };
	struct rf_rdp_dvc_data_pdu data = { 0 };

	if (!rf_rdp_dvc_parse_channel_pdu(payload, payload_length, &channel)) {
		g_debug("RDP: Ignoring malformed drdynvc static channel PDU length %zu.",
			payload_length);
		return;
	}
	if ((channel.flags & (
		     RF_RDP_DVC_CHANNEL_FLAG_FIRST |
		     RF_RDP_DVC_CHANNEL_FLAG_LAST
	     )) != (
		     RF_RDP_DVC_CHANNEL_FLAG_FIRST |
		     RF_RDP_DVC_CHANNEL_FLAG_LAST
	     )) {
		g_debug("RDP: Ignoring fragmented drdynvc static channel PDU.");
		return;
	}

	payload += channel.payload_offset;
	payload_length = channel.payload_length;

	if (rf_rdp_dvc_parse_capability_response(
		    payload,
		    payload_length,
		    &version
	    )) {
		client->drdynvc_version = version;
		client->drdynvc_ready = true;
		g_message("RDP: DVC capability response version=%u.", version);
		if (!client->rdpgfx_create_sent &&
		    !send_rdpgfx_create_request(client))
			g_warning("RDP: Failed to send RDPGFX create request.");
		return;
	}

	if (rf_rdp_dvc_parse_create_response(
		    payload,
		    payload_length,
		    &create
	    )) {
		if (create.channel_id != client->rdpgfx_channel_id) {
			g_debug("RDP: Ignoring DVC create response for channel-id=%u.",
				create.channel_id);
			return;
		}
		if (create.status == 0) {
			client->rdpgfx_channel_open = true;
			g_message("RDP: RDPGFX dynamic channel opened id=%u.",
				create.channel_id);
		} else {
			client->rdpgfx_disabled = true;
			g_warning(
				"RDP: RDPGFX dynamic channel create failed id=%u status=0x%08x.",
				create.channel_id,
				create.status
			);
		}
		return;
	}

	if (rf_rdp_dvc_parse_data_pdu(payload, payload_length, &data)) {
		if (data.channel_id != client->rdpgfx_channel_id ||
		    !client->rdpgfx_channel_open) {
			g_debug("RDP: Ignoring DVC data channel-id=%u length=%zu.",
				data.channel_id,
				data.payload_length);
			return;
		}
		handle_rdpgfx_payload(
			client,
			payload + data.payload_offset,
			data.payload_length
		);
		return;
	}

	g_debug("RDP: Ignoring unsupported drdynvc payload length %zu.",
		payload_length);
}

static void handle_cliprdr_format_data_response(
	struct client *client,
	const uint8_t *data,
	size_t length,
	uint16_t msg_flags
)
{
	RfRDPServer *this = client->server;
	bool changed = false;

	if ((msg_flags & RF_RDP_CLIPRDR_CB_RESPONSE_OK) == 0) {
		g_message("RDP: cliprdr client format data response failed.");
		request_next_cliprdr_format(client, "client response failure");
		return;
	}

	if (client->cliprdr_requested_format_id == RF_RDP_CLIPRDR_CF_UNICODETEXT) {
		g_autofree char *text =
			rf_rdp_cliprdr_parse_format_data_response_text(
				data,
				length,
				msg_flags
			);

			if (text == NULL) {
				g_message(
					"RDP: cliprdr client format data response did not contain text."
				);
				request_next_cliprdr_format(client, "missing text data");
				return;
			}

		g_mutex_lock(&this->lock);
		if (g_strcmp0(this->clipboard_text, text) != 0) {
			g_free(this->clipboard_text);
			this->clipboard_text = g_strdup(text);
			rf_clipboard_rich_payload_clear(&this->rich_clipboard);
			rf_clipboard_rich_payload_set_text(
				&this->rich_clipboard,
				text
			);
			changed = true;
		}
		g_mutex_unlock(&this->lock);

		g_message(
			"RDP: cliprdr received Unicode text response length %zu changed=%s.",
			strlen(text),
			yes_no(changed)
		);
		if (changed)
			rf_remote_server_handle_clipboard_text(
				RF_REMOTE_SERVER(this),
				text
			);
		if (changed)
				send_rdp_clipboard_payload(this, false);
		return;
	}

	if (client->cliprdr_requested_format_id == RF_RDP_CLIPRDR_CF_DIB ||
	    client->cliprdr_requested_format_id == RF_RDP_CLIPRDR_CF_DIBV5) {
		uint32_t width = 0;
		uint32_t height = 0;
		size_t stride = 0;
		g_autoptr(GByteArray) rgba = rf_rdp_cliprdr_dib_to_rgba(
			data,
			length,
			&width,
			&height,
			&stride
		);

			if (rgba == NULL) {
				g_message("RDP: cliprdr image response is not supported DIB data.");
				request_next_cliprdr_format(client, "unsupported DIB data");
				return;
			}
		g_mutex_lock(&this->lock);
		g_free(this->clipboard_text);
		this->clipboard_text = NULL;
		rf_clipboard_rich_payload_clear(&this->rich_clipboard);
		changed = rf_clipboard_rich_payload_set_image_rgba(
			&this->rich_clipboard,
			rgba->data,
			rgba->len,
			width,
			height,
			stride
		);
		g_mutex_unlock(&this->lock);
		g_message(
			"RDP: cliprdr received image response %ux%u stride=%zu length=%u changed=%s.",
			width,
			height,
			stride,
			rgba->len,
			yes_no(changed)
		);
		if (changed)
				send_rdp_clipboard_payload(this, false);
		return;
	}

	if (client->cliprdr_requested_format_id == client->cliprdr_client_png_format_id ||
	    client->cliprdr_requested_format_id == client->cliprdr_client_tiff_format_id ||
	    client->cliprdr_requested_format_id == client->cliprdr_client_jpeg_format_id ||
	    client->cliprdr_requested_format_id == client->cliprdr_client_webp_format_id ||
	    client->cliprdr_requested_format_id == client->cliprdr_client_bmp_format_id ||
	    client->cliprdr_requested_format_id == RF_RDP_CLIPRDR_CF_TIFF) {
		uint32_t width = 0;
		uint32_t height = 0;
		size_t stride = 0;
		const char *name = "image";
		g_autoptr(GByteArray) rgba = rf_rdp_cliprdr_image_format_to_rgba(
			data,
			length,
			&width,
			&height,
			&stride
		);

		if (client->cliprdr_requested_format_id == client->cliprdr_client_png_format_id)
			name = "PNG";
		else if (client->cliprdr_requested_format_id == client->cliprdr_client_tiff_format_id)
			name = "TIFF";
		else if (client->cliprdr_requested_format_id == client->cliprdr_client_jpeg_format_id)
			name = "JPEG";
		else if (client->cliprdr_requested_format_id == client->cliprdr_client_webp_format_id)
			name = "WEBP";
		else if (client->cliprdr_requested_format_id == client->cliprdr_client_bmp_format_id)
			name = "BMP";
		else if (client->cliprdr_requested_format_id == RF_RDP_CLIPRDR_CF_TIFF)
			name = "CF_TIFF";

		if (rgba == NULL) {
			g_message("RDP: cliprdr %s response is not supported image data.",
				name);
			request_next_cliprdr_format(client, "unsupported image data");
			return;
		}
		g_mutex_lock(&this->lock);
		g_free(this->clipboard_text);
		this->clipboard_text = NULL;
		rf_clipboard_rich_payload_clear(&this->rich_clipboard);
		changed = rf_clipboard_rich_payload_set_image_rgba(
			&this->rich_clipboard,
			rgba->data,
			rgba->len,
			width,
			height,
			stride
		);
		g_mutex_unlock(&this->lock);
		g_message(
			"RDP: cliprdr received %s image response %ux%u stride=%zu source-length=%zu changed=%s.",
			name,
			width,
			height,
			stride,
			length,
			yes_no(changed)
		);
		if (changed)
				send_rdp_clipboard_payload(this, false);
		return;
	}

	if (client->cliprdr_requested_format_id == client->cliprdr_client_html_format_id) {
		g_autoptr(GByteArray) html =
			rf_rdp_cliprdr_html_format_unwrap(data, length);
		g_autofree char *text = NULL;
		uint32_t width = 0;
		uint32_t height = 0;
		size_t stride = 0;
		g_autoptr(GByteArray) rgba = rf_rdp_cliprdr_html_image_to_rgba(
			data,
			length,
			&width,
			&height,
			&stride
		);

			if (html == NULL) {
				g_message("RDP: cliprdr HTML response is malformed.");
				request_next_cliprdr_format(client, "malformed HTML data");
				return;
			}
		if (rgba != NULL) {
			g_mutex_lock(&this->lock);
			g_free(this->clipboard_text);
			this->clipboard_text = NULL;
			rf_clipboard_rich_payload_clear(&this->rich_clipboard);
			changed = rf_clipboard_rich_payload_set_image_rgba(
				&this->rich_clipboard,
				rgba->data,
				rgba->len,
				width,
				height,
				stride
			);
			g_mutex_unlock(&this->lock);
			g_message(
				"RDP: cliprdr received HTML image response %ux%u stride=%zu html-length=%u changed=%s.",
				width,
				height,
				stride,
				html->len,
				yes_no(changed)
			);
			if (changed)
					send_rdp_clipboard_payload(this, false);
			return;
		}
		text = rf_rdp_cliprdr_html_fragment_to_text(html->data, html->len);
		g_mutex_lock(&this->lock);
		g_free(this->clipboard_text);
		this->clipboard_text = g_strdup(text);
		rf_clipboard_rich_payload_clear(&this->rich_clipboard);
		if (text != NULL && text[0] != '\0')
			rf_clipboard_rich_payload_set_text(
				&this->rich_clipboard,
				text
			);
		changed = rf_clipboard_rich_payload_set_html(
			&this->rich_clipboard,
			html->data,
			html->len
		);
		g_mutex_unlock(&this->lock);
		g_message(
			"RDP: cliprdr received HTML response length %u text-length=%zu changed=%s.",
			html->len,
			text != NULL ? strlen(text) : 0,
			yes_no(changed)
		);
		if (changed)
				send_rdp_clipboard_payload(this, false);
		return;
	}

	g_message(
		"RDP: cliprdr ignored response for requested format id %u.",
		client->cliprdr_requested_format_id
	);
}

static void handle_cliprdr_format_data_request(
	struct client *client,
	const uint8_t *data,
	size_t length
)
{
	RfRDPServer *this = client->server;
	uint32_t format_id = 0;
	g_autofree char *text = NULL;
	g_autoptr(GByteArray) html = NULL;
	g_autoptr(GByteArray) image = NULL;
	uint32_t image_width = 0;
	uint32_t image_height = 0;
	size_t image_stride = 0;

	if (!rf_rdp_cliprdr_parse_format_data_request(
		    data,
		    length,
		    &format_id
	    )) {
		g_message("RDP: cliprdr unsupported data request length %zu.", length);
		if (!send_cliprdr_format_data_response_text(client, NULL))
			g_warning("RDP: Failed to send cliprdr data response failure.");
		return;
	}

	g_mutex_lock(&this->lock);
	if (this->clipboard_text != NULL)
		text = g_strdup(this->clipboard_text);
	if (this->rich_clipboard.html != NULL)
		html = g_byte_array_ref(this->rich_clipboard.html);
	if (this->rich_clipboard.image_rgba != NULL) {
		image = g_byte_array_ref(this->rich_clipboard.image_rgba);
		image_width = this->rich_clipboard.image_width;
		image_height = this->rich_clipboard.image_height;
		image_stride = this->rich_clipboard.image_stride;
	}
	g_mutex_unlock(&this->lock);

	switch (format_id) {
	case RF_RDP_CLIPRDR_CF_UNICODETEXT:
		g_message("RDP: cliprdr client requested Unicode text.");
		if (!send_cliprdr_format_data_response_text(client, text))
			g_warning("RDP: Failed to send cliprdr Unicode text response.");
		break;
	case RF_RDP_CLIPRDR_CF_TEXT:
		g_message("RDP: cliprdr client requested CF_TEXT.");
		if (!send_cliprdr_format_data_response_utf8_text(client, text))
			g_warning("RDP: Failed to send cliprdr CF_TEXT response.");
		break;
	case RF_RDP_CLIPRDR_CF_OEMTEXT:
		g_message("RDP: cliprdr client requested CF_OEMTEXT.");
		if (!send_cliprdr_format_data_response_utf8_text(client, text))
			g_warning("RDP: Failed to send cliprdr CF_OEMTEXT response.");
		break;
	case RF_RDP_CLIPRDR_CF_LOCALE:
		g_message("RDP: cliprdr client requested CF_LOCALE.");
		if (!send_cliprdr_format_data_response_locale(client, 0x00000409))
			g_warning("RDP: Failed to send cliprdr locale response.");
		break;
	case RF_RDP_CLIPRDR_CF_DIB:
	case RF_RDP_CLIPRDR_CF_DIBV5: {
		const bool v5 = format_id == RF_RDP_CLIPRDR_CF_DIBV5;
		g_autoptr(GByteArray) dib = image != NULL ?
			rf_rdp_cliprdr_rgba_to_dib(
				image->data,
				image->len,
				image_width,
				image_height,
				image_stride,
				v5
			) :
			NULL;

		g_message("RDP: cliprdr client requested %s.",
			v5 ? "CF_DIBV5" : "CF_DIB");
		if (!send_cliprdr_format_data_response_bytes(
			    client,
			    dib != NULL ? dib->data : NULL,
			    dib != NULL ? dib->len : 0,
			    v5 ? "CF_DIBV5" : "CF_DIB"
		    ))
			g_warning("RDP: Failed to send cliprdr image response.");
		break;
	}
	default:
		if (format_id == RF_RDP_CLIPRDR_FORMAT_PNG ||
		    format_id == RF_RDP_CLIPRDR_FORMAT_TIFF ||
		    format_id == RF_RDP_CLIPRDR_CF_TIFF ||
		    format_id == RF_RDP_CLIPRDR_FORMAT_JPEG ||
		    format_id == RF_RDP_CLIPRDR_FORMAT_WEBP ||
		    format_id == RF_RDP_CLIPRDR_FORMAT_BMP) {
			const char *format_name = NULL;
			const char *log_name = NULL;
			g_autoptr(GByteArray) encoded = NULL;

			if (format_id == RF_RDP_CLIPRDR_FORMAT_PNG) {
				format_name = RF_RDP_CLIPRDR_PNG_FORMAT_NAME;
				log_name = "image/png";
			} else if (format_id == RF_RDP_CLIPRDR_FORMAT_TIFF) {
				format_name = RF_RDP_CLIPRDR_TIFF_FORMAT_NAME;
				log_name = "image/tiff";
			} else if (format_id == RF_RDP_CLIPRDR_CF_TIFF) {
				format_name = RF_RDP_CLIPRDR_TIFF_FORMAT_NAME;
				log_name = "CF_TIFF";
			} else if (format_id == RF_RDP_CLIPRDR_FORMAT_JPEG) {
				format_name = RF_RDP_CLIPRDR_JPEG_FORMAT_NAME;
				log_name = "image/jpeg";
			} else if (format_id == RF_RDP_CLIPRDR_FORMAT_WEBP) {
				format_name = RF_RDP_CLIPRDR_WEBP_FORMAT_NAME;
				log_name = "image/webp";
			} else {
				format_name = RF_RDP_CLIPRDR_BMP_FORMAT_NAME;
				log_name = "image/bmp";
			}
			encoded = image != NULL ?
				rf_rdp_cliprdr_rgba_to_image_format(
					image->data,
					image->len,
					image_width,
					image_height,
					image_stride,
					format_name
				) :
				NULL;

			g_message("RDP: cliprdr client requested %s.", log_name);
			if (!send_cliprdr_format_data_response_bytes(
				    client,
				    encoded != NULL ? encoded->data : NULL,
				    encoded != NULL ? encoded->len : 0,
				    log_name
			    ))
				g_warning("RDP: Failed to send cliprdr %s response.",
					log_name);
		} else
		if (format_id == RF_RDP_CLIPRDR_FORMAT_HTML ||
		    format_id == client->cliprdr_client_html_format_id) {
			g_autoptr(GByteArray) wrapped = html != NULL ?
				rf_rdp_cliprdr_html_format_wrap(
					html->data,
					html->len
				) :
				NULL;

			g_message("RDP: cliprdr client requested HTML Format.");
			if (!send_cliprdr_format_data_response_bytes(
				    client,
				    wrapped != NULL ? wrapped->data : NULL,
				    wrapped != NULL ? wrapped->len : 0,
				    "HTML Format"
			    ))
				g_warning("RDP: Failed to send cliprdr HTML response.");
		} else {
			g_message(
				"RDP: cliprdr unsupported data request format id %u.",
				format_id
			);
			if (!send_cliprdr_format_data_response_text(client, NULL))
				g_warning("RDP: Failed to send cliprdr data response failure.");
		}
		break;
	}
}

static void handle_cliprdr_format_list(
	struct client *client,
	const uint8_t *data,
	size_t length
)
{
	struct rf_rdp_cliprdr_format_list formats = { 0 };
	const bool ok = rf_rdp_cliprdr_parse_format_list(
		data,
		length,
		&formats
	);
	const uint32_t request_format_id =
		ok ? rf_rdp_cliprdr_choose_request_format(&formats) : 0;

	client->cliprdr_client_has_unicode_text = ok && formats.unicode_text;
	client->cliprdr_client_html_format_id = ok ? formats.html_format_id : 0;
	client->cliprdr_client_png_format_id = ok ? formats.png_format_id : 0;
	client->cliprdr_client_tiff_format_id = ok ? formats.tiff_format_id : 0;
	client->cliprdr_client_jpeg_format_id = ok ? formats.jpeg_format_id : 0;
	client->cliprdr_client_webp_format_id = ok ? formats.webp_format_id : 0;
	client->cliprdr_client_bmp_format_id = ok ? formats.bmp_format_id : 0;
	if (ok)
		client->cliprdr_client_formats = formats;
	else
		memset(&client->cliprdr_client_formats, 0,
			sizeof(client->cliprdr_client_formats));
	g_message(
		"RDP: cliprdr client format list ok=%s unicode-text=%s html=%s(%u) dib=%s dibv5=%s cf-tiff=%s png=%s(%u) tiff=%s(%u) jpeg=%s(%u) webp=%s(%u) bmp=%s(%u) length=%zu.",
		yes_no(ok),
		yes_no(formats.unicode_text),
		yes_no(formats.html),
		formats.html_format_id,
		yes_no(formats.dib),
		yes_no(formats.dibv5),
		yes_no(formats.cf_tiff),
		yes_no(formats.png),
		formats.png_format_id,
		yes_no(formats.tiff),
		formats.tiff_format_id,
		yes_no(formats.jpeg),
		formats.jpeg_format_id,
		yes_no(formats.webp),
		formats.webp_format_id,
		yes_no(formats.bmp),
		formats.bmp_format_id,
		length
	);
	if (!send_cliprdr_format_list_response(client, ok))
		g_warning("RDP: Failed to send cliprdr format list response.");
	if (!ok)
		return;
	if (request_format_id != 0 &&
	    !send_cliprdr_format_data_request(client, request_format_id))
		g_warning("RDP: Failed to request cliprdr format %u.",
			request_format_id);
}

static void handle_cliprdr_payload(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	struct rf_rdp_cliprdr_pdu pdu = { 0 };
	const uint8_t *body = NULL;

	if (!rf_rdp_cliprdr_parse_pdu(payload, payload_length, &pdu)) {
		g_message("RDP: Ignoring malformed cliprdr PDU length %zu.",
			payload_length);
		return;
	}

	g_message(
		"RDP: cliprdr received msg=0x%04x flags=0x%04x data=%u.",
		pdu.msg_type,
		pdu.msg_flags,
		pdu.data_length
	);
	body = payload + pdu.data_offset;
	switch (pdu.msg_type) {
	case RF_RDP_CLIPRDR_CB_CLIP_CAPS:
		client->cliprdr_client_caps_received = true;
		g_message("RDP: cliprdr client capabilities received.");
		maybe_send_cliprdr_cached_format_list(client);
		break;
	case RF_RDP_CLIPRDR_CB_MONITOR_READY:
		client->cliprdr_ready = true;
		g_message("RDP: cliprdr monitor ready.");
		maybe_send_cliprdr_cached_format_list(client);
		break;
	case RF_RDP_CLIPRDR_CB_FORMAT_LIST:
		handle_cliprdr_format_list(client, body, pdu.data_length);
		break;
	case RF_RDP_CLIPRDR_CB_FORMAT_LIST_RESPONSE:
		if ((pdu.msg_flags & RF_RDP_CLIPRDR_CB_RESPONSE_OK) == 0)
			g_message("RDP: cliprdr client rejected server format list.");
		break;
	case RF_RDP_CLIPRDR_CB_FORMAT_DATA_REQUEST:
		handle_cliprdr_format_data_request(client, body, pdu.data_length);
		break;
	case RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE:
		handle_cliprdr_format_data_response(
			client,
			body,
			pdu.data_length,
			pdu.msg_flags
		);
		break;
	default:
		g_message("RDP: Ignoring unsupported cliprdr message type 0x%04x.",
			pdu.msg_type);
		break;
	}
}

static void handle_cliprdr_channel_pdu(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	struct rf_rdp_dvc_channel_pdu channel = { 0 };

	if (!rf_rdp_dvc_parse_channel_pdu(payload, payload_length, &channel)) {
		g_message("RDP: Ignoring malformed cliprdr static channel PDU length %zu.",
			payload_length);
		return;
	}
	g_message(
		"RDP: cliprdr static channel PDU flags=0x%08x total=%u payload=%zu.",
		channel.flags,
		channel.total_length,
		channel.payload_length
	);
	g_autoptr(GByteArray) reassembled = NULL;
	if (!rf_rdp_cliprdr_channel_reassembler_add(
		    &client->cliprdr_channel_reassembler,
		    payload,
		    payload_length,
		    &reassembled
	    )) {
		g_message("RDP: Ignoring malformed fragmented cliprdr static channel PDU.");
		return;
	}
	if (reassembled == NULL) {
		g_message("RDP: Buffered fragmented cliprdr static channel PDU.");
		return;
	}

	handle_cliprdr_payload(
		client,
		reassembled->data,
		reassembled->len
	);
}

static bool rdpsnd_extract_static_payload(
	const uint8_t *payload,
	size_t payload_length,
	const uint8_t **body,
	size_t *body_length
)
{
	struct rf_rdp_dvc_channel_pdu channel = { 0 };

	if (body == NULL || body_length == NULL)
		return false;
	*body = NULL;
	*body_length = 0;
	if (payload == NULL || payload_length < 4)
		return false;

	if (rf_rdp_dvc_parse_channel_pdu(payload, payload_length, &channel) &&
	    (channel.flags & RF_RDP_DVC_CHANNEL_FLAG_FIRST) != 0 &&
	    (channel.flags & RF_RDP_DVC_CHANNEL_FLAG_LAST) != 0) {
		*body = payload + channel.payload_offset;
		*body_length = channel.payload_length;
		return *body_length >= 4;
	}

	*body = payload;
	*body_length = payload_length;
	return true;
}

static void set_rdpsnd_ready(struct client *client, bool ready, const char *reason);

static void handle_rdpsnd_client_formats(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	g_autofree char *formats_summary = NULL;

	if (!rf_rdp_rdpsnd_parse_client_formats(
		    payload,
		    payload_length,
		    &client->rdpsnd_client_formats
	    )) {
		g_message("RDP: Ignoring malformed rdpsnd client formats.");
		return;
	}

	client->rdpsnd_client_formats_received = true;
	client->rdpsnd_selected_format = rf_rdp_rdpsnd_choose_audio_format(
		&client->rdpsnd_client_formats,
		client->server->audio_sample_rate,
		client->server->audio_channels,
		rdp_audio_prefers_adpcm(client->server)
	);
	set_rdpsnd_ready(
		client,
		client->rdpsnd_selected_format >= 0 &&
			client->rdpsnd_client_formats.version <
				RF_RDP_RDPSND_CHANNEL_VERSION_WIN_7,
		"formats"
	);

	formats_summary = rdpsnd_client_formats_summary(&client->rdpsnd_client_formats);
	g_message(
		"RDP: rdpsnd client formats=%zu version=%u selected=%d ready=%s codec=%s formats=[%s].",
		client->rdpsnd_client_formats.format_count,
		client->rdpsnd_client_formats.version,
		client->rdpsnd_selected_format,
		yes_no(client->rdpsnd_ready),
		client_rdpsnd_selected_format(client) != NULL ?
			rf_rdp_rdpsnd_format_name(
				client_rdpsnd_selected_format(client)->tag
			) :
			"unknown",
		formats_summary
	);
}

static void set_rdpsnd_ready(struct client *client, bool ready, const char *reason)
{
	const bool was_ready = client->rdpsnd_ready;

	client->rdpsnd_ready = ready;
	if (ready && !was_ready) {
		client->rdpsnd_first_audio_sent = false;
		client->rdpsnd_first_audio_confirmed = false;
		client->rdpsnd_first_audio_sent_time_us = 0;
		client->server->audio_silence_suppressed = false;
		client->server->audio_silent_frames = 0;
		clear_queued_audio_frames(client->server);
		g_message(
			"RDP: rdpsnd ready via %s; cleared queued audio for fresh startup.",
			reason
		);
	}
}

static void handle_rdpsnd_channel_pdu(
	struct client *client,
	const uint8_t *payload,
	size_t payload_length
)
{
	const uint8_t *body = NULL;
	size_t body_length = 0;

	if (!rdpsnd_extract_static_payload(
		    payload,
		    payload_length,
		    &body,
		    &body_length
	    )) {
		g_message("RDP: Ignoring malformed rdpsnd static channel PDU.");
		return;
	}

	switch (body[0]) {
	case RF_RDP_RDPSND_SNDC_FORMATS:
		handle_rdpsnd_client_formats(client, body, body_length);
		break;
	case RF_RDP_RDPSND_SNDC_QUALITYMODE:
		client->rdpsnd_quality_mode_received = true;
		set_rdpsnd_ready(
			client,
			client->rdpsnd_selected_format >= 0,
			"quality-mode"
		);
		g_message("RDP: rdpsnd client quality mode received; ready=%s.",
			yes_no(client->rdpsnd_ready));
		break;
	case RF_RDP_RDPSND_SNDC_TRAINING: {
		uint16_t timestamp = 0;
		uint16_t pack_size = 0;

		client->rdpsnd_training_confirmed =
			rf_rdp_rdpsnd_parse_training_confirm(
				body,
				body_length,
				&timestamp,
				&pack_size
			);
		g_message(
			"RDP: rdpsnd training confirm ok=%s timestamp=%u pack-size=%u.",
			yes_no(client->rdpsnd_training_confirmed),
			timestamp,
			pack_size
		);
		break;
	}
	case RF_RDP_RDPSND_SNDC_WAVECONFIRM: {
		uint16_t timestamp = 0;
		uint8_t block_no = 0;

		if (rf_rdp_rdpsnd_parse_wave_confirm(
			    body,
			    body_length,
			    &timestamp,
			    &block_no
		    )) {
			RfRDPServer *this = client->server;
			const int64_t now = g_get_monotonic_time();

			g_mutex_lock(&this->audio_lock);
			this->audio_stats_wave_confirms++;
			if (!client->rdpsnd_first_audio_confirmed &&
			    client->rdpsnd_first_audio_sent &&
			    block_no == client->rdpsnd_first_audio_block_no) {
				client->rdpsnd_first_audio_confirmed = true;
				if (client->rdpsnd_first_audio_sent_time_us > 0 &&
				    now > client->rdpsnd_first_audio_sent_time_us)
					this->audio_stats_first_confirm_us =
						now - client->rdpsnd_first_audio_sent_time_us;
				g_message(
					"RDP: rdpsnd first wave confirm block=%u timestamp=%u after=%" G_GUINT64_FORMAT "ms.",
					block_no,
					timestamp,
					this->audio_stats_first_confirm_us / 1000u
				);
			}
			g_mutex_unlock(&this->audio_lock);
			g_debug("RDP: rdpsnd wave confirm block=%u timestamp=%u.",
				block_no,
				timestamp);
		}
		break;
	}
	default:
		g_debug("RDP: Ignoring rdpsnd message type 0x%02x.", body[0]);
		break;
	}
}

static void handle_active_session(struct client *client)
{
	RfRDPServer *this = client->server;
	GInputStream *input = g_io_stream_get_input_stream(client->stream);

	g_message("RDP: Client reached active state.");
	start_drdynvc(client);
	start_rdpsnd(client);
	start_cliprdr(client);
	add_client(client);

	while (this->running) {
		g_autoptr(GByteArray) pdu = read_tpkt(input);
		struct rf_rdp_mcs_domain_pdu domain_pdu = { 0 };
		struct rf_rdp_core_pdu core_pdu = { 0 };

		if (pdu == NULL)
			break;
		if (rf_rdp_mcs_parse_domain_pdu(
			    pdu->data,
			    pdu->len,
			    &domain_pdu
		    ) && domain_pdu.type == RF_RDP_MCS_PDU_SEND_DATA_REQUEST &&
		    client->drdynvc_channel_id != 0 &&
		    domain_pdu.channel_id == client->drdynvc_channel_id) {
			handle_drdynvc_channel_pdu(
				client,
				pdu->data + domain_pdu.payload_offset,
				domain_pdu.payload_length
			);
			continue;
		}
		if (domain_pdu.type == RF_RDP_MCS_PDU_SEND_DATA_REQUEST &&
		    client->rdpsnd_channel_id != 0 &&
		    domain_pdu.channel_id == client->rdpsnd_channel_id) {
			handle_rdpsnd_channel_pdu(
				client,
				pdu->data + domain_pdu.payload_offset,
				domain_pdu.payload_length
			);
			continue;
		}
		if (domain_pdu.type == RF_RDP_MCS_PDU_SEND_DATA_REQUEST &&
		    client->cliprdr_channel_id != 0 &&
		    domain_pdu.channel_id == client->cliprdr_channel_id) {
			handle_cliprdr_channel_pdu(
				client,
				pdu->data + domain_pdu.payload_offset,
				domain_pdu.payload_length
			);
			continue;
		}
		if (!rf_rdp_core_parse_client_pdu(
			    pdu->data,
			    pdu->len,
			    &core_pdu
		    )) {
			g_debug("RDP: Ignoring unsupported active client PDU.");
			continue;
		}
		if (core_pdu.share_type == RF_RDP_PDU_TYPE_DATA)
			switch (core_pdu.data_type) {
			case RF_RDP_DATA_PDU_TYPE_INPUT:
				handle_input_pdu(client, pdu, &core_pdu);
				break;
			default:
				g_debug(
					"RDP: Ignoring active Data PDU type 0x%02x.",
					core_pdu.data_type
				);
				break;
		}
	}
}

static bool send_rdpgfx_surface_setup(
	struct client *client,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	uint8_t gfx[512] = { 0 };
	size_t length = 0;

	length = rf_rdp_gfx_write_reset_graphics(gfx, sizeof(gfx), width, height);
	if (length == 0 ||
	    !send_rdpgfx_gfx_payload(client, gfx, length, bytes_sent, false))
		return false;

	memset(gfx, 0, sizeof(gfx));
	length = rf_rdp_gfx_write_create_surface(
		gfx,
		sizeof(gfx),
		RDP_RDPGFX_SURFACE_ID,
		width,
		height,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888
	);
	if (length == 0 ||
	    !send_rdpgfx_gfx_payload(client, gfx, length, bytes_sent, false))
		return false;

	memset(gfx, 0, sizeof(gfx));
	length = rf_rdp_gfx_write_map_surface_to_output(
		gfx,
		sizeof(gfx),
		RDP_RDPGFX_SURFACE_ID,
		0,
		0
	);
	if (length == 0 ||
	    !send_rdpgfx_gfx_payload(client, gfx, length, bytes_sent, false))
		return false;

	client->rdpgfx_surface_ready = true;
	client->rdpgfx_surface_width = width;
	client->rdpgfx_surface_height = height;
	g_message("RDP: RDPGFX surface ready %ux%u.", width, height);
	return true;
}

static bool send_rdpgfx_av1_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int policy_quality_level =
		client->server->rdpgfx_video_quality_level;
	unsigned int quality_level = 0;
	unsigned int coded_width = 0;
	unsigned int coded_height = 0;
	int64_t bit_rate = 0;
	unsigned int gop_size = 0;
	uint8_t qp = 0;
	uint8_t quality = 0;
	g_autofree uint8_t *av1_data = NULL;
	g_autofree uint8_t *bitmap_stream = NULL;
	g_autofree uint8_t *gfx = NULL;
	const uint8_t *rgba = NULL;
	size_t rgba_available_length = 0;
	size_t rgba_stride = 0;
	size_t av1_data_length = 0;
	size_t bitmap_stream_length = 0;
	size_t wire_length = 0;
	size_t raw_capacity = 0;
	size_t offset = 0;
	size_t length = 0;
	uint32_t frame_id = 0;
	bool force_keyframe = false;

	if (!client_can_use_rdpgfx_av1(client) || frame_width == 0 ||
	    frame_height == 0 || width == 0 || height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX)
		return false;
	if (buf == NULL || buf->data == NULL ||
	    buf->len < (size_t)frame_width * frame_height * 4)
		return false;
	if (!rf_rdp_core_make_full_surface_rect(
		    frame_width, frame_height, &x, &y, &width, &height
	    ))
		return false;
	coded_width = align16(width);
	coded_height = align16(height);
	if (coded_width > UINT16_MAX || coded_height > UINT16_MAX)
		return false;
	rgba_stride = (size_t)frame_width * 4;
	rgba_available_length = buf->len;
	rgba = buf->data;
	quality_level = client_video_encoder_quality_level(
		client,
		client->av1 != NULL &&
			rf_rdp_av1_encoder_mode(client->av1) == client->av1_mode &&
			client->av1_width == coded_width &&
			client->av1_height == coded_height,
		client->av1_quality_level,
		policy_quality_level
	);

	bit_rate = rf_rdp_core_rdpgfx_avc_bit_rate(
		coded_width,
		coded_height,
		MAX(client->server->adaptive_fps, 1),
		quality_level,
		false
	);
	qp = rf_rdp_core_rdpgfx_avc_qp(quality_level, false);
	quality = rf_rdp_core_rdpgfx_avc_quality(quality_level, false);
	gop_size = rf_rdp_core_rdpgfx_avc_gop_size(
		MAX(client->server->adaptive_fps, 1),
		quality_level,
		false
	);

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != frame_height) {
		if (!send_rdpgfx_surface_setup(
			    client,
			    (uint16_t)frame_width,
			    (uint16_t)frame_height,
			    bytes_sent
		    ))
			return false;
		client_reset_avc(client);
		force_keyframe = true;
	}

	if (client->av1 == NULL ||
	    rf_rdp_av1_encoder_mode(client->av1) != client->av1_mode ||
	    client->av1_width != coded_width ||
	    client->av1_height != coded_height ||
	    client->av1_bit_rate != bit_rate ||
	    client->av1_gop_size != gop_size || client->av1_qp != qp ||
	    client->av1_quality_level != quality_level) {
		client_reset_avc(client);
		client->rdpgfx_update_logged = false;
		client->av1 = rf_rdp_av1_encoder_new_with_rate_and_mode(
			(uint16_t)coded_width,
			(uint16_t)coded_height,
			MAX(client->server->adaptive_fps, 1),
			bit_rate,
			qp,
			gop_size,
			client->av1_mode,
			client->server->avc_encoder
		);
		client->av1_width = client->av1 != NULL ? coded_width : 0;
		client->av1_height = client->av1 != NULL ? coded_height : 0;
		client->av1_bit_rate = client->av1 != NULL ? bit_rate : 0;
		client->av1_gop_size = client->av1 != NULL ? gop_size : 0;
		client->av1_qp = client->av1 != NULL ? qp : 0;
		client->av1_quality_level =
			client->av1 != NULL ? quality_level : 0;
		force_keyframe = true;
	}
	if (client->av1 == NULL) {
		if (!client->rdpgfx_av1_unavailable_logged) {
			g_message("RDP: AV1 encoder unavailable; disabling RDPGFX AV1.");
			client->rdpgfx_av1_unavailable_logged = true;
		}
		return false;
	}

	if (!rf_rdp_av1_encoder_encode_rgba(
		    client->av1,
		    rgba,
		    rgba_available_length,
		    rgba_stride,
		    force_keyframe || client->needs_full_frame,
		    &av1_data,
		    &av1_data_length
	    )) {
		client_reset_avc(client);
		return false;
	}
	if (!client->rdpgfx_update_logged) {
		const char *dump_path = g_getenv("RF_RDP_AV1_DUMP");
		if (dump_path != NULL && dump_path[0] != '\0') {
			GError *error = NULL;
			if (g_file_set_contents(
				    dump_path,
				    (const char *)av1_data,
				    av1_data_length,
				    &error
			    )) {
				g_message(
					"RDP: Dumped first live AV1 frame to %s (%zu bytes).",
					dump_path,
					av1_data_length
				);
			} else {
				g_warning(
					"RDP: Failed to dump first live AV1 frame: %s",
					error != NULL ? error->message : "unknown error"
				);
				g_clear_error(&error);
			}
		}
	}

	if (av1_data_length > UINT32_MAX - 14)
		return false;
	bitmap_stream = g_malloc(14 + av1_data_length);
	bitmap_stream_length = rf_rdp_gfx_write_avc420_bitmap_stream(
		bitmap_stream,
		14 + av1_data_length,
		x,
		y,
		(uint16_t)(x + width),
		(uint16_t)(y + height),
		qp,
		quality,
		av1_data,
		av1_data_length
	);
	if (bitmap_stream_length == 0)
		return false;

	wire_length = RF_RDP_GFX_HEADER_SIZE + 17 + bitmap_stream_length;
	if (wire_length > SIZE_MAX - (RF_RDP_GFX_HEADER_SIZE + 8) -
	    (RF_RDP_GFX_HEADER_SIZE + 4))
		return false;
	raw_capacity =
		(RF_RDP_GFX_HEADER_SIZE + 8) + wire_length +
		(RF_RDP_GFX_HEADER_SIZE + 4);
	gfx = g_malloc(raw_capacity);

	frame_id = ++client->rdpgfx_frame_id;
	if (frame_id == 0)
		frame_id = ++client->rdpgfx_frame_id;

	length = rf_rdp_gfx_write_start_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id,
		(uint32_t)(g_get_monotonic_time() / 1000)
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_wire_to_surface_1(
		gfx + offset,
		raw_capacity - offset,
		RDP_RDPGFX_SURFACE_ID,
		RF_RDP_GFX_CODECID_AV1,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		x,
		y,
		(uint16_t)(x + width),
		(uint16_t)(y + height),
		bitmap_stream,
		bitmap_stream_length
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_end_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id
	);
	if (length == 0)
		return false;
	offset += length;

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent, false))
		return false;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX AV1 updates active using %s, mode=%s, rect %u,%u %ux%u, coded %ux%u, bitrate=%" G_GINT64_FORMAT ", qp=%u, gop=%u.",
			rf_rdp_av1_encoder_name(client->av1),
			av1_mode_name(rf_rdp_av1_encoder_mode(client->av1)),
			x,
			y,
			width,
			height,
			coded_width,
			coded_height,
			bit_rate,
			qp,
			gop_size
		);
		client->rdpgfx_update_logged = true;
	}
	return true;
}

static bool send_rdpgfx_avc444_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int policy_quality_level =
		client->server->rdpgfx_video_quality_level;
	unsigned int encoder_quality_level = 0;
	unsigned int coded_width = 0;
	unsigned int coded_height = 0;
	int64_t bit_rate = 0;
	unsigned int gop_size = 0;
	uint8_t qp = 0;
	uint8_t quality = 0;
	g_autofree uint8_t *luma_h264 = NULL;
	g_autofree uint8_t *chroma_h264 = NULL;
	g_autofree uint8_t *avc = NULL;
	g_autofree uint8_t *gfx = NULL;
	const uint8_t *rgba = NULL;
	size_t rgba_available_length = 0;
	size_t rgba_stride = 0;
	uint8_t lc = RF_RDP_GFX_AVC444_LC_BOTH;
	unsigned int lc_index = 0;
	const bool avc444_v2 =
		client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_AVC444_V2;
	const uint16_t codec_id =
		rf_rdp_gfx_codec_wire_id(client->rdpgfx_policy_codec);
	size_t luma_h264_length = 0;
	size_t chroma_h264_length = 0;
	size_t avc_length = 0;
	size_t avc_capacity = 0;
	size_t wire_length = 0;
	size_t raw_capacity = 0;
	size_t offset = 0;
	size_t length = 0;
	uint32_t frame_id = 0;
	bool force_keyframe = false;
	bool encode_luma = false;
	bool encode_chroma = false;
	bool full_avc444 = false;
	bool skip_avc444_delta = false;
	uint16_t damage_x = x;
	uint16_t damage_y = y;
	uint16_t damage_width = width;
	uint16_t damage_height = height;
	uint32_t damage_pixels = 0;
	uint32_t chroma_changed_pixels = 0;

	if (!rf_rdp_core_should_use_avc444(
		    client_can_use_rdpgfx_avc444(client),
		    client_can_use_rdpgfx_avc420(client),
		    policy_quality_level
	    ) ||
	    frame_width == 0 || frame_height == 0 || width == 0 || height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX)
		return false;
	if (buf == NULL || buf->data == NULL ||
	    buf->len < (size_t)frame_width * frame_height * 4)
		return false;
	if (!rf_rdp_core_make_full_surface_rect(
		    frame_width, frame_height, &x, &y, &width, &height
	    ))
		return false;
	coded_width = align16(width);
	coded_height = align16(height);
	if (coded_width > UINT16_MAX || coded_height > UINT16_MAX)
		return false;
	rgba_stride = (size_t)frame_width * 4;
	rgba_available_length = buf->len;
	rgba = buf->data;
	encoder_quality_level = client_video_encoder_quality_level(
		client,
		client->avc != NULL &&
			client->avc_chroma == NULL &&
			client->avc_width == coded_width &&
			client->avc_height == coded_height,
		client->avc_quality_level,
		policy_quality_level
	);

	bit_rate = rf_rdp_core_rdpgfx_avc_bit_rate(
		coded_width,
		coded_height,
		MAX(client->server->adaptive_fps, 1),
		encoder_quality_level,
		true
	);
	qp = rf_rdp_core_rdpgfx_avc_qp(encoder_quality_level, true);
	quality = rf_rdp_core_rdpgfx_avc_quality(encoder_quality_level, true);
	gop_size = rf_rdp_core_rdpgfx_avc_gop_size(
		MAX(client->server->adaptive_fps, 1),
		encoder_quality_level,
		true
	);

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != frame_height) {
		if (!send_rdpgfx_surface_setup(
			    client,
			    (uint16_t)frame_width,
			    (uint16_t)frame_height,
			    bytes_sent
		    ))
			return false;
		client_reset_avc(client);
		force_keyframe = true;
	}

	if (client->avc == NULL || client->avc_chroma != NULL ||
	    client->avc_width != coded_width ||
	    client->avc_height != coded_height ||
	    client->avc_bit_rate != bit_rate ||
	    client->avc_gop_size != gop_size || client->avc_qp != qp ||
	    client->avc_quality_level != encoder_quality_level) {
		client_reset_avc(client);
		client->rdpgfx_update_logged = false;
		client->avc = rf_rdp_avc_hardware_encoder_new_with_rate(
			(uint16_t)coded_width,
			(uint16_t)coded_height,
			MAX(client->server->adaptive_fps, 1),
			bit_rate,
			qp,
			gop_size,
			client->server->avc_encoder
		);
		if (client->avc == NULL) {
			if (!client->rdpgfx_avc444_software_fallback_logged) {
				g_message(
					"RDP: AVC444 hardware encoder unavailable; falling back to AVC420."
				);
				client->rdpgfx_avc444_software_fallback_logged = true;
			}
			client_reset_avc(client);
			return false;
		}
		client->avc_width = coded_width;
		client->avc_height = coded_height;
		client->avc_bit_rate = bit_rate;
		client->avc_gop_size = gop_size;
		client->avc_qp = qp;
		client->avc_quality_level = encoder_quality_level;
		force_keyframe = true;
	}
	if (!rf_rdp_avc_encoder_is_hardware(client->avc)) {
		if (!client->rdpgfx_avc444_software_fallback_logged) {
			g_message(
				"RDP: AVC444 encoder selected %s is not hardware; falling back to AVC420.",
				rf_rdp_avc_encoder_name(client->avc)
			);
			client->rdpgfx_avc444_software_fallback_logged = true;
		}
		return false;
	}

	full_avc444 = force_keyframe || client->needs_full_frame ||
		client->server->last_frame == NULL ||
		client->server->last_frame_width != frame_width ||
		client->server->last_frame_height != frame_height;
	skip_avc444_delta = rf_rdp_core_should_skip_avc444_delta_for_quality(
		frame_width,
		frame_height,
		damage_width,
		damage_height,
		policy_quality_level
	);
	damage_pixels = (uint32_t)damage_width * damage_height;
	if (full_avc444 || skip_avc444_delta) {
		encode_luma = true;
		encode_chroma = true;
	} else {
		uint32_t luma_changed_pixels = 0;

		if (!rf_rdp_avc_analyze_avc444_rect(
			    rgba,
			    rgba_available_length,
			    client->server->last_frame->data,
			    client->server->last_frame->len,
			    rgba_stride,
			    damage_x,
			    damage_y,
			    damage_width,
			    damage_height,
			    &encode_luma,
			    &encode_chroma,
			    &luma_changed_pixels,
			    &chroma_changed_pixels
		    )) {
			encode_luma = true;
			encode_chroma = true;
			full_avc444 = true;
		}
	}
	if (!encode_luma && !encode_chroma)
		return true;
	if (rf_rdp_core_should_defer_avc444_chroma_for_damage(
		    client->rdpgfx_frame_id + 1u,
		    policy_quality_level,
		    full_avc444,
		    encode_luma,
		    encode_chroma,
		    damage_pixels,
		    chroma_changed_pixels
	    ))
		encode_chroma = false;

	if (encode_luma && encode_chroma) {
		uint8_t encoded_lc = 0xff;

		const bool encoded = avc444_v2 ?
			rf_rdp_avc_encoder_encode_avc444_v2_rgba(
				client->avc,
				rgba,
				rgba_available_length,
				rgba_stride,
				full_avc444,
				&encoded_lc,
				&luma_h264,
				&luma_h264_length,
				&chroma_h264,
				&chroma_h264_length
			) :
			rf_rdp_avc_encoder_encode_avc444_rgba(
			    client->avc,
			    rgba,
			    rgba_available_length,
			    rgba_stride,
			    full_avc444,
			    &encoded_lc,
			    &luma_h264,
			    &luma_h264_length,
			    &chroma_h264,
			    &chroma_h264_length
		    );

		if (!encoded) {
			client_reset_avc(client);
			return false;
		}
	} else if (encode_luma) {
		if (!rf_rdp_avc_encoder_encode_avc444_luma_rgba(
			    client->avc,
			    rgba,
			    rgba_available_length,
			    rgba_stride,
			    full_avc444,
			    &luma_h264,
			    &luma_h264_length
		    ))
			return false;
	} else {
		const bool encoded = avc444_v2 ?
			rf_rdp_avc_encoder_encode_avc444_v2_chroma_rgba(
				client->avc,
				rgba,
				rgba_available_length,
				rgba_stride,
				false,
				&chroma_h264,
				&chroma_h264_length
			) :
			rf_rdp_avc_encoder_encode_avc444_chroma_rgba(
				client->avc,
				rgba,
				rgba_available_length,
				rgba_stride,
				false,
				&chroma_h264,
				&chroma_h264_length
			);
		if (!encoded)
			return false;
	}

	if (encode_luma && encode_chroma) {
		if (luma_h264_length > UINT32_MAX - 14 ||
		    chroma_h264_length > UINT32_MAX - 14 ||
		    luma_h264_length > SIZE_MAX - chroma_h264_length ||
		    luma_h264_length + chroma_h264_length > SIZE_MAX - 32)
			return false;
		avc_capacity = 32 + luma_h264_length + chroma_h264_length;
		avc = g_malloc(avc_capacity);
		avc_length = rf_rdp_gfx_write_avc444_bitmap_stream_pair(
			avc,
			avc_capacity,
			x,
			y,
			(uint16_t)(x + width),
			(uint16_t)(y + height),
			qp,
			quality,
			luma_h264,
			luma_h264_length,
			chroma_h264,
			chroma_h264_length
		);
		lc = RF_RDP_GFX_AVC444_LC_BOTH;
	} else {
		const uint8_t *h264 =
			encode_luma ? luma_h264 : chroma_h264;
		const size_t h264_length =
			encode_luma ? luma_h264_length : chroma_h264_length;

		if (h264_length > UINT32_MAX - 14 ||
		    h264_length > SIZE_MAX - 18)
			return false;
		avc_capacity = 18 + h264_length;
		avc = g_malloc(avc_capacity);
		lc = encode_luma ?
			RF_RDP_GFX_AVC444_LC_SINGLE :
			RF_RDP_GFX_AVC444_LC_CHROMA;
		avc_length = rf_rdp_gfx_write_avc444_bitmap_stream(
			avc,
			avc_capacity,
			lc,
			x,
			y,
			(uint16_t)(x + width),
			(uint16_t)(y + height),
			qp,
			quality,
			h264,
			h264_length
		);
	}
	if (avc_length == 0)
		return false;

	wire_length = RF_RDP_GFX_HEADER_SIZE + 17 + avc_length;
	if (wire_length > SIZE_MAX - (RF_RDP_GFX_HEADER_SIZE + 8) -
	    (RF_RDP_GFX_HEADER_SIZE + 4))
		return false;
	raw_capacity =
		(RF_RDP_GFX_HEADER_SIZE + 8) + wire_length +
		(RF_RDP_GFX_HEADER_SIZE + 4);
	gfx = g_malloc(raw_capacity);

	frame_id = ++client->rdpgfx_frame_id;
	if (frame_id == 0)
		frame_id = ++client->rdpgfx_frame_id;

	length = rf_rdp_gfx_write_start_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id,
		(uint32_t)(g_get_monotonic_time() / 1000)
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_wire_to_surface_1(
		gfx + offset,
		raw_capacity - offset,
		RDP_RDPGFX_SURFACE_ID,
		codec_id,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		x,
		y,
		(uint16_t)(x + width),
		(uint16_t)(y + height),
		avc,
		avc_length
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_end_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id
	);
	if (length == 0)
		return false;
	offset += length;

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent, false))
		return false;
	if (rf_rdp_core_rdpgfx_avc444_lc_index(lc, &lc_index))
		client->server->stats_avc444_lc[lc_index]++;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX %s updates active using %s, rect %u,%u %ux%u, coded %ux%u, lc=%u, bitrate=%" G_GINT64_FORMAT ", qp=%u, gop=%u.",
			rf_rdp_gfx_codec_name(client->rdpgfx_policy_codec),
			rf_rdp_avc_encoder_name(client->avc),
			x,
			y,
			width,
			height,
			coded_width,
			coded_height,
			lc,
			bit_rate,
			qp,
			gop_size
		);
		client->rdpgfx_update_logged = true;
	}
	return true;
}

static bool send_rdpgfx_avc420_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int policy_quality_level =
		client->server->rdpgfx_video_quality_level;
	unsigned int encoder_quality_level = 0;
	unsigned int coded_width = 0;
	unsigned int coded_height = 0;
	int64_t bit_rate = 0;
	unsigned int gop_size = 0;
	uint8_t qp = 0;
	uint8_t quality = 0;
	g_autofree uint8_t *h264 = NULL;
	g_autofree uint8_t *avc = NULL;
	g_autofree uint8_t *gfx = NULL;
	const uint8_t *rgba = NULL;
	size_t rgba_available_length = 0;
	size_t rgba_stride = 0;
	size_t h264_length = 0;
	size_t avc_length = 0;
	size_t wire_length = 0;
	size_t raw_capacity = 0;
	size_t offset = 0;
	size_t length = 0;
	uint32_t frame_id = 0;
	bool force_keyframe = false;

	if (!client_can_use_rdpgfx_avc420(client) || frame_width == 0 ||
	    frame_height == 0 || width == 0 || height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX)
		return false;
	if (buf == NULL || buf->data == NULL ||
	    buf->len < (size_t)frame_width * frame_height * 4)
		return false;
	if (!rf_rdp_core_make_full_surface_rect(
		    frame_width, frame_height, &x, &y, &width, &height
	    ))
		return false;
	coded_width = align16(width);
	coded_height = align16(height);
	if (coded_width > UINT16_MAX || coded_height > UINT16_MAX)
		return false;
	rgba_stride = (size_t)frame_width * 4;
	rgba_available_length = buf->len;
	rgba = buf->data;
	encoder_quality_level = client_video_encoder_quality_level(
		client,
		client->avc != NULL &&
			client->avc_chroma == NULL &&
			client->avc_width == coded_width &&
			client->avc_height == coded_height,
		client->avc_quality_level,
		policy_quality_level
	);

	bit_rate = rf_rdp_core_rdpgfx_avc_bit_rate(
		coded_width,
		coded_height,
		MAX(client->server->adaptive_fps, 1),
		encoder_quality_level,
		false
	);
	qp = rf_rdp_core_rdpgfx_avc_qp(encoder_quality_level, false);
	quality = rf_rdp_core_rdpgfx_avc_quality(encoder_quality_level, false);
	gop_size = rf_rdp_core_rdpgfx_avc_gop_size(
		MAX(client->server->adaptive_fps, 1),
		encoder_quality_level,
		false
	);

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != frame_height) {
		if (!send_rdpgfx_surface_setup(
			    client,
			    (uint16_t)frame_width,
			    (uint16_t)frame_height,
			    bytes_sent
		    ))
			return false;
		client_reset_avc(client);
		force_keyframe = true;
	}

	if (client->avc == NULL || client->avc_chroma != NULL ||
	    client->avc_width != coded_width ||
	    client->avc_height != coded_height ||
	    client->avc_bit_rate != bit_rate ||
	    client->avc_gop_size != gop_size || client->avc_qp != qp ||
	    client->avc_quality_level != encoder_quality_level) {
		client_reset_avc(client);
		client->rdpgfx_update_logged = false;
		client->avc = rf_rdp_avc_encoder_new_with_rate(
			(uint16_t)coded_width,
			(uint16_t)coded_height,
			MAX(client->server->adaptive_fps, 1),
			bit_rate,
			qp,
			gop_size,
			client->server->avc_encoder
		);
		client->avc_width = client->avc != NULL ? coded_width : 0;
		client->avc_height = client->avc != NULL ? coded_height : 0;
		client->avc_bit_rate = client->avc != NULL ? bit_rate : 0;
		client->avc_gop_size = client->avc != NULL ? gop_size : 0;
		client->avc_qp = client->avc != NULL ? qp : 0;
		client->avc_quality_level =
			client->avc != NULL ? encoder_quality_level : 0;
		force_keyframe = true;
	}
	if (client->avc == NULL) {
		if (!client->rdpgfx_avc_unavailable_logged) {
			g_message(
				"RDP: AVC420 encoder unavailable; using PLANAR RDPGFX."
			);
			client->rdpgfx_avc_unavailable_logged = true;
		}
		return false;
	}

	if (!rf_rdp_avc_encoder_encode_rgba(
		    client->avc,
		    rgba,
		    rgba_available_length,
		    rgba_stride,
		    force_keyframe || client->needs_full_frame,
		    &h264,
		    &h264_length
	    ))
		return false;
	if (!client->rdpgfx_update_logged) {
		const char *dump_path = g_getenv("RF_RDP_AVC_DUMP");
		if (dump_path != NULL && dump_path[0] != '\0') {
			GError *error = NULL;
			if (g_file_set_contents(
				    dump_path,
				    (const char *)h264,
				    h264_length,
				    &error
			    )) {
				g_message(
					"RDP: Dumped first live AVC420 H.264 frame to %s (%zu bytes).",
					dump_path,
					h264_length
				);
			} else {
				g_warning(
					"RDP: Failed to dump first live AVC420 frame: %s",
					error != NULL ? error->message : "unknown error"
				);
				g_clear_error(&error);
			}
		}
	}

	if (h264_length > UINT32_MAX - 14)
		return false;
	avc = g_malloc(14 + h264_length);
	avc_length = rf_rdp_gfx_write_avc420_bitmap_stream(
		avc,
		14 + h264_length,
		x,
		y,
		(uint16_t)(x + width),
		(uint16_t)(y + height),
		qp,
		quality,
		h264,
		h264_length
	);
	if (avc_length == 0)
		return false;

	wire_length = RF_RDP_GFX_HEADER_SIZE + 17 + avc_length;
	if (wire_length > SIZE_MAX - (RF_RDP_GFX_HEADER_SIZE + 8) -
	    (RF_RDP_GFX_HEADER_SIZE + 4))
		return false;
	raw_capacity =
		(RF_RDP_GFX_HEADER_SIZE + 8) + wire_length +
		(RF_RDP_GFX_HEADER_SIZE + 4);
	gfx = g_malloc(raw_capacity);

	frame_id = ++client->rdpgfx_frame_id;
	if (frame_id == 0)
		frame_id = ++client->rdpgfx_frame_id;

	length = rf_rdp_gfx_write_start_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id,
		(uint32_t)(g_get_monotonic_time() / 1000)
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_wire_to_surface_1(
		gfx + offset,
		raw_capacity - offset,
		RDP_RDPGFX_SURFACE_ID,
		RF_RDP_GFX_CODECID_AVC420,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		x,
		y,
		(uint16_t)(x + width),
		(uint16_t)(y + height),
		avc,
		avc_length
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_end_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id
	);
	if (length == 0)
		return false;
	offset += length;

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent, false))
		return false;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX AVC420 updates active using %s, rect %u,%u %ux%u, coded %ux%u, bitrate=%" G_GINT64_FORMAT ", qp=%u, gop=%u.",
			rf_rdp_avc_encoder_name(client->avc),
			x,
			y,
			width,
			height,
			coded_width,
			coded_height,
			bit_rate,
			qp,
			gop_size
		);
		client->rdpgfx_update_logged = true;
	}
	return true;
}

static bool send_rdpgfx_progressive_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const uint32_t right = (uint32_t)x + width;
	const uint32_t bottom = (uint32_t)y + height;
	const uint16_t codec_id = RF_RDP_GFX_CODECID_CAPROGRESSIVE;
	const char *codec_name = "Progressive";
	g_autoptr(GByteArray) progressive = NULL;
	g_autofree uint8_t *gfx = NULL;
	size_t wire_length = 0;
	size_t raw_capacity = 0;
	size_t offset = 0;
	size_t length = 0;
	uint32_t frame_id = 0;

	if (!client_can_use_rdpgfx_progressive(client) || frame_width == 0 ||
	    frame_height == 0 || width == 0 || height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX ||
	    right > UINT16_MAX || bottom > UINT16_MAX)
		return false;
	if (buf == NULL || buf->data == NULL ||
	    buf->len < (size_t)frame_width * frame_height * 4)
		return false;

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != frame_height) {
		if (!send_rdpgfx_surface_setup(
			    client,
			    (uint16_t)frame_width,
			    (uint16_t)frame_height,
			    bytes_sent
		    ))
			return false;
		rf_rdp_rfx_context_reset(client->rfx);
	}

	if (client->rfx == NULL) {
		client->rfx = rf_rdp_rfx_context_new();
		if (client->rfx == NULL)
			return false;
	}
	rf_rdp_rfx_context_set_quality_level(
		client->rfx,
		client->server->rdpgfx_video_quality_level,
		client->server->max_video_quality_level
	);

	progressive = rf_rdp_rfx_encode_progressive_rgba(
		client->rfx,
		buf->data,
		buf->len,
		(size_t)frame_width * 4,
		(uint16_t)frame_width,
		(uint16_t)frame_height,
		x,
		y,
		width,
		height
	);
	if (progressive == NULL || progressive->len == 0)
		return false;

	wire_length = RF_RDP_GFX_HEADER_SIZE + 13 + progressive->len;
	if (wire_length > SIZE_MAX - (RF_RDP_GFX_HEADER_SIZE + 8) -
	    (RF_RDP_GFX_HEADER_SIZE + 4))
		return false;
	raw_capacity =
		(RF_RDP_GFX_HEADER_SIZE + 8) + wire_length +
		(RF_RDP_GFX_HEADER_SIZE + 4);
	if (raw_capacity < wire_length)
		return false;

	gfx = g_malloc(raw_capacity);
	frame_id = ++client->rdpgfx_frame_id;
	if (frame_id == 0)
		frame_id = ++client->rdpgfx_frame_id;

	length = rf_rdp_gfx_write_start_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id,
		(uint32_t)(g_get_monotonic_time() / 1000)
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_wire_to_surface_2(
		gfx + offset,
		raw_capacity - offset,
		RDP_RDPGFX_SURFACE_ID,
		codec_id,
		0,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		progressive->data,
		progressive->len
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_end_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id
	);
	if (length == 0)
		return false;
	offset += length;

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent, false))
		return false;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX %s updates active, rect %u,%u %ux%u, quality=%u, tile-threads=%u.",
			codec_name,
			x,
			y,
			width,
			height,
			rf_rdp_rfx_context_get_quality_level(client->rfx),
			rf_rdp_rfx_context_get_thread_count(client->rfx)
		);
		client->rdpgfx_update_logged = true;
	}
	return true;
}

static bool send_rdpgfx_rfx_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const uint32_t right = (uint32_t)x + width;
	const uint32_t bottom = (uint32_t)y + height;
	g_autoptr(GByteArray) rfx = NULL;
	g_autofree uint8_t *gfx = NULL;
	size_t wire_length = 0;
	size_t raw_capacity = 0;
	size_t offset = 0;
	size_t length = 0;
	uint32_t frame_id = 0;

	if (!client_can_use_rdpgfx_rfx(client) || frame_width == 0 ||
	    frame_height == 0 || width == 0 || height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX ||
	    right > UINT16_MAX || bottom > UINT16_MAX)
		return false;
	if (buf == NULL || buf->data == NULL ||
	    buf->len < (size_t)frame_width * frame_height * 4)
		return false;

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != frame_height) {
		if (!send_rdpgfx_surface_setup(
			    client,
			    (uint16_t)frame_width,
			    (uint16_t)frame_height,
			    bytes_sent
		    ))
			return false;
		rf_rdp_rfx_context_reset(client->rfx);
	}

	if (client->rfx == NULL) {
		client->rfx = rf_rdp_rfx_context_new();
		if (client->rfx == NULL)
			return false;
	}
	rf_rdp_rfx_context_set_quality_level(
		client->rfx,
		client->server->rdpgfx_video_quality_level,
		client->server->max_video_quality_level
	);

	rfx = rf_rdp_rfx_encode_rgba(
		client->rfx,
		buf->data,
		buf->len,
		(size_t)frame_width * 4,
		(uint16_t)frame_width,
		(uint16_t)frame_height,
		x,
		y,
		width,
		height
	);
	if (rfx == NULL || rfx->len == 0)
		return false;

	wire_length = RF_RDP_GFX_HEADER_SIZE + 17 + rfx->len;
	if (wire_length > SIZE_MAX - (RF_RDP_GFX_HEADER_SIZE + 8) -
	    (RF_RDP_GFX_HEADER_SIZE + 4))
		return false;
	raw_capacity =
		(RF_RDP_GFX_HEADER_SIZE + 8) + wire_length +
		(RF_RDP_GFX_HEADER_SIZE + 4);
	if (raw_capacity < wire_length)
		return false;

	gfx = g_malloc(raw_capacity);
	frame_id = ++client->rdpgfx_frame_id;
	if (frame_id == 0)
		frame_id = ++client->rdpgfx_frame_id;

	length = rf_rdp_gfx_write_start_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id,
		(uint32_t)(g_get_monotonic_time() / 1000)
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_wire_to_surface_1(
		gfx + offset,
		raw_capacity - offset,
		RDP_RDPGFX_SURFACE_ID,
		RF_RDP_GFX_CODECID_CAVIDEO,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		x,
		y,
		(uint16_t)right,
		(uint16_t)bottom,
		rfx->data,
		rfx->len
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_end_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id
	);
	if (length == 0)
		return false;
	offset += length;

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent, false))
		return false;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX RemoteFX updates active, rect %u,%u %ux%u, quality=%u, tile-threads=%u.",
			x,
			y,
			width,
			height,
			rf_rdp_rfx_context_get_quality_level(client->rfx),
			rf_rdp_rfx_context_get_thread_count(client->rfx)
		);
		client->rdpgfx_update_logged = true;
	}
	return true;
}

static bool send_rdpgfx_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const uint32_t right = (uint32_t)x + width;
	const uint32_t bottom = (uint32_t)y + height;
	const size_t uncompressed_length = (size_t)width * height * 4;
	const size_t planar_capacity = rf_rdp_planar_rgba_size(width, height);
	g_autofree uint8_t *bgrx = NULL;
	g_autofree uint8_t *planar = NULL;
	g_autofree uint8_t *gfx = NULL;
	const uint8_t *bitmap_data = NULL;
	size_t bitmap_data_length = 0;
	uint16_t codec_id = RF_RDP_GFX_CODECID_UNCOMPRESSED;
	const char *codec_name = "uncompressed XRGB_8888";
	size_t offset = 0;
	size_t length = 0;
	size_t wire_length = 0;
	size_t raw_capacity = 0;
	uint32_t frame_id = 0;

	if (!client_can_use_rdpgfx(client) || width == 0 || height == 0 ||
	    frame_width == 0 || frame_height == 0 ||
	    right > UINT16_MAX || bottom > UINT16_MAX)
		return false;
	if (uncompressed_length == 0 ||
	    uncompressed_length / 4 / height != width)
		return false;

	if (send_rdpgfx_av1_update(
		    client,
		    buf,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height,
		    bytes_sent
	    ))
		return true;
	if (client_can_use_rdpgfx_av1(client)) {
		if (!client->rdpgfx_av1_unavailable_logged) {
			g_message(
				"RDP: RDPGFX AV1 update failed; disabling RDPGFX for this client."
			);
			client->rdpgfx_av1_unavailable_logged = true;
		}
		return false;
	}
	if (send_rdpgfx_avc444_update(
		    client,
		    buf,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height,
		    bytes_sent
	    ))
		return true;
	if (send_rdpgfx_avc420_update(
		    client,
		    buf,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height,
		    bytes_sent
	    ))
		return true;
	if (!client_can_use_rdpgfx_avc420(client) &&
	    !client->rdpgfx_avc_client_disabled_logged) {
		g_message(
			"RDP: Client did not enable AVC420 (caps flags=0x%08x); using Progressive/RemoteFX/PLANAR RDPGFX fallback.",
			client->rdpgfx_caps_flags
		);
		client->rdpgfx_avc_client_disabled_logged = true;
	}
	if (send_rdpgfx_progressive_update(
		    client,
		    buf,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height,
		    bytes_sent
	    ))
		return true;
	if (send_rdpgfx_rfx_update(
		    client,
		    buf,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height,
		    bytes_sent
	    ))
		return true;

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != frame_height) {
		if (frame_width > UINT16_MAX || frame_height > UINT16_MAX)
			return false;
		if (!send_rdpgfx_surface_setup(
			    client,
			    frame_width,
			    frame_height,
			    bytes_sent
		    ))
			return false;
	}

	if (planar_capacity > 0 && planar_capacity < uncompressed_length) {
		const size_t pixel_count = uncompressed_length / 4;
		planar = g_malloc(planar_capacity);
		if (pixel_count <= RDP_RDPGFX_PLANAR_RLE_MAX_PIXELS)
			length = rf_rdp_planar_encode_rgba_best(
				planar,
				planar_capacity,
				buf->data,
				buf->len,
				(size_t)frame_width * 4,
				x,
				y,
				width,
				height
			);
		else
			length = rf_rdp_planar_encode_rgba(
				planar,
				planar_capacity,
				buf->data,
				buf->len,
				(size_t)frame_width * 4,
				x,
				y,
				width,
				height
			);
		if (length > 0) {
			bitmap_data = planar;
			bitmap_data_length = length;
			codec_id = RF_RDP_GFX_CODECID_PLANAR;
			if ((planar[0] & RF_RDP_PLANAR_FORMAT_HEADER_RLE) != 0)
				codec_name = "PLANAR RLE";
			else
				codec_name = "PLANAR RGB";
		}
	}

	if (bitmap_data == NULL) {
		bgrx = g_malloc(uncompressed_length);
		if (!rf_rdp_core_convert_rgba_rect_to_bgrx_top_down(
			    bgrx,
			    uncompressed_length,
			    buf->data,
			    buf->len,
			    (size_t)frame_width * 4,
			    x,
			    y,
			    width,
			    height
		    ))
			return false;
		bitmap_data = bgrx;
		bitmap_data_length = uncompressed_length;
	}

	if (bitmap_data_length > SIZE_MAX - RF_RDP_GFX_HEADER_SIZE - 17)
		return false;
	wire_length = RF_RDP_GFX_HEADER_SIZE + 17 + bitmap_data_length;
	if (wire_length > SIZE_MAX - (RF_RDP_GFX_HEADER_SIZE + 8) -
	    (RF_RDP_GFX_HEADER_SIZE + 4))
		return false;
	raw_capacity =
		(RF_RDP_GFX_HEADER_SIZE + 8) + wire_length +
		(RF_RDP_GFX_HEADER_SIZE + 4);
	if (raw_capacity < wire_length)
		return false;

	gfx = g_malloc(raw_capacity);
	frame_id = ++client->rdpgfx_frame_id;
	if (frame_id == 0)
		frame_id = ++client->rdpgfx_frame_id;

	length = rf_rdp_gfx_write_start_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id,
		(uint32_t)(g_get_monotonic_time() / 1000)
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_wire_to_surface_1(
		gfx + offset,
		raw_capacity - offset,
		RDP_RDPGFX_SURFACE_ID,
		codec_id,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		x,
		y,
		(uint16_t)right,
		(uint16_t)bottom,
		bitmap_data,
		bitmap_data_length
	);
	if (length == 0)
		return false;
	offset += length;

	length = rf_rdp_gfx_write_end_frame(
		gfx + offset,
		raw_capacity - offset,
		frame_id
	);
	if (length == 0)
		return false;
	offset += length;

	if (!send_rdpgfx_gfx_payload(
		    client,
		    gfx,
		    offset,
		    bytes_sent,
		    rf_rdp_gfx_codec_payload_allows_zgfx(codec_id)
	    ))
		return false;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX WireToSurface updates active using %s.",
			codec_name
		);
		client->rdpgfx_update_logged = true;
	}
	return true;
}

static bool send_bitmap_tile(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const size_t row_bytes = (size_t)width * 4;
	const size_t bitmap_length = row_bytes * height;
	const size_t bgr_row_bytes = (size_t)width * 3;
	const size_t bgr_length = bgr_row_bytes * height;
	const size_t raw_packet_estimate = 15u + RDP_BITMAP_PDU_OVERHEAD + bitmap_length;
	g_autofree uint8_t *bgrx = NULL;
	g_autofree uint8_t *bgr = NULL;
	g_autofree uint8_t *rle = NULL;
	g_autofree uint8_t *packet = NULL;
	size_t rle_capacity = 0;
	size_t rle_length = 0;
	size_t packet_length = 0;

	packet = g_malloc0(RDP_BITMAP_PACKET_CAPACITY);
	if (bgr_length > 0) {
		bgr = g_malloc(bgr_length);
		rle_capacity = bgr_length + bgr_length / 31 + 32;
		rle = g_malloc(rle_capacity);
		if (!rf_rdp_core_convert_rgba_rect_to_bgr_bottom_up(
			    bgr,
			    bgr_length,
			    buf->data,
			    buf->len,
			    (size_t)frame_width * 4,
			    x,
			    y,
			    width,
			    height
		    ))
			return false;
		rle_length = rf_rdp_core_compress_bgr24_bitmap(
			rle,
			rle_capacity,
			bgr,
			width,
			height,
			bgr_row_bytes
		);
		packet_length = rf_rdp_core_write_compressed_bitmap_update(
			packet,
			RDP_BITMAP_PACKET_CAPACITY,
			RF_RDP_MCS_BASE_CHANNEL_ID,
			RF_RDP_DEFAULT_SHARE_ID,
			x,
			y,
			width,
			height,
			rle,
			rle_length,
			bgr_row_bytes,
			bgr_length
		);
		if (packet_length > 0 && packet_length < raw_packet_estimate) {
			if (!client->bitmap_rle_logged) {
				g_message(
					"RDP: Bitmap fallback using 24bpp RLE compression."
				);
				client->bitmap_rle_logged = true;
			}
				if (!client_write_exact(client, packet, packet_length))
					return false;
			if (bytes_sent != NULL)
				*bytes_sent += packet_length;
			return true;
		}
	}

	bgrx = g_malloc(bitmap_length);
	if (!rf_rdp_core_convert_rgba_rect_to_bgrx_bottom_up(
		    bgrx,
		    bitmap_length,
		    buf->data,
		    buf->len,
		    (size_t)frame_width * 4,
		    x,
		    y,
		    width,
		    height
	    ))
		return false;

	packet_length = rf_rdp_core_write_bitmap_update(
		packet,
		RDP_BITMAP_PACKET_CAPACITY,
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID,
		x,
		y,
		width,
		height,
		bgrx,
		row_bytes
	);
	if (packet_length == 0 ||
	    !client_write_exact(client, packet, packet_length))
		return false;
	if (bytes_sent != NULL)
		*bytes_sent += packet_length;
	return true;
}

static bool send_bitmap_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int max_bitmap_bytes =
		RDP_MAX_MCS_PAYLOAD - RDP_BITMAP_PDU_OVERHEAD;
	const unsigned int max_width = max_bitmap_bytes / 4;

	for (uint16_t tile_x = 0; tile_x < width;) {
		const uint16_t tile_width = MIN(width - tile_x, max_width);
		const unsigned int row_bytes = tile_width * 4;
		const uint16_t max_rows = max_bitmap_bytes / row_bytes;

		if (max_rows == 0)
			return false;
		for (uint16_t tile_y = 0; tile_y < height;) {
			const uint16_t tile_height = MIN(height - tile_y, max_rows);

			if (!send_bitmap_tile(
				    client,
				    buf,
				    frame_width,
				    x + tile_x,
				    y + tile_y,
				    tile_width,
				    tile_height,
				    bytes_sent
			    ))
				return false;
			tile_y += tile_height;
		}
		tile_x += tile_width;
	}

	return true;
}

static bool send_surface_nsc_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent,
	bool *unsupported
)
{
	const size_t rgba_stride = (size_t)frame_width * 4;
	const size_t rgba_offset = (size_t)y * rgba_stride + (size_t)x * 4;
	g_autoptr(GByteArray) encoded = NULL;
	g_autofree uint8_t *packet = NULL;
	size_t packet_capacity = 0;
	size_t packet_length = 0;

	*unsupported = false;
	if (rgba_offset > buf->len) {
		*unsupported = true;
		return false;
	}
	if (client->nsc == NULL)
		client->nsc = rf_rdp_nsc_context_new();
	if (client->nsc == NULL) {
		*unsupported = true;
		return false;
	}

	encoded = rf_rdp_nsc_encode_rgba(
		client->nsc,
		buf->data + rgba_offset,
		buf->len - rgba_offset,
		rgba_stride,
		width,
		height
	);
	if (encoded == NULL) {
		*unsupported = true;
		return false;
	}

	packet_capacity = encoded->len + 64;
	packet = g_malloc0(packet_capacity);
	packet_length = rf_rdp_core_write_surface_bits(
		packet,
		packet_capacity,
		RF_RDP_SURFCMD_SET_SURFACE_BITS,
		x,
		y,
		width,
		height,
		32,
		client->caps.nscodec_id,
		encoded->data,
		encoded->len
	);
	if (packet_length == 0) {
		*unsupported = true;
		return false;
	}
	if (!client_write_exact(client, packet, packet_length))
		return false;
	if (bytes_sent != NULL)
		*bytes_sent += packet_length;
	return true;
}

static bool send_graphics_update(
	struct client *client,
	const GByteArray *buf,
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent,
	bool *deferred
)
{
	if (deferred != NULL)
		*deferred = false;

	if (rf_rdp_core_should_defer_graphics_for_rdpgfx(
		    client->drdynvc_channel_id != 0,
		    client->rdpgfx_disabled,
		    client->rdpgfx_caps_confirmed
	    )) {
		if (deferred != NULL)
			*deferred = true;
		if (!client->rdpgfx_wait_logged) {
			g_message(
				"RDP: Waiting for RDPGFX capabilities before sending graphics update."
			);
			client->rdpgfx_wait_logged = true;
		}
		return true;
	}

	if (client_can_use_rdpgfx(client)) {
		if (send_rdpgfx_update(
			    client,
			    buf,
			    frame_width,
			    frame_height,
			    x,
			    y,
			    width,
			    height,
			    bytes_sent
		    ))
			return true;

		client->rdpgfx_disabled = true;
		client->rdpgfx_surface_ready = false;
		g_warning(
			"RDP: RDPGFX WireToSurface update failed; using legacy graphics fallback."
		);
	}

	if (client->graphics_mode == RF_RDP_GRAPHICS_MODE_SURFACE_NSC) {
		bool unsupported = false;

		if (send_surface_nsc_update(
			    client,
			    buf,
			    frame_width,
			    x,
			    y,
			    width,
			    height,
			    bytes_sent,
			    &unsupported
		    ))
			return true;
		if (!unsupported)
			return false;
		if (!client->surface_fallback_logged) {
			g_message(
				"RDP: SurfaceBits NSCodec update too large or unavailable; using bitmap fallback."
			);
			client->surface_fallback_logged = true;
		}
	}

	return send_bitmap_update(
		client,
		buf,
		frame_width,
		x,
		y,
		width,
		height,
		bytes_sent
	);
}

static gpointer client_thread(void *data)
{
	struct client *client = data;
	RfRDPServer *this = client->server;
	GInputStream *input = g_io_stream_get_input_stream(
		G_IO_STREAM(client->connection)
	);
	GOutputStream *output = g_io_stream_get_output_stream(
		G_IO_STREAM(client->connection)
	);
	g_autoptr(GByteArray) request_pdu = read_tpkt(input);
	struct rf_rdp_connection_request request = { 0 };

	if (request_pdu == NULL ||
	    !rf_rdp_parse_connection_request(
		    request_pdu->data, request_pdu->len, &request
	    ))
		goto out;

	if (!negotiate_security(client, &request, output))
		goto out;

	if (!handle_mcs_connect_sequence(client))
		goto out;
	handle_active_session(client);

out:
	remove_client(client);
	if (this->running)
		g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
	client_unref(client);
	return NULL;
}

static ssize_t on_rdp_clipboard_msg(
	RfRDPServer *this,
	GSocketConnection *connection
)
{
	size_t length = 0;
	gsize read = 0;
	g_autofree uint8_t *data = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GByteArray) wire = NULL;
	GInputStream *input =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));
	g_auto(RfClipboardRichPayload) payload;
	struct rf_rdp_cliprdr_format_list formats = { 0 };
	bool has_text = false;
	bool has_html = false;
	bool has_image = false;

	if (!g_input_stream_read_all(
		    input,
		    &length,
		    sizeof(length),
		    &read,
		    NULL,
		    &error
	    ) || read != sizeof(length))
		goto fail;
	if (length == 0 || length > RF_CLIPBOARD_RICH_MAX_BYTES)
		return -1;

	data = g_malloc(length);
	if (!g_input_stream_read_all(input, data, length, &read, NULL, &error) ||
	    read != length)
		goto fail;
	wire = g_byte_array_sized_new(length);
	g_byte_array_append(wire, data, length);

	rf_clipboard_rich_payload_init(&payload);
	if (!rf_clipboard_rich_payload_parse(data, length, &payload)) {
		g_warning("RDP: Ignoring malformed rich clipboard payload length %zu.",
			length);
		return -1;
	}
	has_text = payload.text != NULL;
	has_html = payload.html != NULL;
	has_image = payload.image_rgba != NULL;

	g_mutex_lock(&this->lock);
	if (rf_clipboard_rich_wire_equal(this->clipboard_wire, wire)) {
		g_mutex_unlock(&this->lock);
		g_message("RDP: Skipping duplicate rich clipboard from helper.");
		return length;
	}
	rf_clipboard_rich_payload_clear(&this->rich_clipboard);
	this->rich_clipboard = payload;
	rf_clipboard_rich_payload_init(&payload);
	g_free(this->clipboard_text);
	this->clipboard_text = this->rich_clipboard.text != NULL ?
		g_strdup(this->rich_clipboard.text) :
		NULL;
	g_clear_pointer(&this->clipboard_wire, g_byte_array_unref);
	this->clipboard_wire = g_byte_array_ref(wire);
	clipboard_formats_locked(this, &formats);
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client->cliprdr_channel_id != 0 && client->cliprdr_ready &&
		    client->cliprdr_client_caps_received &&
		    !send_cliprdr_format_list_with_formats(client, &formats))
			g_warning("RDP: Failed to advertise rich clipboard.");
	}
	g_mutex_unlock(&this->lock);

	g_message(
		"RDP: Received rich clipboard from helper text=%s html=%s image=%s length=%zu.",
		yes_no(has_text),
		yes_no(has_html),
		yes_no(has_image),
		length
	);
	return length;

fail:
	if (error != NULL)
		g_warning("RDP: Failed to receive rich clipboard: %s.",
			error->message);
	return -1;
}

static int on_rdp_clipboard_socket_in(
	GSocket *socket,
	GIOCondition condition,
	void *data
)
{
	RfRDPServer *this = data;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	char type = 0;
	g_autoptr(GSocketConnection) connection =
		g_socket_connection_factory_create_connection(socket);
	GInputStream *input =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));

	if (!(condition & (G_IO_IN | G_IO_PRI)))
		return G_SOURCE_CONTINUE;
	ret = g_input_stream_read(input, &type, sizeof(type), NULL, &error);
	if (ret <= 0)
		goto out;

	switch (type) {
	case RF_MSG_TYPE_RDP_CLIPBOARD_RICH:
		ret = on_rdp_clipboard_msg(this, connection);
		break;
	default:
		g_message("RDP: Ignoring unsupported rich clipboard message %c.", type);
		break;
	}

out:
	if (ret <= 0) {
		if (error != NULL)
			g_warning("RDP: Rich clipboard helper disconnected: %s.",
				error->message);
		g_hash_table_remove(this->clipboard_sockets, socket);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static gboolean on_rdp_clipboard_incoming(
	GSocketService *service,
	GSocketConnection *connection,
	GObject *source_object,
	void *data
)
{
	(void)service;
	(void)source_object;
	RfRDPServer *this = data;
	GSocket *socket = g_socket_connection_get_socket(connection);
	GSource *source = g_socket_create_source(
		socket,
		G_IO_IN | G_IO_PRI,
		NULL
	);

	g_source_set_callback(
		source,
		G_SOURCE_FUNC(on_rdp_clipboard_socket_in),
		this,
		NULL
	);
	g_source_attach(source, NULL);
	g_hash_table_insert(this->clipboard_sockets, g_object_ref(socket), source);
	g_message("RDP: Rich clipboard helper connected.");
	send_rdp_clipboard_payload(this, true);
	return true;
}

static gboolean on_incoming(
	GSocketService *service,
	GSocketConnection *connection,
	GObject *source_object,
	void *data
)
{
	(void)service;
	(void)source_object;
	RfRDPServer *this = data;
	struct client *client = g_new0(struct client, 1);

	g_atomic_int_set(&client->ref_count, 1);
	client->server = this;
	client->rdpgfx_channel_id = RDP_RDPGFX_DVC_CHANNEL_ID;
	client->rdpsnd_selected_format = -1;
	g_mutex_init(&client->write_lock);
	client->connection = g_object_ref(connection);
	client->stream = g_object_ref(G_IO_STREAM(connection));
	client->thread = g_thread_new("rf-rdp-client", client_thread, client);
	g_thread_unref(client->thread);
	return true;
}

static void listen_tcp(RfRDPServer *this, const char *ip, unsigned int port)
{
	if (ip != NULL && ip[0] == '\0')
		return;

	g_autoptr(GError) error = NULL;
	if (ip != NULL) {
		g_autoptr(GSocketAddress) address =
			g_inet_socket_address_new_from_string(ip, port);
		g_socket_listener_add_address(
			G_SOCKET_LISTENER(this->service),
			address,
			G_SOCKET_TYPE_STREAM,
			G_SOCKET_PROTOCOL_TCP,
			NULL,
			NULL,
			&error
		);
	} else {
		g_socket_listener_add_inet_port(
			G_SOCKET_LISTENER(this->service), port, NULL, &error
		);
	}
	if (error != NULL)
		g_warning("RDP: Failed to listen on %s:%u: %s.",
			ip,
			port,
			error->message);
	else
		g_message("RDP: Listening on %s:%u.", ip ? ip : "0.0.0.0", port);
}

static void start_rdp_clipboard_service(RfRDPServer *this)
{
	if (!this->clipboard || this->clipboard_socket_path == NULL)
		return;

	g_autoptr(GError) error = NULL;
	g_autofree char *dir = g_path_get_dirname(this->clipboard_socket_path);

	g_mkdir_with_parents(dir, 0755);
	rf_set_group(dir);
	this->clipboard_service = g_socket_service_new();
	g_clear_object(&this->clipboard_address);
	this->clipboard_address =
		g_unix_socket_address_new(this->clipboard_socket_path);
	g_remove(this->clipboard_socket_path);
	const mode_t previous_umask = umask(0007);
	g_socket_listener_add_address(
		G_SOCKET_LISTENER(this->clipboard_service),
		this->clipboard_address,
		G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_DEFAULT,
		NULL,
		NULL,
		&error
	);
	umask(previous_umask);
	rf_set_group(this->clipboard_socket_path);
	g_chmod(this->clipboard_socket_path, 0660);
	if (error != NULL) {
		g_warning("RDP: Failed to listen on rich clipboard socket %s: %s.",
			this->clipboard_socket_path,
			error->message);
		g_clear_object(&this->clipboard_service);
		return;
	}
	g_signal_connect(
		this->clipboard_service,
		"incoming",
		G_CALLBACK(on_rdp_clipboard_incoming),
		this
	);
	g_socket_service_start(this->clipboard_service);
	g_message("RDP: Rich clipboard listening on %s.",
		this->clipboard_socket_path);
}

static void audio_connection_free(struct audio_connection *connection)
{
	if (connection == NULL)
		return;
	g_clear_object(&connection->connection);
	g_clear_object(&connection->server);
	g_free(connection);
}

static void audio_frame_free(struct audio_frame *frame)
{
	if (frame == NULL)
		return;
	g_clear_pointer(&frame->pcm, g_byte_array_unref);
	g_free(frame);
}

static void clear_queued_audio_frames(RfRDPServer *this)
{
	if (this == NULL)
		return;

	g_mutex_lock(&this->audio_lock);
	if (this->audio_queue != NULL)
		g_queue_clear_full(this->audio_queue, (GDestroyNotify)audio_frame_free);
	this->audio_queue_dropped = 0;
	this->audio_silence_suppressed = false;
	this->audio_silent_frames = 0;
	g_mutex_unlock(&this->audio_lock);
}

static void reset_rdpsnd_first_audio_markers(RfRDPServer *this)
{
	if (this == NULL)
		return;

	g_mutex_lock(&this->lock);
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		client->rdpsnd_first_audio_sent = false;
		client->rdpsnd_first_audio_confirmed = false;
		client->rdpsnd_first_audio_sent_time_us = 0;
	}
	g_mutex_unlock(&this->lock);
}

static unsigned int audio_queue_max_frames(unsigned int frame_ms)
{
	if (frame_ms == 0)
		return 2;
	return MAX(2u, RDP_AUDIO_QUEUE_TARGET_MS / frame_ms);
}

static unsigned int audio_silence_hold_frames(unsigned int frame_ms)
{
	if (frame_ms == 0)
		return 2;
	return MAX(1u, RDP_AUDIO_SILENCE_HOLD_MS / frame_ms);
}

static void queue_audio_frame(
	RfRDPServer *this,
	const struct rf_rdp_audio_pcm_header *header,
	GByteArray *pcm
)
{
	struct audio_frame *frame = NULL;
	const unsigned int max_frames = audio_queue_max_frames(header->frame_ms);
	const unsigned int silence_hold_frames =
		audio_silence_hold_frames(header->frame_ms);
	const bool silent = rf_rdp_audio_pcm_is_silent(
		pcm->data,
		pcm->len,
		RDP_AUDIO_SILENCE_THRESHOLD
	);

	if (this->audio_queue == NULL)
		return;

	if (silent && this->audio_silence_suppressed) {
		g_mutex_lock(&this->audio_lock);
		this->audio_stats_silence_suppressed++;
		g_mutex_unlock(&this->audio_lock);
		return;
	}
	if (!silent && this->audio_silence_suppressed) {
		this->audio_silence_suppressed = false;
		this->audio_silent_frames = 0;
		clear_queued_audio_frames(this);
		reset_rdpsnd_first_audio_markers(this);
		g_message("RDP: Audio resumed after suppressed silence; cleared queued audio.");
	}
	if (!silent)
		this->audio_silent_frames = 0;

	frame = g_new0(struct audio_frame, 1);
	frame->header = *header;
	frame->pcm = g_byte_array_ref(pcm);

	g_mutex_lock(&this->audio_lock);
	frame->sequence = ++this->audio_sequence;
	while (this->audio_queue->length >= max_frames) {
		struct audio_frame *oldest = g_queue_pop_head(this->audio_queue);

		audio_frame_free(oldest);
		this->audio_queue_dropped++;
	}
	g_queue_push_tail(this->audio_queue, frame);
	if (silent)
		this->audio_silent_frames++;
	if (silent && this->audio_silent_frames >= silence_hold_frames) {
		this->audio_silence_suppressed = true;
		g_message(
			"RDP: Audio silence suppression active after %u ms.",
			this->audio_silent_frames * header->frame_ms
		);
	}
	g_cond_signal(&this->audio_cond);
	g_mutex_unlock(&this->audio_lock);
}

static void *audio_sender_thread(void *data)
{
	RfRDPServer *this = data;

	while (true) {
		struct audio_frame *frame = NULL;
		uint64_t skipped = 0;

		g_mutex_lock(&this->audio_lock);
		while (this->running &&
		       (this->audio_queue == NULL || this->audio_queue->length == 0))
			g_cond_wait(&this->audio_cond, &this->audio_lock);
		if (!this->running) {
			g_mutex_unlock(&this->audio_lock);
			break;
		}
		frame = g_queue_pop_head(this->audio_queue);
		skipped = this->audio_queue_dropped;
		this->audio_queue_dropped = 0;
		g_mutex_unlock(&this->audio_lock);

		if (frame != NULL) {
			send_audio_to_clients(
				this,
				&frame->header,
				frame->pcm,
				skipped
			);
			audio_frame_free(frame);
		}
	}

	return NULL;
}

static void *audio_connection_thread(void *data)
{
	struct audio_connection *connection = data;
	RfRDPServer *this = connection->server;
	GInputStream *input =
		g_io_stream_get_input_stream(G_IO_STREAM(connection->connection));
	unsigned int frames = 0;

	while (this->running) {
		struct rf_rdp_audio_pcm_header header = { 0 };
		g_autoptr(GByteArray) pcm = NULL;
		g_autoptr(GError) error = NULL;

		if (!rf_rdp_audio_stream_read_pcm(input, &header, &pcm, &error)) {
			if (error != NULL)
				g_message("RDP: Audio helper disconnected: %s.",
					error->message);
			break;
		}
		if (!this->running)
			break;
		queue_audio_frame(this, &header, pcm);
		frames++;
		if (frames == 1)
			g_message(
				"RDP: Received first audio frame from helper: %u Hz/%u channel(s), %u ms, %u bytes.",
				header.sample_rate,
				header.channels,
				header.frame_ms,
				header.pcm_length
			);
	}

	audio_connection_free(connection);
	return NULL;
}

static gboolean on_rdp_audio_incoming(
	GSocketService *service,
	GSocketConnection *connection,
	GObject *source_object,
	void *data
)
{
	(void)service;
	(void)source_object;
	RfRDPServer *this = data;
	struct audio_connection *audio_connection = g_new0(
		struct audio_connection,
		1
	);
	GThread *thread = NULL;

	audio_connection->server = g_object_ref(this);
	audio_connection->connection = g_object_ref(connection);
	thread = g_thread_new(
		"rf-rdp-audio",
		audio_connection_thread,
		audio_connection
	);
	g_thread_unref(thread);
	g_message("RDP: Audio helper connected.");
	return true;
}

static bool start_rdp_audio_service(RfRDPServer *this)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketAddress) address = NULL;
	g_autofree char *dir = NULL;

	if (!this->audio || this->rdp_audio_socket_path == NULL)
		return true;

	dir = g_path_get_dirname(this->rdp_audio_socket_path);
	g_mkdir_with_parents(dir, 0755);
	rf_set_group(dir);
	this->audio_service = g_socket_service_new();
	address = g_unix_socket_address_new(this->rdp_audio_socket_path);
	g_remove(this->rdp_audio_socket_path);
	const mode_t previous_umask = umask(0007);
	g_socket_listener_add_address(
		G_SOCKET_LISTENER(this->audio_service),
		address,
		G_SOCKET_TYPE_STREAM,
		G_SOCKET_PROTOCOL_DEFAULT,
		NULL,
		NULL,
		&error
	);
	umask(previous_umask);
	rf_set_group(this->rdp_audio_socket_path);
	g_chmod(this->rdp_audio_socket_path, 0660);
	if (error != NULL) {
		g_warning("RDP: Failed to listen on audio socket %s: %s.",
			this->rdp_audio_socket_path,
			error->message);
		g_clear_object(&this->audio_service);
		return false;
	}
	g_signal_connect(
		this->audio_service,
		"incoming",
		G_CALLBACK(on_rdp_audio_incoming),
		this
	);
	g_socket_service_start(this->audio_service);
	this->audio_sender_thread = g_thread_new(
		"rf-rdp-audio-send",
		audio_sender_thread,
		this
	);
	g_message("RDP: Audio helper listening on %s.",
		this->rdp_audio_socket_path);
	return true;
}

static void start(RfRemoteServer *super)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	if (this->running)
		return;
	if (this->tls_private_key_file == NULL ||
	    this->tls_certificate_file == NULL)
		g_error("RDP: TLS private key and certificate are required.");

	g_autoptr(GError) error = NULL;
	this->certificate = g_tls_certificate_new_from_files(
		this->tls_certificate_file,
		this->tls_private_key_file,
		&error
	);
	if (this->certificate == NULL)
		g_error("RDP: Failed to load TLS certificate/key: %s.", error->message);

	this->service = g_socket_service_new();
	if (this->ips != NULL) {
		for (int i = 0; this->ips[i] != NULL; ++i)
			listen_tcp(this, this->ips[i], this->port);
	} else {
		listen_tcp(this, NULL, this->port);
	}
	g_signal_connect(
		this->service, "incoming", G_CALLBACK(on_incoming), this
	);
	this->running = true;
	g_socket_service_start(this->service);
	start_rdp_clipboard_service(this);
	if (!start_rdp_audio_service(this))
		g_warning("RDP: Audio socket disabled after startup failure.");
}

static bool is_running(RfRemoteServer *super)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	return this->running;
}

static unsigned int full_frame_client_count(RfRDPServer *this)
{
	unsigned int full_clients = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client->needs_full_frame)
			full_clients++;
	}
	return full_clients;
}

static unsigned int bitmap_limited_client_count(RfRDPServer *this)
{
	unsigned int limited_clients = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if ((!client_can_use_rdpgfx(client) &&
		     client->graphics_mode == RF_RDP_GRAPHICS_MODE_BITMAP) ||
		    (client_can_use_rdpgfx(client) &&
		     !rf_rdp_gfx_codec_is_video(client->rdpgfx_policy_codec)))
			limited_clients++;
	}
	return limited_clients;
}

static unsigned int rdpgfx_video_client_count(RfRDPServer *this)
{
	unsigned int video_clients = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client_can_use_rdpgfx(client) &&
		    rf_rdp_gfx_codec_is_video(client->rdpgfx_policy_codec))
			video_clients++;
	}
	return video_clients;
}

static unsigned int rdpgfx_rfx_style_client_count(RfRDPServer *this)
{
	unsigned int rfx_clients = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client_can_use_rdpgfx(client) &&
		    (client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_REMOTEFX ||
		     client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_PROGRESSIVE ||
		     client->rdpgfx_policy_codec == RF_RDP_GFX_CODEC_PROGRESSIVE_V2))
			rfx_clients++;
	}
	return rfx_clients;
}

static void maybe_log_stats(
	RfRDPServer *this,
	int64_t now,
	unsigned int full_clients
)
{
	if (this->stats_last_log_time_us == 0) {
		this->stats_last_log_time_us = now;
		return;
	}
	if (now - this->stats_last_log_time_us < RDP_STATS_INTERVAL_US)
		return;

	const uint64_t avg_send_time_us = this->stats_frames_sent == 0 ?
		0 :
		this->stats_send_time_us / this->stats_frames_sent;
	const int64_t interval_us = now - this->stats_last_log_time_us;
	const unsigned int limited_clients = bitmap_limited_client_count(this);
	const unsigned int video_clients = rdpgfx_video_client_count(this);
	const unsigned int rfx_clients = rdpgfx_rfx_style_client_count(this);
	const unsigned int quality_clients = video_clients + rfx_clients;
	const unsigned int rfx_limited_clients = MIN(limited_clients, rfx_clients);
	const unsigned int non_quality_limited_clients =
		limited_clients - rfx_limited_clients;
	const bool quality_auto_clients =
		this->configured_video_quality == RF_CONFIG_RDP_VIDEO_QUALITY_AUTO &&
		quality_clients > 0;
	const bool limit_rfx_fps =
		rf_rdp_core_should_limit_fallback_fps_for_quality_state(
			rfx_limited_clients > 0,
			quality_auto_clients,
			this->rdpgfx_video_quality_level,
			this->max_video_quality_level
		);
	const unsigned int fps_limited_clients =
		non_quality_limited_clients +
		(limit_rfx_fps ? rfx_limited_clients : 0);
	const unsigned int previous_fps = this->adaptive_fps;
	const unsigned int previous_video_quality =
		this->rdpgfx_video_quality_level;
	const unsigned int video_quality_target_fps =
		this->stats_min_target_fps != 0 ?
			this->stats_min_target_fps :
			(this->adaptive_fps != 0 ? this->adaptive_fps : this->max_fps);
	const int64_t video_quality_change_age_us =
		this->rdpgfx_video_quality_last_change_time_us > 0 ?
			now - this->rdpgfx_video_quality_last_change_time_us :
			0;

	this->adaptive_fps = rf_rdp_core_update_adaptive_fps(
		this->adaptive_fps,
		this->max_fps,
		RDP_BITMAP_MIN_FPS,
		this->stats_bytes_sent,
		interval_us,
		RDP_BITMAP_TARGET_BYTES_PER_SECOND,
		avg_send_time_us,
		fps_limited_clients > 0
	);
	if (this->configured_video_quality == RF_CONFIG_RDP_VIDEO_QUALITY_AUTO) {
		this->rdpgfx_video_quality_level =
			rf_rdp_core_update_video_quality_level_stable(
				this->rdpgfx_video_quality_level,
				this->max_video_quality_level,
				this->stats_bytes_sent,
				interval_us,
				this->rdpgfx_target_bytes_per_second,
				video_quality_target_fps,
				avg_send_time_us,
				this->stats_frames_sent,
				this->stats_frames_skipped,
				this->stats_max_rdpgfx_inflight,
				this->stats_max_rdpgfx_qoe_time_diff_se,
				this->stats_max_rdpgfx_qoe_time_diff_edr,
				quality_clients > 0,
				video_quality_change_age_us
			);
	} else {
		this->rdpgfx_video_quality_level =
			(unsigned int)this->configured_video_quality;
	}
	if (this->adaptive_fps != previous_fps) {
		g_message(
			"RDP: Adaptive graphics FPS changed from %u to %u.",
			previous_fps,
			this->adaptive_fps
		);
	}
	if (this->rdpgfx_video_quality_level != previous_video_quality) {
		this->rdpgfx_video_quality_last_change_time_us = now;
		g_message(
			"RDP: RDPGFX graphics quality level changed from %u to %u.",
			previous_video_quality,
			this->rdpgfx_video_quality_level
		);
	}
	g_message(
		"RDP: Stats configured-fps=%u, effective-fps=%u, target-fps-min=%u, video-quality=%u, target-bandwidth=%uMbps, sent=%" G_GUINT64_FORMAT ", skipped=%" G_GUINT64_FORMAT ", bytes=%" G_GUINT64_FORMAT ", limited-clients=%u, fps-limited-clients=%u, rdpgfx-video-clients=%u, rfx-style-clients=%u, full-frame-clients=%u, rdpgfx-inflight-max=%u, ack-depth-max=%u, qoe-se/edr-max=%u/%u, zgfx=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " (payloads/saved-bytes), avg-send=%" G_GUINT64_FORMAT "ms, avc444-lc=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " (both/single/chroma).",
		this->max_fps,
		this->adaptive_fps,
		this->stats_min_target_fps,
		this->rdpgfx_video_quality_level,
		this->rdpgfx_target_bandwidth_mbps,
		this->stats_frames_sent,
		this->stats_frames_skipped,
		this->stats_bytes_sent,
		limited_clients,
		fps_limited_clients,
		video_clients,
		rfx_clients,
		full_clients,
		this->stats_max_rdpgfx_inflight,
		this->stats_max_rdpgfx_ack_queue_depth,
		this->stats_max_rdpgfx_qoe_time_diff_se,
		this->stats_max_rdpgfx_qoe_time_diff_edr,
		this->stats_rdpgfx_zgfx_payloads,
		this->stats_rdpgfx_zgfx_saved_bytes,
		avg_send_time_us / 1000,
		this->stats_avc444_lc[0],
		this->stats_avc444_lc[1],
		this->stats_avc444_lc[2]
	);
	this->stats_last_log_time_us = now;
	this->stats_frames_sent = 0;
	this->stats_frames_skipped = 0;
	this->stats_bytes_sent = 0;
	this->stats_send_time_us = 0;
	for (unsigned int i = 0; i < RDP_RDPGFX_AVC444_LC_STAT_COUNT; ++i)
		this->stats_avc444_lc[i] = 0;
	this->stats_min_target_fps = 0;
	this->stats_max_rdpgfx_inflight = 0;
	this->stats_max_rdpgfx_ack_queue_depth = 0;
	this->stats_max_rdpgfx_qoe_time_diff_se = 0;
	this->stats_max_rdpgfx_qoe_time_diff_edr = 0;
	this->stats_rdpgfx_zgfx_payloads = 0;
	this->stats_rdpgfx_zgfx_saved_bytes = 0;
}

static bool should_render_frame(RfRemoteServer *super)
{
	RfRDPServer *this = RF_RDP_SERVER(super);
	const int64_t now = g_get_monotonic_time();
	bool render = false;
	unsigned int full_clients = 0;
	unsigned int target_fps = 0;

	g_mutex_lock(&this->lock);
	if (this->clients == NULL) {
		g_mutex_unlock(&this->lock);
		return false;
	}

	full_clients = full_frame_client_count(this);
	target_fps = rdpgfx_ack_limited_fps(this, this->adaptive_fps);
	if (this->stats_min_target_fps == 0 ||
	    target_fps < this->stats_min_target_fps)
		this->stats_min_target_fps = target_fps;
	this->stats_max_rdpgfx_inflight = MAX(
		this->stats_max_rdpgfx_inflight,
		max_rdpgfx_inflight_frames(this)
	);
	this->stats_max_rdpgfx_ack_queue_depth = MAX(
		this->stats_max_rdpgfx_ack_queue_depth,
		max_rdpgfx_ack_queue_depth(this)
	);
	render = rf_rdp_core_frame_scheduler_should_render(
		&this->next_render_time_us,
		now,
		target_fps,
		full_clients > 0
	);
	if (!render)
		this->stats_frames_skipped++;
	maybe_log_stats(this, now, full_clients);
	g_mutex_unlock(&this->lock);
	return render;
}

static void stop(RfRemoteServer *super)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	if (!this->running)
		return;

	this->running = false;
	g_mutex_lock(&this->audio_lock);
	g_cond_signal(&this->audio_cond);
	g_mutex_unlock(&this->audio_lock);
	rf_remote_server_flush(super);
	if (this->audio_sender_thread != NULL) {
		g_thread_join(this->audio_sender_thread);
		this->audio_sender_thread = NULL;
	}
	g_mutex_lock(&this->audio_lock);
	if (this->audio_queue != NULL)
		g_queue_clear_full(this->audio_queue, (GDestroyNotify)audio_frame_free);
	this->audio_queue_dropped = 0;
	this->audio_silent_frames = 0;
	this->audio_silence_suppressed = false;
	g_mutex_unlock(&this->audio_lock);
	if (this->clipboard_sockets != NULL) {
		GHashTableIter it;
		void *value;

		g_hash_table_iter_init(&it, this->clipboard_sockets);
		while (g_hash_table_iter_next(&it, NULL, &value))
			g_source_destroy(value);
		g_hash_table_remove_all(this->clipboard_sockets);
	}
	if (this->clipboard_service != NULL) {
		g_socket_service_stop(this->clipboard_service);
		g_socket_listener_close(G_SOCKET_LISTENER(this->clipboard_service));
		g_clear_object(&this->clipboard_service);
	}
	g_clear_object(&this->clipboard_address);
	if (this->clipboard_socket_path != NULL)
		g_remove(this->clipboard_socket_path);
	if (this->audio_service != NULL) {
		g_socket_service_stop(this->audio_service);
		g_socket_listener_close(G_SOCKET_LISTENER(this->audio_service));
		g_clear_object(&this->audio_service);
	}
	if (this->rdp_audio_socket_path != NULL)
		g_remove(this->rdp_audio_socket_path);
	g_socket_service_stop(this->service);
	g_socket_listener_close(G_SOCKET_LISTENER(this->service));
	g_clear_object(&this->service);
	g_clear_object(&this->certificate);
}

static void set_desktop_name(RfRemoteServer *super, const char *desktop_name)
{
	(void)super;
	(void)desktop_name;
}

static void set_rdp_clipboard_socket_path(
	RfRemoteServer *super,
	const char *socket_path
)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	g_free(this->clipboard_socket_path);
	this->clipboard_socket_path = g_strdup(socket_path);
}

static void set_rdp_audio_socket_path(
	RfRemoteServer *super,
	const char *socket_path
)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	g_free(this->rdp_audio_socket_path);
	this->rdp_audio_socket_path = g_strdup(socket_path);
}

static void send_clipboard_text(RfRemoteServer *super, const char *text)
{
	RfRDPServer *this = RF_RDP_SERVER(super);
	struct rf_rdp_cliprdr_format_list formats = { 0 };

	if (!this->clipboard)
		return;
	g_mutex_lock(&this->lock);
	if (g_strcmp0(this->clipboard_text, text) == 0) {
		g_mutex_unlock(&this->lock);
		return;
	}
	g_free(this->clipboard_text);
	this->clipboard_text = g_strdup(text);
	rf_clipboard_rich_payload_clear(&this->rich_clipboard);
	rf_clipboard_rich_payload_set_text(&this->rich_clipboard, text);
	clipboard_formats_locked(this, &formats);
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client->cliprdr_channel_id != 0 && client->cliprdr_ready &&
		    client->cliprdr_client_caps_received &&
		    !send_cliprdr_format_list_with_formats(client, &formats))
			g_warning("RDP: Failed to advertise clipboard text.");
	}
	g_mutex_unlock(&this->lock);
}

static void update(
	RfRemoteServer *super,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage
)
{
	RfRDPServer *this = RF_RDP_SERVER(super);
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t w = 0;
	uint16_t h = 0;
	size_t pixels = 0;
	size_t frame_bytes = 0;
	uint16_t log_x = 0;
	uint16_t log_y = 0;
	uint16_t log_w = 0;
	uint16_t log_h = 0;
	unsigned int log_frame_width = 0;
	unsigned int log_frame_height = 0;
	bool have_log_rect = false;
	unsigned int full_clients = 0;
	size_t bytes_sent = 0;
	const char *log_mode = "graphics";

	if (width == 0 || height == 0)
		return;
	if ((size_t)width > SIZE_MAX / height)
		return;
	pixels = (size_t)width * height;
	if (pixels > SIZE_MAX / 4)
		return;
	frame_bytes = pixels * 4;
	if (frame_bytes > G_MAXUINT)
		return;
	if (buf->len < frame_bytes) {
		g_warning("RDP: Frame buffer is too small: got %u, need %zu.",
			buf->len,
			frame_bytes);
		return;
	}

	g_mutex_lock(&this->lock);
	this->width = width;
	this->height = height;
	const bool source_size_changed = this->last_frame != NULL &&
		(this->last_frame_width != width || this->last_frame_height != height);
	const int64_t start = g_get_monotonic_time();
	unsigned int clients = 0;
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;
		uint16_t client_frame_width = 0;
		uint16_t client_frame_height = 0;
		uint16_t send_x = 0;
		uint16_t send_y = 0;
		uint16_t send_w = 0;
		uint16_t send_h = 0;
		unsigned int send_frame_width = width;
		unsigned int send_frame_height = height;
		const GByteArray *send_buf = buf;
		g_autoptr(GByteArray) client_frame = NULL;

		if (!get_client_frame_size(
			    client,
			    width,
			    height,
			    &client_frame_width,
			    &client_frame_height
		    )) {
			g_warning("RDP: Ignoring client with invalid desktop size.");
			continue;
		}
		const bool full_frame = client->needs_full_frame ||
			source_size_changed ||
			client_needs_rdpgfx_surface(
				client,
				client_frame_width,
				client_frame_height
			);

		if (!rf_rdp_core_clip_update_rect(
			    width,
			    height,
			    damage,
			    full_frame,
			    &x,
			    &y,
			    &w,
			    &h
		    )) {
			g_warning("RDP: Ignoring empty bitmap update rect for %ux%u.",
				width,
				height);
			continue;
		}
		if (!rf_rdp_core_map_update_rect_to_client_surface(
			    width,
			    height,
			    client_frame_width,
			    client_frame_height,
			    x,
			    y,
			    w,
			    h,
			    full_frame,
			    &send_x,
			    &send_y,
			    &send_w,
			    &send_h
		    )) {
			g_warning(
				"RDP: Ignoring update that maps outside client surface %ux%u.",
				client_frame_width,
				client_frame_height
			);
			continue;
		}
		if (client_frame_width != width || client_frame_height != height) {
			client_frame = render_client_surface_frame(
				buf,
				width,
				height,
				client_frame_width,
				client_frame_height
			);
			if (client_frame == NULL) {
				g_warning(
					"RDP: Failed to render client-sized frame %ux%u.",
					client_frame_width,
					client_frame_height
				);
				g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
				continue;
			}
			send_buf = client_frame;
			send_frame_width = client_frame_width;
			send_frame_height = client_frame_height;
		}
		if (!have_log_rect) {
			log_x = send_x;
			log_y = send_y;
			log_w = send_w;
			log_h = send_h;
			log_frame_width = send_frame_width;
			log_frame_height = send_frame_height;
			log_mode = client_graphics_mode_name(client);
			have_log_rect = true;
		}
		if (full_frame)
			full_clients++;
		clients++;
		bool deferred = false;
		if (!send_graphics_update(
			    client,
			    send_buf,
			    send_frame_width,
			    send_frame_height,
			    send_x,
			    send_y,
			    send_w,
			    send_h,
			    &bytes_sent,
			    &deferred
		    )) {
			g_warning("RDP: Failed to send graphics update; closing client.");
			g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
		} else if (!deferred) {
				client->needs_full_frame = false;
		}
	}
	const int64_t elapsed = g_get_monotonic_time() - start;
	this->stats_frames_sent++;
	this->stats_bytes_sent += bytes_sent;
	this->stats_send_time_us += elapsed;
	if (clients == 0 && this->updates_sent < 3) {
		g_message("RDP: No active clients for bitmap update %ux%u.", width, height);
	} else if (clients > 0 &&
		   (this->updates_sent < 3 || elapsed > G_USEC_PER_SEC)) {
			g_message(
				"RDP: Sent %s update %ux%u damage %u,%u %ux%u to %u client(s), %u full, in %" G_GINT64_FORMAT "ms.",
				log_mode,
				log_frame_width,
				log_frame_height,
				log_x,
			log_y,
			log_w,
			log_h,
			clients,
			full_clients,
			elapsed / 1000
		);
	}
	maybe_log_stats(this, g_get_monotonic_time(), full_clients);
	this->updates_sent++;
	if (this->clients != NULL) {
		if (this->last_frame == NULL)
			this->last_frame = g_byte_array_sized_new(frame_bytes);
		g_byte_array_set_size(this->last_frame, frame_bytes);
		memcpy(this->last_frame->data, buf->data, frame_bytes);
		this->last_frame_width = width;
		this->last_frame_height = height;
	}
	g_mutex_unlock(&this->lock);
}

static void flush(RfRemoteServer *super)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	g_mutex_lock(&this->lock);
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;
		g_io_stream_close(G_IO_STREAM(client->connection), NULL, NULL);
	}
	g_mutex_unlock(&this->lock);
}

static void dispose(GObject *o)
{
	rf_remote_server_stop(RF_REMOTE_SERVER(o));

	G_OBJECT_CLASS(rf_rdp_server_parent_class)->dispose(o);
}

static void finalize(GObject *o)
{
	RfRDPServer *this = RF_RDP_SERVER(o);

	g_strfreev(this->ips);
	g_free(this->tls_private_key_file);
	g_free(this->tls_certificate_file);
	g_free(this->username);
	g_free(this->domain);
	g_free(this->password);
	g_free(this->graphics);
	g_free(this->avc_encoder);
	g_free(this->audio_codec);
	g_free(this->clipboard_socket_path);
	g_free(this->rdp_audio_socket_path);
	g_free(this->clipboard_text);
	rf_clipboard_rich_payload_clear(&this->rich_clipboard);
	g_clear_pointer(&this->clipboard_wire, g_byte_array_unref);
	if (this->audio_queue != NULL)
		g_queue_clear_full(this->audio_queue, (GDestroyNotify)audio_frame_free);
	g_clear_pointer(&this->audio_queue, g_queue_free);
	g_clear_pointer(&this->last_frame, g_byte_array_unref);
	g_clear_pointer(&this->clipboard_sockets, g_hash_table_unref);
	g_cond_clear(&this->audio_cond);
	g_mutex_clear(&this->audio_lock);
	g_mutex_clear(&this->lock);

	G_OBJECT_CLASS(rf_rdp_server_parent_class)->finalize(o);
}

static void rf_rdp_server_class_init(RfRDPServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);
	RfRemoteServerClass *r_class = RF_REMOTE_SERVER_CLASS(klass);

	o_class->dispose = dispose;
	o_class->finalize = finalize;
	r_class->start = start;
	r_class->is_running = is_running;
	r_class->stop = stop;
	r_class->set_desktop_name = set_desktop_name;
	r_class->set_rdp_clipboard_socket_path = set_rdp_clipboard_socket_path;
	r_class->set_rdp_audio_socket_path = set_rdp_audio_socket_path;
	r_class->send_clipboard_text = send_clipboard_text;
	r_class->should_render_frame = should_render_frame;
	r_class->update = update;
	r_class->flush = flush;
}

static void rf_rdp_server_init(RfRDPServer *this)
{
	g_mutex_init(&this->lock);
	g_mutex_init(&this->audio_lock);
	g_cond_init(&this->audio_cond);
	this->audio_queue = g_queue_new();
	rf_clipboard_rich_payload_init(&this->rich_clipboard);
	this->clipboard_sockets = g_hash_table_new_full(
		g_direct_hash,
		g_direct_equal,
		g_object_unref,
		(GDestroyNotify)g_source_unref
	);
	this->port = 3389;
	this->width = 1920;
	this->height = 1080;
	this->max_fps = 30;
	this->adaptive_fps = this->max_fps;
	this->next_render_time_us = -1;
	this->clipboard = true;
	this->audio_sample_rate = 48000;
	this->audio_channels = 2;
	this->audio_frame_ms = 20;
	this->audio_codec = g_strdup("auto");
	this->configured_video_quality = RF_CONFIG_RDP_VIDEO_QUALITY_AUTO;
	this->max_video_quality_level = RDP_RDPGFX_MAX_VIDEO_QUALITY_LEVEL;
	this->rdpgfx_target_bandwidth_mbps =
		RDP_RDPGFX_DEFAULT_TARGET_BANDWIDTH_MBPS;
	this->rdpgfx_target_bytes_per_second =
		(uint64_t)this->rdpgfx_target_bandwidth_mbps * 1000000ull / 8ull;
}

G_MODULE_EXPORT RfRemoteServer *rf_rdp_server_new(RfConfig *config)
{
	RfRDPServer *this = g_object_new(RF_TYPE_RDP_SERVER, NULL);

	this->config = config;
	this->ips = rf_config_get_rdp_ip_list(config);
	this->port = rf_config_get_rdp_port(config);
	this->tls_private_key_file =
		rf_config_get_rdp_tls_private_key_file(config);
	this->tls_certificate_file =
		rf_config_get_rdp_tls_certificate_file(config);
	this->username = rf_config_get_rdp_username(config);
	this->domain = rf_config_get_rdp_domain(config);
	this->password = rf_config_get_rdp_password(config);
	this->nla = rf_config_get_rdp_nla(config);
	this->graphics = rf_config_get_rdp_graphics(config);
	this->avc_encoder = rf_config_get_rdp_avc_encoder(config);
	this->clipboard = rf_config_get_rdp_clipboard(config);
	this->audio = rf_config_get_rdp_audio(config);
	this->audio_sample_rate = rf_config_get_rdp_audio_sample_rate(config);
	this->audio_channels = rf_config_get_rdp_audio_channels(config);
	this->audio_frame_ms = rf_config_get_rdp_audio_frame_ms(config);
	g_free(this->audio_codec);
	this->audio_codec = rf_config_get_rdp_audio_codec(config);
	this->max_fps = rf_config_get_fps(config);
	this->adaptive_fps = this->max_fps;
	this->configured_video_quality = rf_config_get_rdp_video_quality(config);
	this->max_video_quality_level =
		rf_config_get_rdp_video_quality_max(config);
	this->rdpgfx_target_bandwidth_mbps =
		rf_config_get_rdp_target_bandwidth_mbps(config);
	this->rdpgfx_target_bytes_per_second =
		(uint64_t)this->rdpgfx_target_bandwidth_mbps * 1000000ull / 8ull;
	if (this->configured_video_quality != RF_CONFIG_RDP_VIDEO_QUALITY_AUTO)
		this->rdpgfx_video_quality_level =
			(unsigned int)this->configured_video_quality;
	unsigned int default_width = rf_config_get_default_width(config);
	unsigned int default_height = rf_config_get_default_height(config);
	if (default_width > 0 && default_height > 0) {
		this->width = default_width;
		this->height = default_height;
	}
	if (this->nla)
		g_warning("RDP: Native NLA is not implemented yet.");
	if (this->configured_video_quality == RF_CONFIG_RDP_VIDEO_QUALITY_AUTO)
		g_message(
			"RDP: Graphics mode is %s, fps is %u, avc-encoder is %s, video-quality is auto (max %u, target %u Mbps).",
			this->graphics,
			this->max_fps,
			this->avc_encoder,
			this->max_video_quality_level,
			this->rdpgfx_target_bandwidth_mbps
		);
	else
		g_message(
			"RDP: Graphics mode is %s, fps is %u, avc-encoder is %s, video-quality is fixed %u, target is %u Mbps.",
			this->graphics,
			this->max_fps,
			this->avc_encoder,
			this->rdpgfx_video_quality_level,
			this->rdpgfx_target_bandwidth_mbps
		);
	g_message(
		"RDP: Audio is %s, preferred format is %u Hz/%u channel(s)/16-bit, frame-ms=%u, codec=%s.",
		this->audio ? "enabled" : "disabled",
		this->audio_sample_rate,
		this->audio_channels,
		this->audio_frame_ms,
		this->audio_codec
	);
	return RF_REMOTE_SERVER(this);
}
