#include <stdlib.h>
#include <string.h>

#include "rf-rdp-core.h"

struct rf_rect {
	int x;
	int y;
	unsigned int w;
	unsigned int h;
};

#define RF_RDP_SEC_KNOWN_MASK \
	(RF_RDP_SEC_EXCHANGE_PKT | RF_RDP_SEC_INFO_PKT | RF_RDP_SEC_LICENSE_PKT)

#define LICENSE_ERROR_ALERT 0xff
#define LICENSE_PREAMBLE_VERSION_3_0 0x03
#define LICENSE_STATUS_VALID_CLIENT 0x00000007u
#define LICENSE_ST_NO_TRANSITION 0x00000002u
#define LICENSE_BB_ERROR_BLOB 0x0004u

#define CAPSET_TYPE_GENERAL 0x0001
#define CAPSET_TYPE_BITMAP 0x0002
#define CAPSET_TYPE_ORDER 0x0003
#define CAPSET_TYPE_POINTER 0x0008
#define CAPSET_TYPE_SHARE 0x0009
#define CAPSET_TYPE_INPUT 0x000d
#define CAPSET_TYPE_FONT 0x000e
#define CAPSET_TYPE_VIRTUAL_CHANNEL 0x0014
#define CAPSET_TYPE_MULTI_FRAGMENT_UPDATE 0x001a
#define CAPSET_TYPE_LARGE_POINTER 0x001b
#define CAPSET_TYPE_SURFACE_COMMANDS 0x001c
#define CAPSET_TYPE_FRAME_ACKNOWLEDGE 0x001e

#define TS_CAPS_PROTOCOLVERSION 0x0200
#define INPUT_FLAG_SCANCODES 0x0001
#define FONTSUPPORT_FONTLIST 0x0001
#define SURFCMDS_SET_SURFACE_BITS 0x00000002u

#define STREAM_LOW 0x01
#define FASTPATH_MAX_SINGLE_PACKET_SIZE 0x3fffu
#define PTR_MSG_TYPE_SYSTEM 0x0001u
#define SYSPTR_DEFAULT 0x00007f00u

static const uint8_t CODEC_GUID_NSCODEC[] = {
	0xb9, 0x1b, 0x8d, 0xca,
	0x0f, 0x00,
	0x4f, 0x15,
	0x58, 0x9f, 0xae, 0x2d, 0x1a, 0x87, 0xe2, 0xd6
};

static const uint8_t CODEC_GUID_REMOTEFX[] = {
	0x12, 0x2f, 0x77, 0x76,
	0x72, 0xbd,
	0x63, 0x44,
	0xaf, 0xb3, 0xb7, 0x3c, 0x9c, 0x6f, 0x78, 0x86
};

struct writer {
	uint8_t *data;
	size_t capacity;
	size_t length;
	bool failed;
};

static bool writer_has(const struct writer *writer, size_t length)
{
	return !writer->failed && writer->length <= writer->capacity &&
	       writer->capacity - writer->length >= length;
}

static void write_u8(struct writer *writer, uint8_t value)
{
	if (!writer_has(writer, 1)) {
		writer->failed = true;
		return;
	}
	writer->data[writer->length++] = value;
}

static void write_u16_le(struct writer *writer, uint16_t value)
{
	write_u8(writer, value & 0xff);
	write_u8(writer, value >> 8);
}

static void write_u32_le(struct writer *writer, uint32_t value)
{
	write_u8(writer, value & 0xff);
	write_u8(writer, (value >> 8) & 0xff);
	write_u8(writer, (value >> 16) & 0xff);
	write_u8(writer, (value >> 24) & 0xff);
}

static void write_bytes(
	struct writer *writer,
	const uint8_t *data,
	size_t length
)
{
	if (!writer_has(writer, length)) {
		writer->failed = true;
		return;
	}
	if (length > 0)
		memcpy(writer->data + writer->length, data, length);
	writer->length += length;
}

static uint16_t read_u16_le(const uint8_t *data)
{
	return data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
	return data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void patch_u16_le(uint8_t *data, size_t offset, uint16_t value)
{
	data[offset] = value & 0xff;
	data[offset + 1] = value >> 8;
}

static size_t capability_start(struct writer *writer)
{
	const size_t offset = writer->length;

	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	return offset;
}

static bool capability_finish(
	struct writer *writer,
	size_t offset,
	uint16_t type
)
{
	const size_t length = writer->length - offset;

	if (length > 0xffff || writer->failed)
		return false;
	patch_u16_le(writer->data, offset, type);
	patch_u16_le(writer->data, offset + 2, length);
	return true;
}

static bool write_general_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u16_le(writer, 4);
	write_u16_le(writer, 0);
	write_u16_le(writer, TS_CAPS_PROTOCOLVERSION);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u8(writer, 0);
	write_u8(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_GENERAL);
}

static bool write_bitmap_capability(
	struct writer *writer,
	uint16_t width,
	uint16_t height
)
{
	const size_t header = capability_start(writer);

	write_u16_le(writer, 32);
	write_u16_le(writer, 1);
	write_u16_le(writer, 1);
	write_u16_le(writer, 1);
	write_u16_le(writer, width);
	write_u16_le(writer, height);
	write_u16_le(writer, 0);
	write_u16_le(writer, 1);
	write_u16_le(writer, 1);
	write_u8(writer, 0);
	write_u8(writer, 0);
	write_u16_le(writer, 1);
	write_u16_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_BITMAP);
}

static bool write_order_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);
	const uint8_t zero[32] = { 0 };

	write_bytes(writer, zero, 16);
	write_u32_le(writer, 0);
	write_u16_le(writer, 1);
	write_u16_le(writer, 20);
	write_u16_le(writer, 0);
	write_u16_le(writer, 1);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_bytes(writer, zero, sizeof(zero));
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u32_le(writer, 0);
	write_u32_le(writer, 230400);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_ORDER);
}

