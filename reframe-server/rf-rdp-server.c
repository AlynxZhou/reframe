#include <string.h>

#include <gio/gio.h>

#include "rf-rdp-av1.h"
#include "rf-rdp-avc.h"
#include "rf-rdp-core.h"
#include "rf-rdp-dvc.h"
#include "rf-rdp-gfx.h"
#include "rf-rdp-input.h"
#include "rf-rdp-mcs.h"
#include "rf-rdp-nsc.h"
#include "rf-rdp-planar.h"
#include "rf-rdp-proto.h"
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

static unsigned int align16(unsigned int value)
{
	return (value + 15u) & ~15u;
}

static const char *yes_no(bool value)
{
	return value ? "yes" : "no";
}

struct _RfRDPServer {
	RfRemoteServer parent_instance;
	RfConfig *config;
	GSocketService *service;
	GTlsCertificate *certificate;
	GMutex lock;
	GList *clients;
	char **ips;
	char *tls_private_key_file;
	char *tls_certificate_file;
	char *username;
	char *domain;
	char *password;
	char *graphics;
	char *avc_encoder;
	char *clipboard_text;
	GByteArray *last_frame;
	unsigned int port;
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
	unsigned int stats_min_target_fps;
	unsigned int rdpgfx_video_quality_level;
	int configured_video_quality;
	unsigned int max_video_quality_level;
	unsigned int rdpgfx_target_bandwidth_mbps;
	uint64_t rdpgfx_target_bytes_per_second;
	uint32_t stats_max_rdpgfx_inflight;
	uint32_t stats_max_rdpgfx_ack_queue_depth;
	bool nla;
	bool clipboard;
	bool running;
};
G_DEFINE_TYPE(RfRDPServer, rf_rdp_server, RF_TYPE_REMOTE_SERVER)

