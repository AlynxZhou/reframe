#include "rf-rdp-gfx.h"

#include <stdlib.h>
#include <string.h>

#define ZGFX_HASH_SIZE 65536u
#define ZGFX_MIN_MATCH_LENGTH 3u

static void write_u16_le(uint8_t *data, uint16_t value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = (value >> 24) & 0xff;
}

static uint16_t read_u16_le(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static bool write_header(
	uint8_t *data,
	size_t capacity,
	uint16_t command_id,
	size_t payload_length
)
{
	const size_t length = RF_RDP_GFX_HEADER_SIZE + payload_length;

	if (data == NULL || capacity < length || length > UINT32_MAX)
		return false;

	write_u16_le(data, command_id);
	write_u16_le(data + 2, 0);
	write_u32_le(data + 4, length);
	return true;
}

static bool is_supported_caps_version(uint32_t version)
{
	return version == RF_RDP_GFX_CAPVERSION_8 ||
	       version == RF_RDP_GFX_CAPVERSION_81 ||
	       version == RF_RDP_GFX_CAPVERSION_10 ||
	       version == RF_RDP_GFX_CAPVERSION_101 ||
	       version == RF_RDP_GFX_CAPVERSION_102 ||
	       version == RF_RDP_GFX_CAPVERSION_103 ||
	       version == RF_RDP_GFX_CAPVERSION_104 ||
	       version == RF_RDP_GFX_CAPVERSION_105 ||
	       version == RF_RDP_GFX_CAPVERSION_106 ||
	       version == RF_RDP_GFX_CAPVERSION_106_ERR ||
	       version == RF_RDP_GFX_CAPVERSION_107;
}

static void derive_caps_codecs(struct rf_rdp_gfx_caps *caps)
{
	if (caps->selected_version == 0)
		return;

	caps->planar = true;
	caps->remotefx = true;
	caps->progressive = true;
	caps->progressive_v2 = caps->selected_version >= RF_RDP_GFX_CAPVERSION_10;
	caps->avc444 =
		caps->selected_version >= RF_RDP_GFX_CAPVERSION_10 &&
		(caps->selected_flags & RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED) == 0;
	caps->avc444_v2 =
		caps->avc444 &&
		caps->selected_version >= RF_RDP_GFX_CAPVERSION_106;
}

static bool check_command(
	const uint8_t *data,
	size_t length,
	uint16_t command_id,
	size_t payload_length
)
{
	const size_t expected_length = RF_RDP_GFX_HEADER_SIZE + payload_length;

	if (data == NULL || length != expected_length)
		return false;
	return read_u16_le(data) == command_id &&
	       read_u32_le(data + 4) == expected_length;
}

struct zgfx_bit_writer {
	uint8_t *data;
	size_t capacity;
	size_t length;
	uint8_t current;
	uint8_t bits;
	bool failed;
};

struct zgfx_literal_token {
	uint8_t value;
	uint8_t prefix_length;
	uint16_t prefix_code;
};

struct zgfx_distance_token {
	uint8_t prefix_length;
	uint16_t prefix_code;
	uint8_t value_bits;
	uint32_t value_base;
};

static const struct zgfx_literal_token ZGFX_LITERAL_TOKENS[] = {
	{ 0x00, 5, 24 }, { 0x01, 5, 25 },
	{ 0x02, 6, 52 }, { 0x03, 6, 53 }, { 0xff, 6, 54 },
	{ 0x04, 7, 110 }, { 0x05, 7, 111 }, { 0x06, 7, 112 },
	{ 0x07, 7, 113 }, { 0x08, 7, 114 }, { 0x09, 7, 115 },
	{ 0x0a, 7, 116 }, { 0x0b, 7, 117 }, { 0x3a, 7, 118 },
	{ 0x3b, 7, 119 }, { 0x3c, 7, 120 }, { 0x3d, 7, 121 },
	{ 0x3e, 7, 122 }, { 0x3f, 7, 123 }, { 0x40, 7, 124 },
	{ 0x80, 7, 125 }, { 0x0c, 8, 252 }, { 0x38, 8, 253 },
	{ 0x39, 8, 254 }, { 0x66, 8, 255 },
};

static const struct zgfx_distance_token ZGFX_DISTANCE_TOKENS[] = {
	{ 5, 17, 5, 0 }, { 5, 18, 7, 32 }, { 5, 19, 9, 160 },
	{ 5, 20, 10, 672 }, { 5, 21, 12, 1696 },
	{ 6, 44, 14, 5792 }, { 6, 45, 15, 22176 },
	{ 7, 92, 18, 54944 }, { 7, 93, 20, 317088 },
	{ 8, 188, 20, 1365664 }, { 8, 189, 21, 2414240 },
	{ 9, 380, 22, 4511392 }, { 9, 381, 23, 8705696 },
	{ 9, 382, 24, 17094304 },
};

static bool bit_writer_has(struct zgfx_bit_writer *writer, size_t length)
{
	return !writer->failed && writer->length <= writer->capacity &&
	       writer->capacity - writer->length >= length;
}

static void bit_writer_emit_byte(struct zgfx_bit_writer *writer, uint8_t value)
{
	if (!bit_writer_has(writer, 1)) {
		writer->failed = true;
		return;
	}
	writer->data[writer->length++] = value;
}

static void bit_writer_write(
	struct zgfx_bit_writer *writer,
	uint32_t value,
	unsigned int bits
)
{
	for (unsigned int i = 0; i < bits; ++i) {
		const unsigned int shift = bits - i - 1;

		writer->current = (writer->current << 1) |
			((value >> shift) & 1);
		writer->bits++;
		if (writer->bits == 8) {
			bit_writer_emit_byte(writer, writer->current);
			writer->current = 0;
			writer->bits = 0;
		}
	}
}

static size_t bit_writer_finish(struct zgfx_bit_writer *writer)
{
	uint8_t unused_bits = 0;

	if (writer->bits != 0) {
		unused_bits = 8 - writer->bits;
		bit_writer_emit_byte(writer, writer->current << unused_bits);
	}
	bit_writer_emit_byte(writer, unused_bits);
	return writer->failed ? 0 : writer->length;
}

static uint32_t zgfx_hash3(const uint8_t *data)
{
	return ((uint32_t)data[0] * 251u ^
		(uint32_t)data[1] * 199u ^
		(uint32_t)data[2] * 131u) & (ZGFX_HASH_SIZE - 1);
}

static void zgfx_insert_hashes(
	const uint8_t *data,
	size_t length,
	int32_t *hash,
	size_t start,
	size_t end
)
{
	for (size_t i = start; i < end && i + 2 < length; ++i)
		hash[zgfx_hash3(data + i)] = (int32_t)i;
}

static bool zgfx_distance_token_for(
	size_t distance,
	const struct zgfx_distance_token **token
)
{
	if (distance == 0 || distance > UINT32_MAX)
		return false;
	for (size_t i = 0;
	     i < sizeof(ZGFX_DISTANCE_TOKENS) / sizeof(ZGFX_DISTANCE_TOKENS[0]);
	     ++i) {
		const struct zgfx_distance_token *candidate =
			&ZGFX_DISTANCE_TOKENS[i];
		const uint32_t max_value =
			candidate->value_base + ((1u << candidate->value_bits) - 1);

		if (distance >= candidate->value_base && distance <= max_value) {
			*token = candidate;
			return true;
		}
	}
	return false;
}

static size_t zgfx_match_length(
	const uint8_t *data,
	size_t length,
	size_t position,
	size_t distance
)
{
	size_t match = 0;

	if (distance == 0 || distance > position)
		return 0;
	while (position + match < length &&
	       data[position + match] == data[position + match - distance])
		match++;
	return match;
}

static void zgfx_best_match(
	const uint8_t *data,
	size_t length,
	const int32_t *hash,
	size_t position,
	size_t *best_distance,
	size_t *best_length
)
{
	*best_distance = 0;
	*best_length = 0;

	if (position + ZGFX_MIN_MATCH_LENGTH > length)
		return;

	const size_t distances[] = { 1, 4 };
	for (size_t i = 0; i < sizeof(distances) / sizeof(distances[0]); ++i) {
		const size_t distance = distances[i];
		const size_t match = zgfx_match_length(
			data,
			length,
			position,
			distance
		);

		if (match > *best_length) {
			*best_distance = distance;
			*best_length = match;
		}
	}

	const int32_t previous = hash[zgfx_hash3(data + position)];
	if (previous >= 0 && (size_t)previous < position) {
		const size_t distance = position - (size_t)previous;
		const size_t match = zgfx_match_length(
			data,
			length,
			position,
			distance
		);

		if (match > *best_length) {
			*best_distance = distance;
			*best_length = match;
		}
	}

	const struct zgfx_distance_token *token = NULL;
	if (*best_length < ZGFX_MIN_MATCH_LENGTH ||
	    !zgfx_distance_token_for(*best_distance, &token)) {
		*best_distance = 0;
		*best_length = 0;
	}
}

static void zgfx_write_literal(struct zgfx_bit_writer *writer, uint8_t value)
{
	for (size_t i = 0;
	     i < sizeof(ZGFX_LITERAL_TOKENS) / sizeof(ZGFX_LITERAL_TOKENS[0]);
	     ++i) {
		if (ZGFX_LITERAL_TOKENS[i].value == value) {
			bit_writer_write(
				writer,
				ZGFX_LITERAL_TOKENS[i].prefix_code,
				ZGFX_LITERAL_TOKENS[i].prefix_length
			);
			return;
		}
	}

	bit_writer_write(writer, 0, 1);
	bit_writer_write(writer, value, 8);
}

static void zgfx_write_count(struct zgfx_bit_writer *writer, size_t count)
{
	if (count == 3) {
		bit_writer_write(writer, 0, 1);
		return;
	}

	size_t base = 4;
	unsigned int extra = 2;
	bit_writer_write(writer, 1, 1);
	while (count >= base * 2) {
		bit_writer_write(writer, 1, 1);
		base *= 2;
		extra++;
	}
	bit_writer_write(writer, 0, 1);
	bit_writer_write(writer, (uint32_t)(count - base), extra);
}

static bool zgfx_write_match(
	struct zgfx_bit_writer *writer,
	size_t distance,
	size_t count
)
{
	const struct zgfx_distance_token *token = NULL;

	if (!zgfx_distance_token_for(distance, &token))
		return false;

	bit_writer_write(writer, token->prefix_code, token->prefix_length);
	bit_writer_write(writer, (uint32_t)(distance - token->value_base), token->value_bits);
	zgfx_write_count(writer, count);
	return !writer->failed;
}

static size_t rf_rdp_gfx_write_zgfx_compressed_segment(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (data == NULL || payload == NULL || payload_length == 0 ||
	    payload_length > RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE || capacity < 2)
		return 0;

	int32_t *hash = malloc(sizeof(*hash) * ZGFX_HASH_SIZE);
	if (hash == NULL)
		return 0;
	for (size_t i = 0; i < ZGFX_HASH_SIZE; ++i)
		hash[i] = -1;

	data[0] = RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8 |
		  RF_RDP_GFX_ZGFX_PACKET_COMPRESSED;
	struct zgfx_bit_writer writer = {
		data + 1,
		capacity - 1,
		0,
		0,
		0,
		false
	};

	size_t position = 0;
	while (position < payload_length && !writer.failed) {
		size_t distance = 0;
		size_t match = 0;

		zgfx_best_match(
			payload,
			payload_length,
			hash,
			position,
			&distance,
			&match
		);
		if (match >= ZGFX_MIN_MATCH_LENGTH) {
			if (!zgfx_write_match(&writer, distance, match))
				break;
			zgfx_insert_hashes(
				payload,
				payload_length,
				hash,
				position,
				position + match
			);
			position += match;
		} else {
			zgfx_write_literal(&writer, payload[position]);
			zgfx_insert_hashes(
				payload,
				payload_length,
				hash,
				position,
				position + 1
			);
			position++;
		}
	}

	free(hash);
	const size_t bitstream_length = bit_writer_finish(&writer);
	if (bitstream_length == 0)
		return 0;
	return 1 + bitstream_length;
}

static size_t zgfx_segment_count(size_t payload_length)
{
	return (payload_length + RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE - 1) /
	       RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE;
}

static size_t zgfx_uncompressed_capacity(size_t payload_length)
{
	if (payload_length > UINT32_MAX)
		return 0;
	if (payload_length <= RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE)
		return payload_length + 2;

	const size_t segment_count = zgfx_segment_count(payload_length);
	if (segment_count > UINT16_MAX ||
	    segment_count > (SIZE_MAX - 7 - payload_length) / 5)
		return 0;
	return 7 + payload_length + segment_count * 5;
}

bool rf_rdp_gfx_parse_caps_advertise(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_gfx_caps *caps
)
{
	if (data == NULL || caps == NULL || length < RF_RDP_GFX_HEADER_SIZE + 2)
		return false;

	const uint16_t command_id = read_u16_le(data);
	const uint32_t pdu_length = read_u32_le(data + 4);

	if (command_id != RF_RDP_GFX_CMDID_CAPSADVERTISE ||
	    pdu_length != length)
		return false;

	memset(caps, 0, sizeof(*caps));
	size_t offset = RF_RDP_GFX_HEADER_SIZE;
	caps->count = read_u16_le(data + offset);
	offset += 2;

	for (uint16_t i = 0; i < caps->count; ++i) {
		if (offset + 8 > length)
			return false;

		const uint32_t version = read_u32_le(data + offset);
		const uint32_t caps_length = read_u32_le(data + offset + 4);
		offset += 8;

		if (offset + caps_length > length)
			return false;

		uint32_t flags = 0;
		if (caps_length >= 4)
			flags = read_u32_le(data + offset);

		if (is_supported_caps_version(version) &&
		    version >= RF_RDP_GFX_CAPVERSION_81 &&
		    (flags & RF_RDP_GFX_CAPS_FLAG_AVC420_ENABLED) != 0)
			caps->avc420 = true;
		if (version == RF_RDP_GFX_CAPVERSION_FRDP_1) {
			caps->av1 = true;
			caps->av1_i444 =
				(flags & RF_RDP_GFX_CAPS_FLAG_AV1_I444_SUPPORTED) != 0 &&
				(flags & RF_RDP_GFX_CAPS_FLAG_AV1_I444_DISABLED) == 0;
		}

		if (is_supported_caps_version(version) &&
		    version >= caps->selected_version) {
			caps->selected_version = version;
			caps->selected_flags = flags;
		}
		offset += caps_length;
	}

	derive_caps_codecs(caps);
	return caps->selected_version != 0;
}

enum rf_rdp_gfx_codec rf_rdp_gfx_select_codec(
	const struct rf_rdp_gfx_caps *caps,
	const struct rf_rdp_gfx_server_codecs *server,
	bool prefer_avc444
)
{
	if (caps == NULL || server == NULL)
		return RF_RDP_GFX_CODEC_UNCOMPRESSED;

	if (server->av1 && caps->av1)
		return RF_RDP_GFX_CODEC_AV1;
	if (prefer_avc444 && server->avc444 && caps->avc444)
		return caps->avc444_v2 ?
			RF_RDP_GFX_CODEC_AVC444_V2 :
			RF_RDP_GFX_CODEC_AVC444;
	if (server->avc420 && caps->avc420)
		return RF_RDP_GFX_CODEC_AVC420;
	if (server->avc444 && caps->avc444)
		return caps->avc444_v2 ?
			RF_RDP_GFX_CODEC_AVC444_V2 :
			RF_RDP_GFX_CODEC_AVC444;
	if (server->progressive && caps->progressive)
		return caps->progressive_v2 ?
			RF_RDP_GFX_CODEC_PROGRESSIVE_V2 :
			RF_RDP_GFX_CODEC_PROGRESSIVE;
	if (server->remotefx && caps->remotefx)
		return RF_RDP_GFX_CODEC_REMOTEFX;
	if (server->planar && caps->planar)
		return RF_RDP_GFX_CODEC_PLANAR;
	return RF_RDP_GFX_CODEC_UNCOMPRESSED;
}

const char *rf_rdp_gfx_codec_name(enum rf_rdp_gfx_codec codec)
{
	switch (codec) {
	case RF_RDP_GFX_CODEC_AV1:
		return "AV1";
	case RF_RDP_GFX_CODEC_AVC444_V2:
		return "AVC444 v2";
	case RF_RDP_GFX_CODEC_AVC444:
		return "AVC444";
	case RF_RDP_GFX_CODEC_AVC420:
		return "AVC420";
	case RF_RDP_GFX_CODEC_PROGRESSIVE_V2:
		return "Progressive v2";
	case RF_RDP_GFX_CODEC_PROGRESSIVE:
		return "Progressive";
	case RF_RDP_GFX_CODEC_REMOTEFX:
		return "RemoteFX";
	case RF_RDP_GFX_CODEC_PLANAR:
		return "PLANAR";
	case RF_RDP_GFX_CODEC_UNCOMPRESSED:
	default:
		return "uncompressed";
	}
}

bool rf_rdp_gfx_parse_frame_acknowledge(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_gfx_frame_ack *ack
)
{
	if (ack == NULL ||
	    !check_command(
		    data,
		    length,
		    RF_RDP_GFX_CMDID_FRAMEACKNOWLEDGE,
		    12
	    ))
		return false;

	ack->queue_depth = read_u32_le(data + 8);
	ack->frame_id = read_u32_le(data + 12);
	ack->total_frames_decoded = read_u32_le(data + 16);
	return true;
}

bool rf_rdp_gfx_parse_qoe_frame_acknowledge(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_gfx_qoe_frame_ack *ack
)
{
	if (ack == NULL ||
	    !check_command(
		    data,
		    length,
		    RF_RDP_GFX_CMDID_QOEFRAMEACKNOWLEDGE,
		    12
	    ))
		return false;

	ack->frame_id = read_u32_le(data + 8);
	ack->timestamp = read_u32_le(data + 12);
	ack->time_diff_se = read_u16_le(data + 16);
	ack->time_diff_edr = read_u16_le(data + 18);
	return true;
}

size_t rf_rdp_gfx_write_caps_confirm(
	uint8_t *data,
	size_t capacity,
	uint32_t version,
	uint32_t flags
)
{
	const size_t payload_length = 12;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_GFX_CMDID_CAPSCONFIRM,
		    payload_length
	    ))
		return 0;

	write_u32_le(data + 8, version);
	write_u32_le(data + 12, 4);
	write_u32_le(data + 16, flags);
	return RF_RDP_GFX_HEADER_SIZE + payload_length;
}