static bool write_pointer_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u16_le(writer, 1);
	write_u16_le(writer, 20);
	write_u16_le(writer, 20);
	return capability_finish(writer, header, CAPSET_TYPE_POINTER);
}

static bool write_input_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);
	const uint8_t ime_file_name[64] = { 0 };

	write_u16_le(writer, INPUT_FLAG_SCANCODES);
	write_u16_le(writer, 0);
	write_u32_le(writer, 0x00000409);
	write_u32_le(writer, 4);
	write_u32_le(writer, 0);
	write_u32_le(writer, 12);
	write_bytes(writer, ime_file_name, sizeof(ime_file_name));
	return capability_finish(writer, header, CAPSET_TYPE_INPUT);
}

static bool write_virtual_channel_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u32_le(writer, 0);
	write_u32_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_VIRTUAL_CHANNEL);
}

static bool write_share_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u16_le(writer, 0x03ea);
	write_u16_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_SHARE);
}

static bool write_font_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u16_le(writer, FONTSUPPORT_FONTLIST);
	write_u16_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_FONT);
}

static bool write_multifragment_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u32_le(writer, 0x003f80);
	return capability_finish(writer, header, CAPSET_TYPE_MULTI_FRAGMENT_UPDATE);
}

static bool write_large_pointer_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u16_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_LARGE_POINTER);
}

static bool write_surface_commands_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u32_le(writer, SURFCMDS_SET_SURFACE_BITS);
	write_u32_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_SURFACE_COMMANDS);
}

static bool write_bitmap_codecs_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u8(writer, 1);
	write_bytes(writer, CODEC_GUID_NSCODEC, sizeof(CODEC_GUID_NSCODEC));
	write_u8(writer, 0);
	write_u16_le(writer, 4);
	write_u32_le(writer, 0);
	return capability_finish(writer, header, RF_RDP_CAPSET_TYPE_BITMAP_CODECS);
}

static bool write_frame_ack_capability(struct writer *writer)
{
	const size_t header = capability_start(writer);

	write_u32_le(writer, 0);
	return capability_finish(writer, header, CAPSET_TYPE_FRAME_ACKNOWLEDGE);
}

static bool write_demand_active_payload(
	struct writer *writer,
	uint32_t share_id,
	uint16_t width,
	uint16_t height
)
{
	const uint8_t source_descriptor[] = { 'R', 'D', 'P', '\0' };
	const size_t combined_length_offset = 6;
	size_t caps_start = 0;
	size_t capability_count_offset = 0;
	uint16_t capability_count = 0;
	size_t caps_end = 0;
	size_t combined_length = 0;

	write_u32_le(writer, share_id);
	write_u16_le(writer, sizeof(source_descriptor));
	write_u16_le(writer, 0);
	write_bytes(writer, source_descriptor, sizeof(source_descriptor));
	caps_start = writer->length;
	capability_count_offset = writer->length;
	write_u16_le(writer, 0);
	write_u16_le(writer, 0);

	if (!write_general_capability(writer))
		return false;
	capability_count++;
	if (!write_bitmap_capability(writer, width, height))
		return false;
	capability_count++;
	if (!write_order_capability(writer))
		return false;
	capability_count++;
	if (!write_pointer_capability(writer))
		return false;
	capability_count++;
	if (!write_input_capability(writer))
		return false;
	capability_count++;
	if (!write_virtual_channel_capability(writer))
		return false;
	capability_count++;
	if (!write_share_capability(writer))
		return false;
	capability_count++;
	if (!write_font_capability(writer))
		return false;
	capability_count++;
	if (!write_multifragment_capability(writer))
		return false;
	capability_count++;
	if (!write_large_pointer_capability(writer))
		return false;
	capability_count++;
	if (!write_surface_commands_capability(writer))
		return false;
	capability_count++;
	if (!write_bitmap_codecs_capability(writer))
		return false;
	capability_count++;
	if (!write_frame_ack_capability(writer))
		return false;
	capability_count++;

	caps_end = writer->length;
	combined_length = caps_end - caps_start;
	if (combined_length > 0xffff || writer->failed)
		return false;
	patch_u16_le(writer->data, combined_length_offset, combined_length);
	patch_u16_le(writer->data, capability_count_offset, capability_count);
	write_u32_le(writer, 0);
	return !writer->failed;
}

bool rf_rdp_core_parse_client_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_pdu *pdu
)
{
	struct rf_rdp_mcs_domain_pdu mcs_pdu = { 0 };
	const uint8_t *payload = NULL;
	size_t payload_length = 0;

	if (data == NULL || pdu == NULL ||
	    !rf_rdp_mcs_parse_domain_pdu(data, length, &mcs_pdu) ||
	    mcs_pdu.type != RF_RDP_MCS_PDU_SEND_DATA_REQUEST)
		return false;
	if (mcs_pdu.payload_offset > length ||
	    mcs_pdu.payload_offset + mcs_pdu.payload_length > length)
		return false;

	memset(pdu, 0, sizeof(*pdu));
	pdu->channel_id = mcs_pdu.channel_id;
	pdu->payload_offset = mcs_pdu.payload_offset;
	pdu->payload_length = mcs_pdu.payload_length;
	payload = data + mcs_pdu.payload_offset;
	payload_length = mcs_pdu.payload_length;

	if (payload_length >= 4) {
		const uint16_t flags = read_u16_le(payload);
		const uint16_t flags_hi = read_u16_le(payload + 2);

		if (flags_hi == 0 && (flags & RF_RDP_SEC_KNOWN_MASK) != 0 &&
		    (flags & ~RF_RDP_SEC_KNOWN_MASK) == 0) {
			pdu->security_flags = flags;
			pdu->payload_offset += 4;
			pdu->payload_length -= 4;
			return true;
		}
	}

	if (payload_length < 6)
		return false;
	const uint16_t total_length = read_u16_le(payload);
	if (total_length < 6 || total_length > payload_length)
		return false;
	pdu->share_type = read_u16_le(payload + 2) & 0x0f;
	pdu->share_source = read_u16_le(payload + 4);
	pdu->payload_offset += 6;
	pdu->payload_length = total_length - 6;
	if (pdu->share_type == RF_RDP_PDU_TYPE_DATA) {
		const uint8_t *share_data = data + pdu->payload_offset;
		const size_t share_data_length = pdu->payload_length;
		uint16_t uncompressed_length = 0;

		if (share_data_length < 12)
			return false;
		uncompressed_length = read_u16_le(share_data + 6);
		if (uncompressed_length > share_data_length - 12)
			return false;
		pdu->data_type = share_data[8];
		pdu->payload_offset += 12;
		pdu->payload_length = uncompressed_length;
	}
	return true;
}