struct client {
	RfRDPServer *server;
	GSocketConnection *connection;
	GIOStream *stream;
	GThread *thread;
	GMutex write_lock;
	uint32_t selected_protocol;
	uint32_t rdpgfx_channel_id;
	uint16_t desktop_width;
	uint16_t desktop_height;
	uint16_t channel_count;
	uint16_t drdynvc_channel_id;
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
	RfRdpNscContext *nsc;
	RfRdpAv1Encoder *av1;
	RfRdpAvcEncoder *avc;
	RfRdpAvcEncoder *avc_chroma;
	int64_t av1_bit_rate;
	int64_t avc_bit_rate;
	unsigned int av1_gop_size;
	unsigned int avc_gop_size;
	uint8_t av1_qp;
	uint8_t avc_qp;
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
	bool drdynvc_capability_sent;
	bool drdynvc_ready;
	bool rdpgfx_create_sent;
	bool rdpgfx_channel_open;
	bool rdpgfx_caps_confirmed;
	bool rdpgfx_surface_ready;
	bool rdpgfx_disabled;
	bool rdpgfx_av1_available;
	bool rdpgfx_avc420_available;
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

static struct rf_rdp_gfx_server_codecs rdpgfx_server_codecs(void)
{
	return (struct rf_rdp_gfx_server_codecs){
	#ifdef RF_HAVE_RDP_AVC
		.av1 = true,
		.avc420 = true,
		.avc444 = true,
	#endif
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
	client->av1_qp = 0;
	client->avc_width = 0;
	client->avc_height = 0;
	client->avc_bit_rate = 0;
	client->avc_gop_size = 0;
	client->avc_qp = 0;
	client->avc444_signature_x = 0;
	client->avc444_signature_y = 0;
	client->avc444_signature_width = 0;
	client->avc444_signature_height = 0;
	client->avc444_luma_signature = 0;
	client->avc444_chroma_signature = 0;
	client->avc444_have_signature = false;
}

static void client_free(struct client *client)
{
	if (client == NULL)
		return;

	g_clear_object(&client->stream);
	g_clear_object(&client->connection);
	client_reset_avc(client);
	rf_rdp_nsc_context_free(client->nsc);
	g_mutex_clear(&client->write_lock);
	g_free(client);
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

	width = requested_width;
	height = requested_height;
	if (width > 0xffff)
		width = 0xffff;
	if (height > 0xffff)
		height = 0xffff;

	client->desktop_width = width;
	client->desktop_height = height;
	g_mutex_lock(&this->lock);
	this->width = width;
	this->height = height;
	g_mutex_unlock(&this->lock);
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

static void remove_client(struct client *client)
{
	RfRDPServer *this = client->server;
	bool last_client = false;

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
	g_mutex_unlock(&this->lock);

	if (last_client)
		rf_remote_server_handle_last_client(RF_REMOTE_SERVER(this));
}

static void add_client(struct client *client)
{
	RfRDPServer *this = client->server;
	bool first_client = false;

	g_mutex_lock(&this->lock);
	if (!client->counted) {
		first_client = this->clients == NULL;
		this->clients = g_list_prepend(this->clients, client);
		client->counted = true;
		client->needs_full_frame = true;
		if (first_client) {
			this->updates_sent = 0;
			this->next_render_time_us = -1;
			this->stats_last_log_time_us = 0;
			this->stats_frames_sent = 0;
				this->stats_frames_skipped = 0;
				this->stats_bytes_sent = 0;
				this->stats_send_time_us = 0;
				this->stats_min_target_fps = 0;
				this->stats_max_rdpgfx_inflight = 0;
				this->stats_max_rdpgfx_ack_queue_depth = 0;
			} else if (this->last_frame != NULL &&
			 this->last_frame_width == client->desktop_width &&
			 this->last_frame_height == client->desktop_height) {
			bool deferred = false;

			if (send_graphics_update(
				    client,
				    this->last_frame,
				    this->last_frame_width,
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
	}
	g_mutex_unlock(&this->lock);

	if (first_client)
		rf_remote_server_handle_first_client(RF_REMOTE_SERVER(this));
	if (client->desktop_width > 0 && client->desktop_height > 0)
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
	unsigned int current_width = 0;
	unsigned int current_height = 0;

	g_mutex_lock(&this->lock);
	current_width = this->width;
	current_height = this->height;
	g_mutex_unlock(&this->lock);

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
	const size_t channel_length = rf_rdp_dvc_write_channel_pdu(
		channel,
		sizeof(channel),
		payload,
		payload_length
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
	size_t *bytes_sent
)
{
	const size_t zgfx_capacity = rdpgfx_zgfx_capacity(payload_length);
	if (zgfx_capacity == 0)
		return false;

	g_autofree uint8_t *zgfx = g_malloc(zgfx_capacity);
	const size_t zgfx_length = rf_rdp_gfx_write_zgfx_uncompressed(
		zgfx,
		zgfx_capacity,
		payload,
		payload_length
	);

	return zgfx_length > 0 &&
	       send_rdpgfx_dvc_payload(client, zgfx, zgfx_length, bytes_sent);
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

static bool send_rdpgfx_caps_confirm(
	struct client *client,
	const struct rf_rdp_gfx_caps *caps
)
{
	RfRDPServer *this = client->server;
	const struct rf_rdp_gfx_server_codecs codecs = rdpgfx_server_codecs();
	const bool prefer_avc444 = rf_rdp_core_should_use_avc444(
		caps->avc444,
		caps->avc420,
		this->rdpgfx_video_quality_level
	);
	const enum rf_rdp_gfx_codec policy_codec =
		rf_rdp_gfx_select_codec(caps, &codecs, prefer_avc444);
	const bool use_av1 = policy_codec == RF_RDP_GFX_CODEC_AV1;
	const uint32_t confirm_version = use_av1 ?
		RF_RDP_GFX_CAPVERSION_FRDP_1 :
		caps->selected_version;
	const uint32_t confirm_flags = use_av1 ?
		((caps->av1_flags & ~RF_RDP_GFX_CAPS_FLAG_AV1_I444_SUPPORTED) |
		 RF_RDP_GFX_CAPS_FLAG_AV1_I444_DISABLED) :
		caps->selected_flags;
	struct rf_rdp_gfx_caps confirmed_caps = *caps;
	uint8_t gfx[64] = { 0 };
	const size_t length = rf_rdp_gfx_write_caps_confirm(
		gfx,
		sizeof(gfx),
		confirm_version,
		confirm_flags
	);

	if (length == 0 || !send_rdpgfx_gfx_payload(client, gfx, length, NULL))
		return false;

	client->rdpgfx_caps_confirmed = true;
	client->rdpgfx_caps_version = confirm_version;
	client->rdpgfx_caps_flags = confirm_flags;
	client->rdpgfx_av1_available = use_av1;
	client->rdpgfx_avc420_available = caps->avc420;
	confirmed_caps.selected_version = confirm_version;
	confirmed_caps.selected_flags = confirm_flags;
	log_rdpgfx_codec_policy(&confirmed_caps, &codecs, policy_codec);
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
	       client->rdpgfx_caps_version != RF_RDP_GFX_CAPVERSION_FRDP_1 &&
	       client->rdpgfx_avc420_available &&
	       (client->rdpgfx_caps_flags & RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED) == 0;
}

static bool client_can_use_rdpgfx_av1(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       client->rdpgfx_av1_available &&
	       client->rdpgfx_caps_version == RF_RDP_GFX_CAPVERSION_FRDP_1;
}

static bool client_can_use_rdpgfx_avc444(const struct client *client)
{
	return client_can_use_rdpgfx(client) &&
	       client->rdpgfx_caps_version != RF_RDP_GFX_CAPVERSION_FRDP_1 &&
	       client->rdpgfx_caps_version >= RF_RDP_GFX_CAPVERSION_10 &&
	       (client->rdpgfx_caps_flags & RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED) == 0;
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
			client->drdynvc_channel_id = client_info.drdynvc_channel_id;
			g_message("RDP: Client desktop is %ux%u with %u static channels.",
				client->desktop_width,
				client->desktop_height,
				client->channel_count);
			if (client->drdynvc_channel_id != 0)
				g_message("RDP: Client advertised drdynvc static channel %u.",
					client->drdynvc_channel_id);
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

static void handle_active_session(struct client *client)
{
	RfRDPServer *this = client->server;
	GInputStream *input = g_io_stream_get_input_stream(client->stream);

	g_message("RDP: Client reached active state.");
	start_drdynvc(client);
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
	if (length == 0 || !send_rdpgfx_gfx_payload(client, gfx, length, bytes_sent))
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
	if (length == 0 || !send_rdpgfx_gfx_payload(client, gfx, length, bytes_sent))
		return false;

	memset(gfx, 0, sizeof(gfx));
	length = rf_rdp_gfx_write_map_surface_to_output(
		gfx,
		sizeof(gfx),
		RDP_RDPGFX_SURFACE_ID,
		0,
		0
	);
	if (length == 0 || !send_rdpgfx_gfx_payload(client, gfx, length, bytes_sent))
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
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int frame_height = client->server->height;
	const unsigned int quality_level =
		client->server->rdpgfx_video_quality_level;
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
	    client->av1_width != coded_width ||
	    client->av1_height != coded_height ||
	    client->av1_bit_rate != bit_rate ||
	    client->av1_gop_size != gop_size || client->av1_qp != qp) {
		client_reset_avc(client);
		client->av1 = rf_rdp_av1_encoder_new_with_rate(
			(uint16_t)coded_width,
			(uint16_t)coded_height,
			MAX(client->server->adaptive_fps, 1),
			bit_rate,
			qp,
			gop_size,
			client->server->avc_encoder
		);
		client->av1_width = client->av1 != NULL ? coded_width : 0;
		client->av1_height = client->av1 != NULL ? coded_height : 0;
		client->av1_bit_rate = client->av1 != NULL ? bit_rate : 0;
		client->av1_gop_size = client->av1 != NULL ? gop_size : 0;
		client->av1_qp = client->av1 != NULL ? qp : 0;
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

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent))
		return false;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX AV1 updates active using %s, rect %u,%u %ux%u, coded %ux%u, bitrate=%" G_GINT64_FORMAT ", qp=%u, gop=%u.",
			rf_rdp_av1_encoder_name(client->av1),
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
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int frame_height = client->server->height;
	const unsigned int quality_level =
		client->server->rdpgfx_video_quality_level;
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

	if (!rf_rdp_core_should_use_avc444(
		    client_can_use_rdpgfx_avc444(client),
		    client_can_use_rdpgfx_avc420(client),
		    quality_level
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

	bit_rate = rf_rdp_core_rdpgfx_avc_bit_rate(
		coded_width,
		coded_height,
		MAX(client->server->adaptive_fps, 1),
		quality_level,
		true
	);
	qp = rf_rdp_core_rdpgfx_avc_qp(quality_level, true);
	quality = rf_rdp_core_rdpgfx_avc_quality(quality_level, true);
	gop_size = rf_rdp_core_rdpgfx_avc_gop_size(
		MAX(client->server->adaptive_fps, 1),
		quality_level,
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
	    client->avc_gop_size != gop_size || client->avc_qp != qp) {
		client_reset_avc(client);
		client->avc = rf_rdp_avc_encoder_new_with_rate(
			(uint16_t)coded_width,
			(uint16_t)coded_height,
			MAX(client->server->adaptive_fps, 1),
			bit_rate,
			qp,
			gop_size,
			client->server->avc_encoder
		);
		if (client->avc == NULL) {
			client_reset_avc(client);
			return false;
		}
		client->avc_width = coded_width;
		client->avc_height = coded_height;
		client->avc_bit_rate = bit_rate;
		client->avc_gop_size = gop_size;
		client->avc_qp = qp;
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
		quality_level
	);
	if (full_avc444 || skip_avc444_delta) {
		encode_luma = true;
		encode_chroma = true;
	} else if (!rf_rdp_avc_compare_avc444_rect(
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
			   &encode_chroma
		   )) {
		encode_luma = true;
		encode_chroma = true;
		full_avc444 = true;
	}
	if (!encode_luma && !encode_chroma)
		return true;

	if (encode_luma && encode_chroma) {
		uint8_t encoded_lc = 0xff;

		if (!rf_rdp_avc_encoder_encode_avc444_rgba(
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
		    )) {
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
	} else if (!rf_rdp_avc_encoder_encode_avc444_chroma_rgba(
			   client->avc,
			   rgba,
			   rgba_available_length,
			   rgba_stride,
			   false,
			   &chroma_h264,
			   &chroma_h264_length
		   )) {
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
		RF_RDP_GFX_CODECID_AVC444,
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

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent))
		return false;
	if (rf_rdp_core_rdpgfx_avc444_lc_index(lc, &lc_index))
		client->server->stats_avc444_lc[lc_index]++;

	if (!client->rdpgfx_update_logged) {
		g_message(
			"RDP: RDPGFX AVC444 updates active using %s, rect %u,%u %ux%u, coded %ux%u, lc=%u, bitrate=%" G_GINT64_FORMAT ", qp=%u, gop=%u.",
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
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *bytes_sent
)
{
	const unsigned int frame_height = client->server->height;
	const unsigned int quality_level =
		client->server->rdpgfx_video_quality_level;
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

	if (client->avc == NULL || client->avc_chroma != NULL ||
	    client->avc_width != coded_width ||
	    client->avc_height != coded_height ||
	    client->avc_bit_rate != bit_rate ||
	    client->avc_gop_size != gop_size || client->avc_qp != qp) {
		client_reset_avc(client);
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

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent))
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

static bool send_rdpgfx_update(
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
	    frame_width == 0 || right > UINT16_MAX || bottom > UINT16_MAX)
		return false;
	if (uncompressed_length == 0 ||
	    uncompressed_length / 4 / height != width)
		return false;

	if (send_rdpgfx_av1_update(
		    client,
		    buf,
		    frame_width,
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
			"RDP: Client did not enable AVC420 (caps flags=0x%08x); using PLANAR RDPGFX.",
			client->rdpgfx_caps_flags
		);
		client->rdpgfx_avc_client_disabled_logged = true;
	}

	if (!client->rdpgfx_surface_ready ||
	    client->rdpgfx_surface_width != frame_width ||
	    client->rdpgfx_surface_height != client->server->height) {
		if (frame_width > UINT16_MAX || client->server->height > UINT16_MAX)
			return false;
		if (!send_rdpgfx_surface_setup(
			    client,
			    frame_width,
			    client->server->height,
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

	if (!send_rdpgfx_gfx_payload(client, gfx, offset, bytes_sent))
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
	client_free(client);
	return NULL;
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

	client->server = this;
	client->rdpgfx_channel_id = RDP_RDPGFX_DVC_CHANNEL_ID;
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
	g_socket_service_start(this->service);

	this->running = true;
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

		if (!client_can_use_rdpgfx(client) &&
		    client->graphics_mode == RF_RDP_GRAPHICS_MODE_BITMAP)
			limited_clients++;
	}
	return limited_clients;
}

static unsigned int rdpgfx_video_client_count(RfRDPServer *this)
{
	unsigned int video_clients = 0;

	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client_can_use_rdpgfx(client))
			video_clients++;
	}
	return video_clients;
}

static void reset_rdpgfx_video_encoders(RfRDPServer *this)
{
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;

		if (client_can_use_rdpgfx(client)) {
			client_reset_avc(client);
			client->rdpgfx_update_logged = false;
		}
	}
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
	const unsigned int previous_fps = this->adaptive_fps;
	const unsigned int previous_video_quality =
		this->rdpgfx_video_quality_level;
	const unsigned int video_quality_target_fps =
		this->stats_min_target_fps != 0 ?
			this->stats_min_target_fps :
			(this->adaptive_fps != 0 ? this->adaptive_fps : this->max_fps);

	this->adaptive_fps = rf_rdp_core_update_adaptive_fps(
		this->adaptive_fps,
		this->max_fps,
		RDP_BITMAP_MIN_FPS,
		this->stats_bytes_sent,
		interval_us,
		RDP_BITMAP_TARGET_BYTES_PER_SECOND,
		avg_send_time_us,
		limited_clients > 0
	);
	if (this->configured_video_quality == RF_CONFIG_RDP_VIDEO_QUALITY_AUTO) {
		this->rdpgfx_video_quality_level =
			rf_rdp_core_update_video_quality_level(
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
				video_clients > 0
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
		reset_rdpgfx_video_encoders(this);
		g_message(
			"RDP: RDPGFX video quality level changed from %u to %u.",
			previous_video_quality,
			this->rdpgfx_video_quality_level
		);
	}
	g_message(
		"RDP: Stats max-fps=%u, effective-fps=%u, target-fps-min=%u, video-quality=%u, target-bandwidth=%uMbps, sent=%" G_GUINT64_FORMAT ", skipped=%" G_GUINT64_FORMAT ", bytes=%" G_GUINT64_FORMAT ", limited-clients=%u, rdpgfx-clients=%u, full-frame-clients=%u, rdpgfx-inflight-max=%u, ack-depth-max=%u, avg-send=%" G_GUINT64_FORMAT "ms, avc444-lc=%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " (both/single/chroma).",
		this->max_fps,
		this->adaptive_fps,
		this->stats_min_target_fps,
		this->rdpgfx_video_quality_level,
		this->rdpgfx_target_bandwidth_mbps,
		this->stats_frames_sent,
		this->stats_frames_skipped,
		this->stats_bytes_sent,
		limited_clients,
		video_clients,
		full_clients,
		this->stats_max_rdpgfx_inflight,
		this->stats_max_rdpgfx_ack_queue_depth,
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
	rf_remote_server_flush(super);
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

static void send_clipboard_text(RfRemoteServer *super, const char *text)
{
	RfRDPServer *this = RF_RDP_SERVER(super);

	if (!this->clipboard)
		return;
	g_mutex_lock(&this->lock);
	g_free(this->clipboard_text);
	this->clipboard_text = g_strdup(text);
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
	const int64_t start = g_get_monotonic_time();
	unsigned int clients = 0;
	for (GList *l = this->clients; l != NULL; l = l->next) {
		struct client *client = l->data;
		const bool full_frame = client->needs_full_frame ||
			client_needs_rdpgfx_surface(client, width, height);

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
		if (!have_log_rect) {
			log_x = x;
			log_y = y;
				log_w = w;
				log_h = h;
				log_mode = client_graphics_mode_name(client);
				have_log_rect = true;
			}
		if (full_frame)
			full_clients++;
		clients++;
		bool deferred = false;
		if (!send_graphics_update(
			    client,
			    buf,
			    width,
			    x,
			    y,
			    w,
			    h,
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
				width,
				height,
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
	g_free(this->clipboard_text);
	g_clear_pointer(&this->last_frame, g_byte_array_unref);
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
	r_class->send_clipboard_text = send_clipboard_text;
	r_class->should_render_frame = should_render_frame;
	r_class->update = update;
	r_class->flush = flush;
}

static void rf_rdp_server_init(RfRDPServer *this)
{
	g_mutex_init(&this->lock);
	this->port = 3389;
	this->width = 1920;
	this->height = 1080;
	this->max_fps = 30;
	this->adaptive_fps = this->max_fps;
	this->next_render_time_us = -1;
	this->clipboard = true;
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
	this->max_fps = rf_config_get_rdp_max_fps(config);
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
			"RDP: Graphics mode is %s, max-fps is %u, avc-encoder is %s, video-quality is auto (max %u, target %u Mbps).",
			this->graphics,
			this->max_fps,
			this->avc_encoder,
			this->max_video_quality_level,
			this->rdpgfx_target_bandwidth_mbps
		);
	else
		g_message(
			"RDP: Graphics mode is %s, max-fps is %u, avc-encoder is %s, video-quality is fixed %u, target is %u Mbps.",
			this->graphics,
			this->max_fps,
			this->avc_encoder,
			this->rdpgfx_video_quality_level,
			this->rdpgfx_target_bandwidth_mbps
		);
	return RF_REMOTE_SERVER(this);
}