size_t rf_rdp_gfx_write_create_surface(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint16_t width,
	uint16_t height,
	uint8_t pixel_format
)
{
	const size_t payload_length = 7;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_GFX_CMDID_CREATESURFACE,
		    payload_length
	    ))
		return 0;

	write_u16_le(data + 8, surface_id);
	write_u16_le(data + 10, width);
	write_u16_le(data + 12, height);
	data[14] = pixel_format;
	return RF_RDP_GFX_HEADER_SIZE + payload_length;
}

size_t rf_rdp_gfx_write_map_surface_to_output(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint32_t output_origin_x,
	uint32_t output_origin_y
)
{
	const size_t payload_length = 12;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_GFX_CMDID_MAPSURFACETOOUTPUT,
		    payload_length
	    ))
		return 0;

	write_u16_le(data + 8, surface_id);
	write_u16_le(data + 10, 0);
	write_u32_le(data + 12, output_origin_x);
	write_u32_le(data + 16, output_origin_y);
	return RF_RDP_GFX_HEADER_SIZE + payload_length;
}

size_t rf_rdp_gfx_write_reset_graphics(
	uint8_t *data,
	size_t capacity,
	uint32_t width,
	uint32_t height
)
{
	const size_t length = 340;

	if (data == NULL || capacity < length || width == 0 || height == 0)
		return 0;
	memset(data, 0, length);
	if (!write_header(data, capacity, RF_RDP_GFX_CMDID_RESETGRAPHICS, length - 8))
		return 0;

	write_u32_le(data + 8, width);
	write_u32_le(data + 12, height);
	write_u32_le(data + 16, 1);
	write_u32_le(data + 20, 0);
	write_u32_le(data + 24, 0);
	write_u32_le(data + 28, width - 1);
	write_u32_le(data + 32, height - 1);
	write_u32_le(data + 36, 1);
	return length;
}