bool rf_rdp_core_parse_synchronize_body(
	const uint8_t *data,
	size_t length,
	uint16_t *target_user
)
{
	if (data == NULL || length < 4)
		return false;
	if (read_u16_le(data) != RF_RDP_SYNC_MESSAGE_TYPE_SYNC)
		return false;
	if (target_user != NULL)
		*target_user = read_u16_le(data + 2);
	return true;
}

bool rf_rdp_core_parse_control_body(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_control_pdu *control
)
{
	if (data == NULL || control == NULL || length < 8)
		return false;

	control->action = read_u16_le(data);
	control->grant_id = read_u16_le(data + 2);
	control->control_id = read_u32_le(data + 4);
	return true;
}

bool rf_rdp_core_parse_font_list_body(
	const uint8_t *data,
	size_t length,
	uint16_t *flags
)
{
	if (data == NULL || length < 8)
		return false;
	if (flags != NULL)
		*flags = read_u16_le(data + 4);
	return true;
}

static bool parse_surface_commands_capability(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_capabilities *caps
)
{
	if (length < 8)
		return false;

	const uint32_t commands = read_u32_le(data);
	caps->surface_set_bits =
		(commands & RF_RDP_SURFCMDS_SET_SURFACE_BITS) != 0;
	caps->surface_stream_bits =
		(commands & RF_RDP_SURFCMDS_STREAM_SURFACE_BITS) != 0;
	return true;
}

static bool parse_bitmap_codecs_capability(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_capabilities *caps
)
{
	size_t offset = 0;

	if (length < 1)
		return false;

	uint8_t codec_count = data[offset++];
	while (codec_count-- > 0) {
		if (length - offset < 19)
			return false;

		const uint8_t *guid = data + offset;
		const uint8_t codec_id = data[offset + 16];
		const uint16_t properties_length = read_u16_le(data + offset + 17);
		offset += 19;
		if (properties_length > length - offset)
			return false;

		if (memcmp(guid, CODEC_GUID_NSCODEC, sizeof(CODEC_GUID_NSCODEC)) == 0) {
			caps->nscodec = true;
			caps->nscodec_id = codec_id;
		} else if (memcmp(
				   guid,
				   CODEC_GUID_REMOTEFX,
				   sizeof(CODEC_GUID_REMOTEFX)
			   ) == 0) {
			caps->remotefx = true;
			caps->remotefx_id = codec_id;
		}
		offset += properties_length;
	}

	return true;
}

bool rf_rdp_core_parse_confirm_active_capabilities(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_capabilities *caps
)
{
	if (data == NULL || caps == NULL || length < 14)
		return false;

	memset(caps, 0, sizeof(*caps));

	const uint16_t source_descriptor_length = read_u16_le(data + 6);
	const uint16_t combined_capabilities_length = read_u16_le(data + 8);
	if (source_descriptor_length > length - 10)
		return false;

	const size_t caps_start = 10 + source_descriptor_length;
	if (combined_capabilities_length < 4 ||
	    combined_capabilities_length > length - caps_start)
		return false;

	const size_t caps_end = caps_start + combined_capabilities_length;
	const uint16_t capability_count = read_u16_le(data + caps_start);
	size_t offset = caps_start + 4;

	for (uint16_t i = 0; i < capability_count; ++i) {
		if (caps_end - offset < 4)
			return false;

		const uint16_t type = read_u16_le(data + offset);
		const uint16_t cap_length = read_u16_le(data + offset + 2);
		if (cap_length < 4 || cap_length > caps_end - offset)
			return false;

		const uint8_t *cap_data = data + offset + 4;
		const size_t cap_data_length = cap_length - 4;
		if (type == RF_RDP_CAPSET_TYPE_SURFACE_COMMANDS) {
			if (!parse_surface_commands_capability(
				    cap_data,
				    cap_data_length,
				    caps
			    ))
				return false;
		} else if (type == RF_RDP_CAPSET_TYPE_BITMAP_CODECS) {
			if (!parse_bitmap_codecs_capability(
				    cap_data,
				    cap_data_length,
				    caps
			    ))
				return false;
		}
		offset += cap_length;
	}

	return true;
}

bool rf_rdp_core_convert_rgba_rect_to_bgrx_bottom_up(
	uint8_t *dst,
	size_t dst_length,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	const size_t row_bytes = (size_t)width * 4;
	const size_t dst_needed = row_bytes * height;
	const size_t src_x_offset = (size_t)x * 4;

	if (dst == NULL || rgba == NULL || width == 0 || height == 0 ||
	    dst_length < dst_needed || rgba_stride < src_x_offset + row_bytes)
		return false;
	if (height > (SIZE_MAX / rgba_stride) || y > SIZE_MAX / rgba_stride)
		return false;
	const size_t last_row = (size_t)y + height - 1;
	if (last_row > SIZE_MAX / rgba_stride)
		return false;
	if (last_row * rgba_stride + src_x_offset + row_bytes > rgba_length)
		return false;

	for (uint16_t row = 0; row < height; ++row) {
		const size_t src_row = (size_t)y + height - 1 - row;
		const uint8_t *src = rgba + src_row * rgba_stride + src_x_offset;
		uint8_t *out = dst + (size_t)row * row_bytes;

		for (uint16_t col = 0; col < width; ++col) {
			out[col * 4] = src[col * 4 + 2];
			out[col * 4 + 1] = src[col * 4 + 1];
			out[col * 4 + 2] = src[col * 4];
			out[col * 4 + 3] = 0xff;
		}
	}

	return true;
}

bool rf_rdp_core_convert_rgba_rect_to_bgrx_top_down(
	uint8_t *dst,
	size_t dst_length,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	const size_t row_bytes = (size_t)width * 4;
	const size_t dst_needed = row_bytes * height;
	const size_t src_x_offset = (size_t)x * 4;

	if (dst == NULL || rgba == NULL || width == 0 || height == 0 ||
	    dst_length < dst_needed || rgba_stride < src_x_offset + row_bytes)
		return false;
	if (height > (SIZE_MAX / rgba_stride) || y > SIZE_MAX / rgba_stride)
		return false;
	const size_t last_row = (size_t)y + height - 1;
	if (last_row > SIZE_MAX / rgba_stride)
		return false;
	if (last_row * rgba_stride + src_x_offset + row_bytes > rgba_length)
		return false;

	for (uint16_t row = 0; row < height; ++row) {
		const uint8_t *src =
			rgba + ((size_t)y + row) * rgba_stride + src_x_offset;
		uint8_t *out = dst + (size_t)row * row_bytes;

		for (uint16_t col = 0; col < width; ++col) {
			out[col * 4] = src[col * 4 + 2];
			out[col * 4 + 1] = src[col * 4 + 1];
			out[col * 4 + 2] = src[col * 4];
			out[col * 4 + 3] = 0xff;
		}
	}

	return true;
}

bool rf_rdp_core_convert_rgba_rect_to_bgr_bottom_up(
	uint8_t *dst,
	size_t dst_length,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	const size_t row_bytes = (size_t)width * 3;
	const size_t src_row_bytes = (size_t)width * 4;
	const size_t dst_needed = row_bytes * height;
	const size_t src_x_offset = (size_t)x * 4;

	if (dst == NULL || rgba == NULL || width == 0 || height == 0 ||
	    dst_length < dst_needed || rgba_stride < src_x_offset + src_row_bytes)
		return false;
	if (height > (SIZE_MAX / rgba_stride) || y > SIZE_MAX / rgba_stride)
		return false;
	const size_t last_row = (size_t)y + height - 1;
	if (last_row > SIZE_MAX / rgba_stride)
		return false;
	if (last_row * rgba_stride + src_x_offset + src_row_bytes > rgba_length)
		return false;

	for (uint16_t row = 0; row < height; ++row) {
		const size_t src_row = (size_t)y + height - 1 - row;
		const uint8_t *src = rgba + src_row * rgba_stride + src_x_offset;
		uint8_t *out = dst + (size_t)row * row_bytes;

		for (uint16_t col = 0; col < width; ++col) {
			out[col * 3] = src[col * 4 + 2];
			out[col * 3 + 1] = src[col * 4 + 1];
			out[col * 3 + 2] = src[col * 4];
		}
	}

	return true;
}

static const uint8_t *bgr24_pixel_at(
	const uint8_t *bgr,
	size_t bgr_stride,
	uint16_t width,
	size_t index
)
{
	const size_t row = index / width;
	const size_t col = index % width;

	return bgr + row * bgr_stride + col * 3;
}

static bool bgr24_pixels_equal(const uint8_t *left, const uint8_t *right)
{
	return left[0] == right[0] && left[1] == right[1] && left[2] == right[2];
}

static void write_rle24_color_run(
	struct writer *writer,
	const uint8_t *pixel,
	uint8_t count
)
{
	write_u8(writer, 0x60 | count);
	write_u8(writer, pixel[0]);
	write_u8(writer, pixel[1]);
	write_u8(writer, pixel[2]);
}

static void write_rle24_color_image(
	struct writer *writer,
	const uint8_t *bgr,
	size_t bgr_stride,
	uint16_t width,
	size_t start,
	uint8_t count
)
{
	write_u8(writer, 0x80 | count);
	for (uint8_t i = 0; i < count; ++i) {
		const uint8_t *pixel = bgr24_pixel_at(bgr, bgr_stride, width, start + i);

		write_u8(writer, pixel[0]);
		write_u8(writer, pixel[1]);
		write_u8(writer, pixel[2]);
	}
}

static size_t bgr24_run_length(
	const uint8_t *bgr,
	size_t bgr_stride,
	uint16_t width,
	size_t total_pixels,
	size_t index
)
{
	const uint8_t *pixel = bgr24_pixel_at(bgr, bgr_stride, width, index);
	size_t length = 1;

	while (index + length < total_pixels && length < 31) {
		const uint8_t *next = bgr24_pixel_at(
			bgr,
			bgr_stride,
			width,
			index + length
		);
		if (!bgr24_pixels_equal(pixel, next))
			break;
		length++;
	}
	return length;
}

size_t rf_rdp_core_compress_bgr24_bitmap(
	uint8_t *dst,
	size_t dst_capacity,
	const uint8_t *bgr,
	uint16_t width,
	uint16_t height,
	size_t bgr_stride
)
{
	const size_t row_bytes = (size_t)width * 3;
	struct writer writer = { dst, dst_capacity, 0, false };
	size_t total_pixels = 0;
	size_t index = 0;

	if (dst == NULL || bgr == NULL || width == 0 || height == 0 ||
	    bgr_stride < row_bytes)
		return 0;
	if ((size_t)width > SIZE_MAX / height)
		return 0;
	total_pixels = (size_t)width * height;

	while (index < total_pixels) {
		const size_t run_length = bgr24_run_length(
			bgr,
			bgr_stride,
			width,
			total_pixels,
			index
		);

		if (run_length >= 2) {
			write_rle24_color_run(
				&writer,
				bgr24_pixel_at(bgr, bgr_stride, width, index),
				(uint8_t)run_length
			);
			index += run_length;
			continue;
		}

		size_t literal_length = 1;
		while (index + literal_length < total_pixels && literal_length < 31) {
			const size_t next_run = bgr24_run_length(
				bgr,
				bgr_stride,
				width,
				total_pixels,
				index + literal_length
			);
			if (next_run >= 2)
				break;
			literal_length++;
		}
		write_rle24_color_image(
			&writer,
			bgr,
			bgr_stride,
			width,
			index,
			(uint8_t)literal_length
		);
		index += literal_length;
	}

	if (writer.failed)
		return 0;
	return writer.length;
}