size_t rf_rdp_gfx_write_start_frame(
	uint8_t *data,
	size_t capacity,
	uint32_t frame_id,
	uint32_t timestamp
)
{
	const size_t payload_length = 8;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_GFX_CMDID_STARTFRAME,
		    payload_length
	    ))
		return 0;

	write_u32_le(data + 8, timestamp);
	write_u32_le(data + 12, frame_id);
	return RF_RDP_GFX_HEADER_SIZE + payload_length;
}

size_t rf_rdp_gfx_write_end_frame(
	uint8_t *data,
	size_t capacity,
	uint32_t frame_id
)
{
	const size_t payload_length = 4;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_GFX_CMDID_ENDFRAME,
		    payload_length
	    ))
		return 0;

	write_u32_le(data + 8, frame_id);
	return RF_RDP_GFX_HEADER_SIZE + payload_length;
}

size_t rf_rdp_gfx_write_wire_to_surface_1(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint16_t codec_id,
	uint8_t pixel_format,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	const uint8_t *bitmap_data,
	size_t bitmap_data_length
)
{
	const size_t payload_length = 17 + bitmap_data_length;

	if ((bitmap_data == NULL && bitmap_data_length > 0) ||
	    bitmap_data_length > UINT32_MAX ||
	    !write_header(
		    data,
		    capacity,
		    RF_RDP_GFX_CMDID_WIRETOSURFACE_1,
		    payload_length
	    ))
		return 0;

	write_u16_le(data + 8, surface_id);
	write_u16_le(data + 10, codec_id);
	data[12] = pixel_format;
	write_u16_le(data + 13, left);
	write_u16_le(data + 15, top);
	write_u16_le(data + 17, right);
	write_u16_le(data + 19, bottom);
	write_u32_le(data + 21, (uint32_t)bitmap_data_length);
	if (bitmap_data_length > 0)
		memcpy(data + 25, bitmap_data, bitmap_data_length);
	return RF_RDP_GFX_HEADER_SIZE + payload_length;
}