bool rf_rdp_core_clip_update_rect(
	unsigned int frame_width,
	unsigned int frame_height,
	const struct rf_rect *damage,
	bool full_frame,
	uint16_t *x,
	uint16_t *y,
	uint16_t *width,
	uint16_t *height
)
{
	int64_t x0 = 0;
	int64_t y0 = 0;
	int64_t x1 = frame_width;
	int64_t y1 = frame_height;

	if (x == NULL || y == NULL || width == NULL || height == NULL)
		return false;
	if (frame_width == 0 || frame_height == 0 ||
	    frame_width > 0xffff || frame_height > 0xffff)
		return false;

	if (!full_frame && damage != NULL) {
		x0 = damage->x;
		y0 = damage->y;
		x1 = (int64_t)damage->x + damage->w;
		y1 = (int64_t)damage->y + damage->h;
	}

	if (x0 < 0)
		x0 = 0;
	if (y0 < 0)
		y0 = 0;
	if (x1 > (int64_t)frame_width)
		x1 = frame_width;
	if (y1 > (int64_t)frame_height)
		y1 = frame_height;
	if (x1 <= x0 || y1 <= y0)
		return false;

	*x = x0;
	*y = y0;
	*width = x1 - x0;
	*height = y1 - y0;
	return true;
}

bool rf_rdp_core_get_rgba_rect_source(
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *offset,
	size_t *available_length
)
{
	if (offset == NULL || available_length == NULL)
		return false;
	*offset = 0;
	*available_length = 0;
	if (width == 0 || height == 0 || (size_t)width > SIZE_MAX / 4)
		return false;

	const size_t row_bytes = (size_t)width * 4;
	const size_t x_offset = (size_t)x * 4;

	if (x_offset > SIZE_MAX - row_bytes ||
	    rgba_stride < x_offset + row_bytes)
		return false;
	if ((size_t)y > SIZE_MAX / rgba_stride ||
	    (size_t)height - 1 > SIZE_MAX - y)
		return false;

	const size_t first_row = (size_t)y;
	const size_t last_row = first_row + height - 1;
	if (last_row > SIZE_MAX / rgba_stride)
		return false;
	const size_t rect_offset = first_row * rgba_stride + x_offset;
	const size_t last_byte = last_row * rgba_stride + x_offset + row_bytes;
	if (last_byte < rect_offset || last_byte > rgba_length)
		return false;

	*offset = rect_offset;
	*available_length = rgba_length - rect_offset;
	return true;
}

bool rf_rdp_core_expand_update_rect(
	unsigned int frame_width,
	unsigned int frame_height,
	unsigned int min_width,
	unsigned int min_height,
	uint16_t *x,
	uint16_t *y,
	uint16_t *width,
	uint16_t *height
)
{
	if (x == NULL || y == NULL || width == NULL || height == NULL ||
	    frame_width == 0 || frame_height == 0 || *width == 0 || *height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX)
		return false;
	if ((unsigned int)*x + *width > frame_width ||
	    (unsigned int)*y + *height > frame_height)
		return false;

	unsigned int target_width = *width;
	unsigned int target_height = *height;
	if (target_width < min_width)
		target_width = min_width;
	if (target_height < min_height)
		target_height = min_height;
	if (target_width > frame_width)
		target_width = frame_width;
	if (target_height > frame_height)
		target_height = frame_height;

	int new_x = (int)*x - (int)(target_width - *width) / 2;
	int new_y = (int)*y - (int)(target_height - *height) / 2;
	const int max_x = (int)(frame_width - target_width);
	const int max_y = (int)(frame_height - target_height);
	if (new_x < 0)
		new_x = 0;
	if (new_y < 0)
		new_y = 0;
	if (new_x > max_x)
		new_x = max_x;
	if (new_y > max_y)
		new_y = max_y;

	*x = (uint16_t)new_x;
	*y = (uint16_t)new_y;
	*width = (uint16_t)target_width;
	*height = (uint16_t)target_height;
	return true;
}

bool rf_rdp_core_make_full_surface_rect(
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t *x,
	uint16_t *y,
	uint16_t *width,
	uint16_t *height
)
{
	if (x == NULL || y == NULL || width == NULL || height == NULL ||
	    frame_width == 0 || frame_height == 0 ||
	    frame_width > UINT16_MAX || frame_height > UINT16_MAX)
		return false;

	*x = 0;
	*y = 0;
	*width = (uint16_t)frame_width;
	*height = (uint16_t)frame_height;
	return true;
}

static int64_t frame_interval_us(unsigned int max_fps)
{
	if (max_fps == 0)
		return 0;

	const int64_t interval = 1000000 / max_fps;
	return interval > 0 ? interval : 1;
}

bool rf_rdp_core_frame_should_render(
	int64_t last_frame_time_us,
	int64_t now_us,
	unsigned int max_fps,
	bool needs_full_frame
)
{
	if (needs_full_frame || last_frame_time_us < 0 || max_fps == 0)
		return true;
	if (now_us <= last_frame_time_us)
		return false;
	const int64_t min_interval_us = frame_interval_us(max_fps);

	return now_us - last_frame_time_us >= min_interval_us;
}

bool rf_rdp_core_frame_scheduler_should_render(
	int64_t *next_frame_time_us,
	int64_t now_us,
	unsigned int max_fps,
	bool needs_full_frame
)
{
	if (next_frame_time_us == NULL)
		return false;
	if (max_fps == 0) {
		*next_frame_time_us = now_us;
		return true;
	}

	const int64_t interval = frame_interval_us(max_fps);
	if (needs_full_frame || *next_frame_time_us < 0) {
		*next_frame_time_us = now_us + interval;
		return true;
	}
	if (now_us < *next_frame_time_us) {
		const int64_t early_allowance = interval / 2;

		if (*next_frame_time_us - now_us > early_allowance)
			return false;
		if (*next_frame_time_us > INT64_MAX - interval)
			*next_frame_time_us = now_us + interval;
		else
			*next_frame_time_us += interval;
		return true;
	}

	const int64_t intervals = ((now_us - *next_frame_time_us) / interval) + 1;
	if (intervals > (INT64_MAX - *next_frame_time_us) / interval) {
		*next_frame_time_us = now_us + interval;
		return true;
	}
	*next_frame_time_us += intervals * interval;
	if (*next_frame_time_us <= now_us)
		*next_frame_time_us = now_us + interval;
	return true;
}

bool rf_rdp_core_should_defer_graphics_for_rdpgfx(
	bool drdynvc_advertised,
	bool rdpgfx_disabled,
	bool rdpgfx_caps_confirmed
)
{
	return drdynvc_advertised && !rdpgfx_disabled && !rdpgfx_caps_confirmed;
}

unsigned int rf_rdp_core_update_adaptive_fps(
	unsigned int current_fps,
	unsigned int configured_max_fps,
	unsigned int min_fps,
	uint64_t bytes_sent,
	int64_t interval_us,
	uint64_t target_bytes_per_second,
	uint64_t avg_send_time_us,
	bool bandwidth_limited_clients
)
{
	if (configured_max_fps == 0)
		return 0;
	if (!bandwidth_limited_clients || target_bytes_per_second == 0 || interval_us <= 0)
		return configured_max_fps;
	if (min_fps == 0)
		min_fps = 1;
	if (min_fps > configured_max_fps)
		min_fps = configured_max_fps;
	if (current_fps == 0 || current_fps > configured_max_fps)
		current_fps = configured_max_fps;

	const uint64_t bytes_per_second =
		bytes_sent * 1000000ull / (uint64_t)interval_us;
	if (bytes_per_second > target_bytes_per_second && current_fps > min_fps) {
		uint64_t scaled =
			(uint64_t)current_fps * target_bytes_per_second / bytes_per_second;
		if (scaled >= current_fps)
			scaled = current_fps - 1;
		if (scaled < min_fps)
			scaled = min_fps;
		return (unsigned int)scaled;
	}

	if (avg_send_time_us > 0 && current_fps > min_fps) {
		const uint64_t frame_budget_us = 1000000ull / current_fps;
		if (avg_send_time_us > frame_budget_us)
			return current_fps - 1;
	}

	if (bytes_per_second * 10 < target_bytes_per_second * 7 &&
	    avg_send_time_us < 20000 && current_fps < configured_max_fps)
		return current_fps + 1;
	return current_fps;
}

unsigned int rf_rdp_core_update_video_quality_level(
	unsigned int current_level,
	unsigned int max_level,
	uint64_t bytes_sent,
	int64_t interval_us,
	uint64_t target_bytes_per_second,
	unsigned int target_fps,
	uint64_t avg_send_time_us,
	uint64_t frames_sent,
	uint64_t frames_skipped,
	uint32_t max_inflight_frames,
	bool video_clients
)
{
	if (!video_clients || max_level == 0 || target_bytes_per_second == 0 ||
	    interval_us <= 0)
		return 0;
	if (current_level > max_level)
		current_level = max_level;

	const uint64_t bytes_per_second =
		bytes_sent * 1000000ull / (uint64_t)interval_us;
	const bool bandwidth_pressure =
		bytes_per_second > target_bytes_per_second;
	if (target_fps == 0)
		target_fps = 30;
	const uint64_t frame_budget_us = 1000000ull / target_fps;
	const uint64_t recovery_threshold_us =
		frame_budget_us / 2 > 0 ? frame_budget_us / 2 : 1;
	const bool send_pressure =
		avg_send_time_us > 0 && avg_send_time_us > frame_budget_us;
	const bool skipped_pressure =
		frames_skipped > 0 &&
		(frames_sent == 0 || frames_skipped > frames_sent / 10);
	const bool inflight_pressure = max_inflight_frames >= 10;
	const bool queue_pressure =
		inflight_pressure || (frames_skipped > 0 && max_inflight_frames >= 8);

	if ((bandwidth_pressure || send_pressure || skipped_pressure ||
	     queue_pressure) && current_level < max_level) {
		if (bandwidth_pressure &&
		    bytes_per_second > target_bytes_per_second * 2 &&
		    current_level + 1 < max_level)
			return current_level + 2;
		return current_level + 1;
	}

	if (current_level > 0 &&
	    !skipped_pressure &&
	    !queue_pressure &&
	    bytes_per_second * 10 < target_bytes_per_second * 2 &&
	    avg_send_time_us < recovery_threshold_us)
		return current_level - 1;

	return current_level;
}

bool rf_rdp_core_should_use_avc444(
	bool avc444_available,
	bool avc420_available,
	unsigned int video_quality_level
)
{
	(void)video_quality_level;

	if (!avc444_available)
		return false;
	return !avc420_available;
}

bool rf_rdp_core_should_skip_avc444_delta(
	unsigned int frame_width,
	unsigned int frame_height,
	unsigned int damage_width,
	unsigned int damage_height
)
{
	if (frame_width == 0 || frame_height == 0 ||
	    damage_width == 0 || damage_height == 0)
		return false;

	const uint64_t frame_pixels = (uint64_t)frame_width * frame_height;
	const uint64_t damage_pixels = (uint64_t)damage_width * damage_height;
	uint64_t threshold = frame_pixels / 4;
	const uint64_t max_delta_pixels = 512ull * 1024ull;

	if (threshold == 0)
		threshold = 1;
	if (threshold > max_delta_pixels)
		threshold = max_delta_pixels;
	return damage_pixels >= threshold;
}