size_t rf_rdp_gfx_write_avc420_bitmap_stream(
	uint8_t *data,
	size_t capacity,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	uint8_t qp,
	uint8_t quality,
	const uint8_t *h264_data,
	size_t h264_data_length
)
{
	const size_t metadata_length = 4 + 8 + 2;
	const size_t length = metadata_length + h264_data_length;

	if (data == NULL || h264_data == NULL || h264_data_length == 0 ||
	    h264_data_length > UINT32_MAX || capacity < length ||
	    left >= right || top >= bottom || qp > 63)
		return 0;

	write_u32_le(data, 1);
	write_u16_le(data + 4, left);
	write_u16_le(data + 6, top);
	write_u16_le(data + 8, right);
	write_u16_le(data + 10, bottom);
	data[12] = qp & 0x3f;
	data[13] = quality;
	memcpy(data + metadata_length, h264_data, h264_data_length);
	return length;
}

size_t rf_rdp_gfx_write_avc444_bitmap_stream(
	uint8_t *data,
	size_t capacity,
	uint8_t lc,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	uint8_t qp,
	uint8_t quality,
	const uint8_t *h264_data,
	size_t h264_data_length
)
{
	if (data == NULL || capacity < 4 || lc == RF_RDP_GFX_AVC444_LC_BOTH ||
	    lc > RF_RDP_GFX_AVC444_LC_CHROMA)
		return 0;

	const size_t avc420_length = rf_rdp_gfx_write_avc420_bitmap_stream(
		data + 4,
		capacity - 4,
		left,
		top,
		right,
		bottom,
		qp,
		quality,
		h264_data,
		h264_data_length
	);
	if (avc420_length == 0 || avc420_length > 0x3fffffffu)
		return 0;

	write_u32_le(data, (uint32_t)avc420_length | ((uint32_t)lc << 30));
	return 4 + avc420_length;
}