unsigned int rf_rdp_core_rdpgfx_ack_limited_fps(
	unsigned int base_fps,
	uint32_t ack_queue_depth,
	bool ack_queue_depth_valid,
	uint32_t inflight_frames
)
{
	unsigned int fps = base_fps;

	if (base_fps == 0)
		return 0;

	if (ack_queue_depth_valid && ack_queue_depth >= 8) {
		fps = base_fps / 2;

		if (fps == 0)
			fps = 1;
	}

	if (inflight_frames >= 18) {
		unsigned int inflight_fps = base_fps / 2;

		if (inflight_fps == 0)
			inflight_fps = 1;
		if (inflight_fps < fps)
			fps = inflight_fps;
	}

	return fps;
}

enum rf_rdp_graphics_mode rf_rdp_core_select_graphics_mode(
	const char *requested,
	const struct rf_rdp_core_capabilities *caps,
	bool nscodec_encoder_available
)
{
	const char *mode = requested != NULL ? requested : "auto";
	const bool wants_surface_nsc =
		strcmp(mode, "auto") == 0 ||
		strcmp(mode, "surface") == 0 ||
		strcmp(mode, "surface-nsc") == 0;

	if (!wants_surface_nsc)
		return RF_RDP_GRAPHICS_MODE_BITMAP;
	if (!nscodec_encoder_available || caps == NULL ||
	    !caps->surface_set_bits || !caps->nscodec || caps->nscodec_id == 0)
		return RF_RDP_GRAPHICS_MODE_BITMAP;
	return RF_RDP_GRAPHICS_MODE_SURFACE_NSC;
}

static size_t write_data_pdu(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint8_t data_type,
	const uint8_t *body,
	size_t body_length
)
{
	const size_t total_length = 6 + 12 + body_length;
	uint8_t *payload = NULL;
	size_t length = 0;

	if (body_length > 0xffff || total_length > 0xffff ||
	    total_length > 0x7fff ||
	    (body == NULL && body_length > 0))
		return 0;

	payload = malloc(total_length);
	if (payload == NULL)
		return 0;

	struct writer writer = { payload, total_length, 0, false };
	write_u16_le(&writer, total_length);
	write_u16_le(&writer, RF_RDP_PDU_TYPE_DATA | 0x10);
	write_u16_le(&writer, user_id);
	write_u32_le(&writer, share_id);
	write_u8(&writer, 0);
	write_u8(&writer, STREAM_LOW);
	write_u16_le(&writer, body_length);
	write_u8(&writer, data_type);
	write_u8(&writer, 0);
	write_u16_le(&writer, 0);
	write_bytes(&writer, body, body_length);
	if (writer.failed) {
		free(payload);
		return 0;
	}

	length = rf_rdp_mcs_write_send_data_indication(
		data,
		capacity,
		user_id,
		RF_RDP_MCS_GLOBAL_CHANNEL_ID,
		payload,
		writer.length
	);
	free(payload);
	return length;
}

size_t rf_rdp_core_write_license_valid_client(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id
)
{
	uint8_t payload[64] = { 0 };
	struct writer writer = { payload, sizeof(payload), 0, false };

	write_u16_le(&writer, RF_RDP_SEC_LICENSE_PKT);
	write_u16_le(&writer, 0);
	write_u8(&writer, LICENSE_ERROR_ALERT);
	write_u8(&writer, LICENSE_PREAMBLE_VERSION_3_0);
	write_u16_le(&writer, 16);
	write_u32_le(&writer, LICENSE_STATUS_VALID_CLIENT);
	write_u32_le(&writer, LICENSE_ST_NO_TRANSITION);
	write_u16_le(&writer, LICENSE_BB_ERROR_BLOB);
	write_u16_le(&writer, 0);
	if (writer.failed)
		return 0;

	return rf_rdp_mcs_write_send_data_indication(
		data,
		capacity,
		user_id,
		RF_RDP_MCS_GLOBAL_CHANNEL_ID,
		payload,
		writer.length
	);
}

size_t rf_rdp_core_write_demand_active(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint16_t width,
	uint16_t height
)
{
	uint8_t demand[2048] = { 0 };
	uint8_t payload[4096] = { 0 };
	struct writer demand_writer = { demand, sizeof(demand), 0, false };
	struct writer payload_writer = { payload, sizeof(payload), 0, false };

	if (!write_demand_active_payload(
		    &demand_writer,
		    share_id,
		    width,
		    height
	    ))
		return 0;
	if (demand_writer.length > 0xffff - 6)
		return 0;

	write_u16_le(&payload_writer, demand_writer.length + 6);
	write_u16_le(&payload_writer, RF_RDP_PDU_TYPE_DEMAND_ACTIVE | 0x10);
	write_u16_le(&payload_writer, user_id);
	write_bytes(&payload_writer, demand, demand_writer.length);
	if (payload_writer.failed)
		return 0;

	return rf_rdp_mcs_write_send_data_indication(
		data,
		capacity,
		user_id,
		RF_RDP_MCS_GLOBAL_CHANNEL_ID,
		payload,
		payload_writer.length
	);
}

size_t rf_rdp_core_write_bitmap_update(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	const uint8_t *bgrx,
	size_t bgrx_stride
)
{
	uint8_t body[32768] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };
	const size_t row_bytes = (size_t)width * 4;
	const size_t bitmap_length = row_bytes * height;

	if (data == NULL || bgrx == NULL || width == 0 || height == 0 ||
	    bgrx_stride < row_bytes || bitmap_length > 0xffff ||
	    x > 0xffff - width || y > 0xffff - height)
		return 0;

	write_u16_le(&writer, RF_RDP_UPDATE_TYPE_BITMAP);
	write_u16_le(&writer, 1);
	write_u16_le(&writer, x);
	write_u16_le(&writer, y);
	write_u16_le(&writer, x + width - 1);
	write_u16_le(&writer, y + height - 1);
	write_u16_le(&writer, width);
	write_u16_le(&writer, height);
	write_u16_le(&writer, 32);
	write_u16_le(&writer, 0);
	write_u16_le(&writer, bitmap_length);
	for (uint16_t row = 0; row < height; ++row)
		write_bytes(&writer, bgrx + (size_t)row * bgrx_stride, row_bytes);
	if (writer.failed)
		return 0;

	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_UPDATE,
		body,
		writer.length
	);
}

size_t rf_rdp_core_write_compressed_bitmap_update(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	const uint8_t *compressed,
	size_t compressed_length,
	uint16_t scan_width,
	uint16_t uncompressed_size
)
{
	uint8_t body[32768] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };
	const size_t bitmap_length = compressed_length + 8;

	if (data == NULL || compressed == NULL || width == 0 || height == 0 ||
	    compressed_length == 0 || compressed_length > UINT16_MAX ||
	    bitmap_length > UINT16_MAX || x > 0xffff - width ||
	    y > 0xffff - height || scan_width == 0 || uncompressed_size == 0)
		return 0;

	write_u16_le(&writer, RF_RDP_UPDATE_TYPE_BITMAP);
	write_u16_le(&writer, 1);
	write_u16_le(&writer, x);
	write_u16_le(&writer, y);
	write_u16_le(&writer, x + width - 1);
	write_u16_le(&writer, y + height - 1);
	write_u16_le(&writer, width);
	write_u16_le(&writer, height);
	write_u16_le(&writer, 24);
	write_u16_le(&writer, RF_RDP_BITMAP_COMPRESSION);
	write_u16_le(&writer, bitmap_length);
	write_u16_le(&writer, 0);
	write_u16_le(&writer, compressed_length);
	write_u16_le(&writer, scan_width);
	write_u16_le(&writer, uncompressed_size);
	write_bytes(&writer, compressed, compressed_length);
	if (writer.failed)
		return 0;

	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_UPDATE,
		body,
		writer.length
	);
}

size_t rf_rdp_core_write_surface_bits(
	uint8_t *data,
	size_t capacity,
	uint16_t command_type,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	uint8_t bits_per_pixel,
	uint8_t codec_id,
	const uint8_t *bitmap_data,
	size_t bitmap_data_length
)
{
	const size_t surface_command_length = 2 + 8 + 12 + bitmap_data_length;
	const size_t length = 3 + 3 + surface_command_length;
	struct writer writer = { data, capacity, 0, false };

	if (data == NULL || width == 0 || height == 0 ||
	    bitmap_data_length > UINT32_MAX ||
	    (bitmap_data == NULL && bitmap_data_length > 0))
		return 0;
	if (command_type != RF_RDP_SURFCMD_SET_SURFACE_BITS &&
	    command_type != RF_RDP_SURFCMD_STREAM_SURFACE_BITS)
		return 0;
	if (x > UINT16_MAX - width || y > UINT16_MAX - height)
		return 0;
	if (surface_command_length > UINT16_MAX ||
	    length > FASTPATH_MAX_SINGLE_PACKET_SIZE ||
	    capacity < length)
		return 0;

	write_u8(&writer, 0);
	write_u8(&writer, 0x80 | (length >> 8));
	write_u8(&writer, length & 0xff);
	write_u8(&writer, RF_RDP_FASTPATH_UPDATETYPE_SURFCMDS);
	write_u16_le(&writer, surface_command_length);

	write_u16_le(&writer, command_type);
	write_u16_le(&writer, x);
	write_u16_le(&writer, y);
	write_u16_le(&writer, x + width);
	write_u16_le(&writer, y + height);
	write_u8(&writer, bits_per_pixel);
	write_u8(&writer, 0);
	write_u8(&writer, 0);
	write_u8(&writer, codec_id);
	write_u16_le(&writer, width);
	write_u16_le(&writer, height);
	write_u32_le(&writer, bitmap_data_length);
	write_bytes(&writer, bitmap_data, bitmap_data_length);

	if (writer.failed)
		return 0;
	return writer.length;
}

size_t rf_rdp_core_write_server_synchronize(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
)
{
	uint8_t body[4] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };

	write_u16_le(&writer, RF_RDP_SYNC_MESSAGE_TYPE_SYNC);
	write_u16_le(&writer, user_id);
	if (writer.failed)
		return 0;
	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_SYNCHRONIZE,
		body,
		writer.length
	);
}

size_t rf_rdp_core_write_server_control_cooperate(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
)
{
	uint8_t body[8] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };

	write_u16_le(&writer, RF_RDP_CONTROL_ACTION_COOPERATE);
	write_u16_le(&writer, 0);
	write_u32_le(&writer, 0);
	if (writer.failed)
		return 0;
	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_CONTROL,
		body,
		writer.length
	);
}

size_t rf_rdp_core_write_server_control_granted(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
)
{
	uint8_t body[8] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };

	write_u16_le(&writer, RF_RDP_CONTROL_ACTION_GRANTED_CONTROL);
	write_u16_le(&writer, user_id);
	write_u32_le(&writer, 0x03ea);
	if (writer.failed)
		return 0;
	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_CONTROL,
		body,
		writer.length
	);
}

size_t rf_rdp_core_write_server_font_map(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
)
{
	uint8_t body[8] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };

	write_u16_le(&writer, 0);
	write_u16_le(&writer, 0);
	write_u16_le(&writer, RF_RDP_FONTLIST_FIRST | RF_RDP_FONTLIST_LAST);
	write_u16_le(&writer, 4);
	if (writer.failed)
		return 0;
	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_FONT_MAP,
		body,
		writer.length
	);
}

size_t rf_rdp_core_write_server_default_pointer(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
)
{
	uint8_t body[8] = { 0 };
	struct writer writer = { body, sizeof(body), 0, false };

	write_u16_le(&writer, PTR_MSG_TYPE_SYSTEM);
	write_u16_le(&writer, 0);
	write_u32_le(&writer, SYSPTR_DEFAULT);
	if (writer.failed)
		return 0;
	return write_data_pdu(
		data,
		capacity,
		user_id,
		share_id,
		RF_RDP_DATA_PDU_TYPE_POINTER,
		body,
		writer.length
	);
}