size_t rf_rdp_gfx_write_avc444_bitmap_stream_pair(
	uint8_t *data,
	size_t capacity,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	uint8_t qp,
	uint8_t quality,
	const uint8_t *first_h264_data,
	size_t first_h264_data_length,
	const uint8_t *second_h264_data,
	size_t second_h264_data_length
)
{
	if (data == NULL || capacity < 4)
		return 0;

	const size_t first_length = rf_rdp_gfx_write_avc420_bitmap_stream(
		data + 4,
		capacity - 4,
		left,
		top,
		right,
		bottom,
		qp,
		quality,
		first_h264_data,
		first_h264_data_length
	);
	if (first_length == 0 || first_length > 0x3fffffffu)
		return 0;
	if (first_length > capacity - 4)
		return 0;

	const size_t second_offset = 4 + first_length;
	const size_t second_length = rf_rdp_gfx_write_avc420_bitmap_stream(
		data + second_offset,
		capacity - second_offset,
		left,
		top,
		right,
		bottom,
		qp,
		quality,
		second_h264_data,
		second_h264_data_length
	);
	if (second_length == 0)
		return 0;

	write_u32_le(
		data,
		(uint32_t)first_length |
			((uint32_t)RF_RDP_GFX_AVC444_LC_BOTH << 30)
	);
	return second_offset + second_length;
}

size_t rf_rdp_gfx_write_zgfx_uncompressed(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (data == NULL || (payload == NULL && payload_length > 0) ||
	    payload_length > UINT32_MAX)
		return 0;

	if (payload_length <= RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE) {
		if (capacity < payload_length + 2)
			return 0;
		data[0] = RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE;
		data[1] = RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8;
		if (payload_length > 0)
			memcpy(data + 2, payload, payload_length);
		return payload_length + 2;
	}

	const size_t segment_count =
		(payload_length + RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE - 1) /
		RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE;
	if (segment_count > UINT16_MAX ||
	    segment_count > (SIZE_MAX - 7 - payload_length) / 5)
		return 0;

	const size_t length = 7 + payload_length + segment_count * 5;
	if (capacity < length)
		return 0;

	data[0] = RF_RDP_GFX_ZGFX_SEGMENTED_MULTIPART;
	write_u16_le(data + 1, (uint16_t)segment_count);
	write_u32_le(data + 3, (uint32_t)payload_length);

	size_t out = 7;
	size_t in = 0;
	while (in < payload_length) {
		const size_t remaining = payload_length - in;
		const size_t segment_length = remaining >
				RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE ?
			RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE :
			remaining;

		write_u32_le(data + out, (uint32_t)(segment_length + 1));
		out += 4;
		data[out++] = RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8;
		memcpy(data + out, payload + in, segment_length);
		out += segment_length;
		in += segment_length;
	}

	return out;
}

size_t rf_rdp_gfx_write_zgfx(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length,
	bool *compressed
)
{
	if (compressed != NULL)
		*compressed = false;
	if (data == NULL || (payload == NULL && payload_length > 0))
		return 0;

	const size_t raw_capacity = zgfx_uncompressed_capacity(payload_length);
	if (raw_capacity == 0 || capacity < raw_capacity)
		return 0;

	if (payload_length == 0)
		return rf_rdp_gfx_write_zgfx_uncompressed(
			data,
			capacity,
			payload,
			payload_length
		);

	if (payload_length <= RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE) {
		data[0] = RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE;
		const size_t segment_length =
			rf_rdp_gfx_write_zgfx_compressed_segment(
				data + 1,
				raw_capacity - 1,
				payload,
				payload_length
			);
		if (segment_length > 0 && 1 + segment_length < raw_capacity) {
			if (compressed != NULL)
				*compressed = true;
			return 1 + segment_length;
		}
		return rf_rdp_gfx_write_zgfx_uncompressed(
			data,
			capacity,
			payload,
			payload_length
		);
	}

	const size_t segment_count = zgfx_segment_count(payload_length);
	if (segment_count > UINT16_MAX)
		return 0;

	data[0] = RF_RDP_GFX_ZGFX_SEGMENTED_MULTIPART;
	write_u16_le(data + 1, (uint16_t)segment_count);
	write_u32_le(data + 3, (uint32_t)payload_length);

	size_t out = 7;
	size_t in = 0;
	bool any_compressed = false;
	while (in < payload_length) {
		const size_t remaining = payload_length - in;
		const size_t segment_payload_length = remaining >
				RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE ?
			RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE :
			remaining;
		const size_t raw_segment_length = segment_payload_length + 1;
		const size_t size_offset = out;
		size_t segment_length = 0;

		out += 4;
		segment_length = rf_rdp_gfx_write_zgfx_compressed_segment(
			data + out,
			raw_segment_length,
			payload + in,
			segment_payload_length
		);
		if (segment_length > 0 && segment_length < raw_segment_length) {
			any_compressed = true;
		} else {
			segment_length = raw_segment_length;
			data[out] = RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8;
			memcpy(data + out + 1, payload + in, segment_payload_length);
		}

		write_u32_le(data + size_offset, (uint32_t)segment_length);
		out += segment_length;
		in += segment_payload_length;
	}

	if (compressed != NULL)
		*compressed = any_compressed;
	return out;
}
