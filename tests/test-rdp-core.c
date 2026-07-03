#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-core.h"
#include "rf-rdp-gfx.h"
#include "rf-rdp-mcs.h"
#include "rf-rdp-proto.h"

extern bool rf_rdp_core_should_defer_graphics_for_rdpgfx(
	bool drdynvc_advertised,
	bool rdpgfx_disabled,
	bool rdpgfx_caps_confirmed
);
struct rf_rect {
	int x;
	int y;
	unsigned int w;
	unsigned int h;
};

static bool contains_bytes(
	const uint8_t *haystack,
	size_t haystack_length,
	const uint8_t *needle,
	size_t needle_length
)
{
	if (needle_length == 0)
		return true;
	if (haystack_length < needle_length)
		return false;

	for (size_t i = 0; i <= haystack_length - needle_length; ++i) {
		if (memcmp(haystack + i, needle, needle_length) == 0)
			return true;
	}
	return false;
}

static size_t write_client_data_request(
	uint8_t *out,
	size_t capacity,
	uint8_t data_type,
	const uint8_t *body,
	size_t body_length
)
{
	const size_t payload_length = 6 + 12 + body_length;
	const size_t length = 15 + payload_length;
	size_t offset = 0;

	assert(capacity >= length);
	assert(payload_length <= 0x7fff);

	out[offset++] = 0x03;
	out[offset++] = 0x00;
	out[offset++] = length >> 8;
	out[offset++] = length & 0xff;
	out[offset++] = 0x02;
	out[offset++] = 0xf0;
	out[offset++] = 0x80;
	out[offset++] = RF_RDP_MCS_PDU_SEND_DATA_REQUEST << 2;
	out[offset++] = 0x00;
	out[offset++] = 0x00;
	out[offset++] = 0x03;
	out[offset++] = 0xeb;
	out[offset++] = 0x70;
	out[offset++] = (payload_length | 0x8000) >> 8;
	out[offset++] = (payload_length | 0x8000) & 0xff;

	out[offset++] = payload_length & 0xff;
	out[offset++] = payload_length >> 8;
	out[offset++] = RF_RDP_PDU_TYPE_DATA | 0x10;
	out[offset++] = 0x00;
	out[offset++] = 0xe9;
	out[offset++] = 0x03;
	out[offset++] = RF_RDP_DEFAULT_SHARE_ID & 0xff;
	out[offset++] = (RF_RDP_DEFAULT_SHARE_ID >> 8) & 0xff;
	out[offset++] = (RF_RDP_DEFAULT_SHARE_ID >> 16) & 0xff;
	out[offset++] = (RF_RDP_DEFAULT_SHARE_ID >> 24) & 0xff;
	out[offset++] = 0x00;
	out[offset++] = 0x01;
	out[offset++] = body_length & 0xff;
	out[offset++] = body_length >> 8;
	out[offset++] = data_type;
	out[offset++] = 0x00;
	out[offset++] = 0x00;
	out[offset++] = 0x00;
	memcpy(out + offset, body, body_length);
	offset += body_length;

	return offset;
}

static void append_u8(uint8_t *out, size_t capacity, size_t *offset, uint8_t value)
{
	assert(*offset < capacity);
	out[(*offset)++] = value;
}

static void append_u16_le(
	uint8_t *out,
	size_t capacity,
	size_t *offset,
	uint16_t value
)
{
	append_u8(out, capacity, offset, value & 0xff);
	append_u8(out, capacity, offset, value >> 8);
}

static void append_u32_le(
	uint8_t *out,
	size_t capacity,
	size_t *offset,
	uint32_t value
)
{
	append_u8(out, capacity, offset, value & 0xff);
	append_u8(out, capacity, offset, (value >> 8) & 0xff);
	append_u8(out, capacity, offset, (value >> 16) & 0xff);
	append_u8(out, capacity, offset, (value >> 24) & 0xff);
}

static void append_bytes(
	uint8_t *out,
	size_t capacity,
	size_t *offset,
	const uint8_t *data,
	size_t length
)
{
	assert(capacity - *offset >= length);
	memcpy(out + *offset, data, length);
	*offset += length;
}

static void test_parse_client_info_shape(void)
{
	const uint8_t pdu[] = {
		0x03, 0x00, 0x00, 0x13,
		0x02, 0xf0, 0x80,
		0x64, 0x00, 0x00, 0x03, 0xeb,
		0x70, 0x80, 0x04,
		0x40, 0x00, 0x00, 0x00
	};
	struct rf_rdp_core_pdu parsed = { 0 };

	assert(rf_rdp_core_parse_client_pdu(pdu, sizeof(pdu), &parsed));
	assert(parsed.channel_id == RF_RDP_MCS_GLOBAL_CHANNEL_ID);
	assert(parsed.security_flags == RF_RDP_SEC_INFO_PKT);
	assert(parsed.payload_length == 0);
}

static void test_write_license_valid_client(void)
{
	uint8_t out[256] = { 0 };
	uint16_t tpkt_length = 0;
	const uint8_t license_payload[] = {
		0x80, 0x00, 0x00, 0x00,
		0xff, 0x03, 0x10, 0x00,
		0x07, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x00, 0x00
	};

	const size_t length = rf_rdp_core_write_license_valid_client(
		out, sizeof(out), RF_RDP_MCS_BASE_CHANNEL_ID
	);
	assert(length == 35);
	assert(rf_rdp_read_tpkt_header(out, length, &tpkt_length));
	assert(tpkt_length == length);
	assert(out[7] == 0x68);
	assert(out[12] == 0x70);
	assert(out[13] == 0x80);
	assert(out[14] == sizeof(license_payload));
	assert(memcmp(out + 15, license_payload, sizeof(license_payload)) == 0);
}

static void test_write_demand_active(void)
{
	uint8_t out[1024] = { 0 };
	uint16_t tpkt_length = 0;
	const uint8_t source_descriptor[] = { 'R', 'D', 'P', '\0' };
	const uint8_t general_capability[] = { 0x01, 0x00, 0x18, 0x00 };
	const uint8_t bitmap_capability[] = { 0x02, 0x00, 0x1c, 0x00 };
	const uint8_t input_capability[] = { 0x0d, 0x00, 0x58, 0x00 };
	const uint8_t font_capability[] = { 0x0e, 0x00, 0x08, 0x00 };
	const uint8_t nscodec_server_capability[] = {
		0x1d, 0x00, 0x1c, 0x00,
		0x01,
		0xb9, 0x1b, 0x8d, 0xca,
		0x0f, 0x00,
		0x4f, 0x15,
		0x58, 0x9f, 0xae, 0x2d, 0x1a, 0x87, 0xe2, 0xd6,
		0x00,
		0x04, 0x00,
		0x00, 0x00, 0x00, 0x00
	};

	const size_t length = rf_rdp_core_write_demand_active(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID,
		1920,
		1080
	);
	assert(length > 64);
	assert(rf_rdp_read_tpkt_header(out, length, &tpkt_length));
	assert(tpkt_length == length);
	assert(out[7] == 0x68);
	assert(out[15 + 2] == 0x11);
	assert(out[15 + 3] == 0x00);
	assert(contains_bytes(out, length, source_descriptor, sizeof(source_descriptor)));
	assert(contains_bytes(out, length, general_capability, sizeof(general_capability)));
	assert(contains_bytes(out, length, bitmap_capability, sizeof(bitmap_capability)));
	assert(contains_bytes(out, length, input_capability, sizeof(input_capability)));
	assert(contains_bytes(out, length, font_capability, sizeof(font_capability)));
	assert(contains_bytes(
		out,
		length,
		nscodec_server_capability,
		sizeof(nscodec_server_capability)
	));
}

static void test_write_finalization_data_pdus(void)
{
	uint8_t out[256] = { 0 };
	const uint8_t sync_body[] = {
		0x01, 0x00, 0xe9, 0x03
	};
	const uint8_t control_granted_body[] = {
		0x02, 0x00, 0xe9, 0x03,
		0xea, 0x03, 0x00, 0x00
	};
	const uint8_t control_cooperate_body[] = {
		0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	const uint8_t font_map_body[] = {
		0x00, 0x00, 0x00, 0x00,
		0x03, 0x00, 0x04, 0x00
	};
	const uint8_t pointer_default_body[] = {
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x7f, 0x00, 0x00
	};

	size_t length = rf_rdp_core_write_server_synchronize(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID
	);
	assert(length == 37);
	assert(out[7] == 0x68);
	assert(out[15 + 2] == 0x17);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_SYNCHRONIZE);
	assert(memcmp(out + length - sizeof(sync_body), sync_body, sizeof(sync_body)) == 0);

	length = rf_rdp_core_write_server_control_cooperate(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID
	);
	assert(length == 41);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_CONTROL);
	assert(memcmp(
		out + length - sizeof(control_cooperate_body),
		control_cooperate_body,
		sizeof(control_cooperate_body)
	) == 0);

	length = rf_rdp_core_write_server_control_granted(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID
	);
	assert(length == 41);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_CONTROL);
	assert(memcmp(
		out + length - sizeof(control_granted_body),
		control_granted_body,
		sizeof(control_granted_body)
	) == 0);

	length = rf_rdp_core_write_server_font_map(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID
	);
	assert(length == 41);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_FONT_MAP);
	assert(memcmp(
		out + length - sizeof(font_map_body),
		font_map_body,
		sizeof(font_map_body)
	) == 0);

	length = rf_rdp_core_write_server_default_pointer(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID
	);
	assert(length == 41);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_POINTER);
	assert(memcmp(
		out + length - sizeof(pointer_default_body),
		pointer_default_body,
		sizeof(pointer_default_body)
	) == 0);
}

static void test_parse_client_finalization_data(void)
{
	uint8_t out[128] = { 0 };
	const uint8_t sync_body[] = {
		0x01, 0x00, 0xe9, 0x03
	};
	const uint8_t control_request_body[] = {
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	const uint8_t font_list_body[] = {
		0x00, 0x00, 0x00, 0x00,
		0x03, 0x00, 0x04, 0x00
	};
	const uint8_t invalid_sync_body[] = {
		0x02, 0x00, 0xe9, 0x03
	};
	struct rf_rdp_core_pdu parsed = { 0 };
	struct rf_rdp_core_control_pdu control = { 0 };
	uint16_t target_user = 0;
	uint16_t font_flags = 0;

	size_t length = write_client_data_request(
		out,
		sizeof(out),
		RF_RDP_DATA_PDU_TYPE_SYNCHRONIZE,
		sync_body,
		sizeof(sync_body)
	);
	assert(rf_rdp_core_parse_client_pdu(out, length, &parsed));
	assert(parsed.data_type == RF_RDP_DATA_PDU_TYPE_SYNCHRONIZE);
	assert(rf_rdp_core_parse_synchronize_body(
		out + parsed.payload_offset,
		parsed.payload_length,
		&target_user
	));
	assert(target_user == RF_RDP_MCS_BASE_CHANNEL_ID);

	length = write_client_data_request(
		out,
		sizeof(out),
		RF_RDP_DATA_PDU_TYPE_CONTROL,
		control_request_body,
		sizeof(control_request_body)
	);
	assert(rf_rdp_core_parse_client_pdu(out, length, &parsed));
	assert(parsed.data_type == RF_RDP_DATA_PDU_TYPE_CONTROL);
	assert(rf_rdp_core_parse_control_body(
		out + parsed.payload_offset,
		parsed.payload_length,
		&control
	));
	assert(control.action == RF_RDP_CONTROL_ACTION_REQUEST_CONTROL);
	assert(control.grant_id == 0);
	assert(control.control_id == 0);

	length = write_client_data_request(
		out,
		sizeof(out),
		RF_RDP_DATA_PDU_TYPE_FONT_LIST,
		font_list_body,
		sizeof(font_list_body)
	);
	assert(rf_rdp_core_parse_client_pdu(out, length, &parsed));
	assert(parsed.data_type == RF_RDP_DATA_PDU_TYPE_FONT_LIST);
	assert(rf_rdp_core_parse_font_list_body(
		out + parsed.payload_offset,
		parsed.payload_length,
		&font_flags
	));
	assert(font_flags == (RF_RDP_FONTLIST_FIRST | RF_RDP_FONTLIST_LAST));

	assert(!rf_rdp_core_parse_synchronize_body(
		invalid_sync_body,
		sizeof(invalid_sync_body),
		&target_user
	));
	assert(!rf_rdp_core_parse_control_body(sync_body, 2, &control));
}

static void test_parse_confirm_active_capabilities(void)
{
	const uint8_t nscodec_guid[] = {
		0xb9, 0x1b, 0x8d, 0xca,
		0x0f, 0x00,
		0x4f, 0x15,
		0x58, 0x9f, 0xae, 0x2d, 0x1a, 0x87, 0xe2, 0xd6
	};
	const uint8_t remotefx_guid[] = {
		0x12, 0x2f, 0x77, 0x76,
		0x72, 0xbd,
		0x63, 0x44,
		0xaf, 0xb3, 0xb7, 0x3c, 0x9c, 0x6f, 0x78, 0x86
	};
	const uint8_t nscodec_properties[] = { 1, 1, 3 };
	uint8_t body[128] = { 0 };
	size_t offset = 0;
	struct rf_rdp_core_capabilities caps = { 0 };

	append_u32_le(body, sizeof(body), &offset, RF_RDP_DEFAULT_SHARE_ID);
	append_u16_le(body, sizeof(body), &offset, RF_RDP_MCS_BASE_CHANNEL_ID);
	append_u16_le(body, sizeof(body), &offset, 4);
	append_u16_le(body, sizeof(body), &offset, 62);
	append_bytes(body, sizeof(body), &offset, (const uint8_t *)"RDP\0", 4);
	append_u16_le(body, sizeof(body), &offset, 2);
	append_u16_le(body, sizeof(body), &offset, 0);

	append_u16_le(body, sizeof(body), &offset, RF_RDP_CAPSET_TYPE_SURFACE_COMMANDS);
	append_u16_le(body, sizeof(body), &offset, 12);
	append_u32_le(body, sizeof(body), &offset,
		RF_RDP_SURFCMDS_SET_SURFACE_BITS |
		RF_RDP_SURFCMDS_STREAM_SURFACE_BITS);
	append_u32_le(body, sizeof(body), &offset, 0);

	append_u16_le(body, sizeof(body), &offset, RF_RDP_CAPSET_TYPE_BITMAP_CODECS);
	append_u16_le(body, sizeof(body), &offset, 46);
	append_u8(body, sizeof(body), &offset, 2);
	append_bytes(body, sizeof(body), &offset, nscodec_guid, sizeof(nscodec_guid));
	append_u8(body, sizeof(body), &offset, RF_RDP_CODEC_ID_NSCODEC);
	append_u16_le(body, sizeof(body), &offset, sizeof(nscodec_properties));
	append_bytes(
		body,
		sizeof(body),
		&offset,
		nscodec_properties,
		sizeof(nscodec_properties)
	);
	append_bytes(body, sizeof(body), &offset, remotefx_guid, sizeof(remotefx_guid));
	append_u8(body, sizeof(body), &offset, RF_RDP_CODEC_ID_REMOTEFX);
	append_u16_le(body, sizeof(body), &offset, 0);

	assert(offset == 76);
	assert(rf_rdp_core_parse_confirm_active_capabilities(
		body,
		offset,
		&caps
	));
	assert(caps.surface_set_bits);
	assert(caps.surface_stream_bits);
	assert(caps.nscodec);
	assert(caps.nscodec_id == RF_RDP_CODEC_ID_NSCODEC);
	assert(caps.remotefx);
	assert(caps.remotefx_id == RF_RDP_CODEC_ID_REMOTEFX);
	assert(!rf_rdp_core_parse_confirm_active_capabilities(body, 10, &caps));
}

static void test_write_bitmap_update(void)
{
	uint8_t out[128] = { 0 };
	const uint8_t bgrx[] = {
		0x10, 0x20, 0x30, 0xff,
		0x40, 0x50, 0x60, 0xff,
		0x70, 0x80, 0x90, 0xff,
		0xa0, 0xb0, 0xc0, 0xff
	};
	const size_t body = 15 + 6 + 12;

	size_t length = rf_rdp_core_write_bitmap_update(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID,
		5,
		7,
		2,
		2,
		bgrx,
		8
	);
	assert(length == 71);
	assert(out[7] == 0x68);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_UPDATE);
	assert(out[body] == RF_RDP_UPDATE_TYPE_BITMAP);
	assert(out[body + 1] == 0x00);
	assert(out[body + 2] == 0x01);
	assert(out[body + 3] == 0x00);
	assert(out[body + 4] == 0x05);
	assert(out[body + 6] == 0x07);
	assert(out[body + 8] == 0x06);
	assert(out[body + 10] == 0x08);
	assert(out[body + 12] == 0x02);
	assert(out[body + 14] == 0x02);
	assert(out[body + 16] == 0x20);
	assert(out[body + 18] == 0x00);
	assert(out[body + 20] == sizeof(bgrx));
	assert(memcmp(out + length - sizeof(bgrx), bgrx, sizeof(bgrx)) == 0);
	assert(rf_rdp_core_write_bitmap_update(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID,
		0,
		0,
		2,
		2,
		bgrx,
		4
	) == 0);
}

static void test_write_compressed_bitmap_update_uses_rle24_header(void)
{
	uint8_t out[128] = { 0 };
	const uint8_t bgr[] = {
		0x10, 0x20, 0x30,
		0x10, 0x20, 0x30,
		0x10, 0x20, 0x30,
		0x10, 0x20, 0x30
	};
	uint8_t rle[16] = { 0 };
	const size_t body = 15 + 6 + 12;

	const size_t rle_length = rf_rdp_core_compress_bgr24_bitmap(
		rle,
		sizeof(rle),
		bgr,
		2,
		2,
		2 * 3
	);
	assert(rle_length == 4);
	assert(rle[0] == 0x64);
	assert(rle[1] == 0x10);
	assert(rle[2] == 0x20);
	assert(rle[3] == 0x30);

	size_t length = rf_rdp_core_write_compressed_bitmap_update(
		out,
		sizeof(out),
		RF_RDP_MCS_BASE_CHANNEL_ID,
		RF_RDP_DEFAULT_SHARE_ID,
		5,
		7,
		2,
		2,
		rle,
		rle_length,
		2 * 3,
		sizeof(bgr)
	);
	assert(length == 67);
	assert(out[7] == 0x68);
	assert(out[15 + 14] == RF_RDP_DATA_PDU_TYPE_UPDATE);
	assert(out[body] == RF_RDP_UPDATE_TYPE_BITMAP);
	assert(out[body + 16] == 24);
	assert(out[body + 18] == RF_RDP_BITMAP_COMPRESSION);
	assert(out[body + 20] == rle_length + 8);
	assert(out[body + 22] == 0);
	assert(out[body + 24] == rle_length);
	assert(out[body + 26] == 2 * 3);
	assert(out[body + 28] == sizeof(bgr));
	assert(memcmp(out + length - rle_length, rle, rle_length) == 0);
}

static void test_write_surface_bits_fastpath_update(void)
{
	uint8_t out[128] = { 0 };
	const uint8_t payload[] = { 0xde, 0xad, 0xbe, 0xef };
	const size_t length = rf_rdp_core_write_surface_bits(
		out,
		sizeof(out),
		RF_RDP_SURFCMD_SET_SURFACE_BITS,
		5,
		7,
		2,
		3,
		32,
		RF_RDP_CODEC_ID_NSCODEC,
		payload,
		sizeof(payload)
	);
	const size_t body = 6;

	assert(length == 32);
	assert(out[0] == 0x00);
	assert(out[1] == 0x80);
	assert(out[2] == length);
	assert(out[3] == RF_RDP_FASTPATH_UPDATETYPE_SURFCMDS);
	assert(out[4] == 26);
	assert(out[5] == 0x00);
	assert(out[body] == RF_RDP_SURFCMD_SET_SURFACE_BITS);
	assert(out[body + 2] == 5);
	assert(out[body + 4] == 7);
	assert(out[body + 6] == 7);
	assert(out[body + 8] == 10);
	assert(out[body + 10] == 32);
	assert(out[body + 11] == 0);
	assert(out[body + 12] == 0);
	assert(out[body + 13] == RF_RDP_CODEC_ID_NSCODEC);
	assert(out[body + 14] == 2);
	assert(out[body + 16] == 3);
	assert(out[body + 18] == sizeof(payload));
	assert(memcmp(out + length - sizeof(payload), payload, sizeof(payload)) == 0);
	assert(rf_rdp_core_write_surface_bits(
		out,
		sizeof(out),
		0xffff,
		0,
		0,
		2,
		3,
		32,
		RF_RDP_CODEC_ID_NSCODEC,
		payload,
		sizeof(payload)
	) == 0);
}

static void test_select_graphics_mode(void)
{
	struct rf_rdp_core_capabilities caps = {
		.surface_set_bits = true,
		.nscodec = true,
		.nscodec_id = RF_RDP_CODEC_ID_NSCODEC
	};

	assert(rf_rdp_core_select_graphics_mode(
		"auto",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_SURFACE_NSC);
	assert(rf_rdp_core_select_graphics_mode(
		"surface-nsc",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_SURFACE_NSC);
	assert(rf_rdp_core_select_graphics_mode(
		"surface",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_SURFACE_NSC);
	assert(rf_rdp_core_select_graphics_mode(
		"bitmap",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_BITMAP);
	assert(rf_rdp_core_select_graphics_mode(
		"auto",
		&caps,
		false
	) == RF_RDP_GRAPHICS_MODE_BITMAP);
	assert(rf_rdp_core_select_graphics_mode(
		"bogus",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_BITMAP);

	caps.surface_set_bits = false;
	assert(rf_rdp_core_select_graphics_mode(
		"surface-nsc",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_BITMAP);
	caps.surface_set_bits = true;
	caps.nscodec = false;
	assert(rf_rdp_core_select_graphics_mode(
		"auto",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_BITMAP);
	caps.nscodec = true;
	caps.nscodec_id = 0;
	assert(rf_rdp_core_select_graphics_mode(
		"auto",
		&caps,
		true
	) == RF_RDP_GRAPHICS_MODE_BITMAP);
}

static void test_defer_legacy_graphics_until_rdpgfx_caps_resolve(void)
{
	assert(rf_rdp_core_should_defer_graphics_for_rdpgfx(
		true,
		false,
		false
	));
	assert(!rf_rdp_core_should_defer_graphics_for_rdpgfx(
		false,
		false,
		false
	));
	assert(!rf_rdp_core_should_defer_graphics_for_rdpgfx(
		true,
		true,
		false
	));
	assert(!rf_rdp_core_should_defer_graphics_for_rdpgfx(
		true,
		false,
		true
	));
}

static void test_convert_rgba_rect_to_bgrx_bottom_up(void)
{
	const uint8_t rgba[] = {
		0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08,
		0x09, 0x0a, 0x0b, 0x0c,
		0x11, 0x12, 0x13, 0x14,
		0x15, 0x16, 0x17, 0x18,
		0x19, 0x1a, 0x1b, 0x1c
	};
	const uint8_t expected[] = {
		0x17, 0x16, 0x15, 0xff,
		0x1b, 0x1a, 0x19, 0xff,
		0x07, 0x06, 0x05, 0xff,
		0x0b, 0x0a, 0x09, 0xff
	};
	uint8_t bgrx[sizeof(expected)] = { 0 };

	assert(rf_rdp_core_convert_rgba_rect_to_bgrx_bottom_up(
		bgrx,
		sizeof(bgrx),
		rgba,
		sizeof(rgba),
		3 * 4,
		1,
		0,
		2,
		2
	));
	assert(memcmp(bgrx, expected, sizeof(expected)) == 0);
	assert(!rf_rdp_core_convert_rgba_rect_to_bgrx_bottom_up(
		bgrx,
		4,
		rgba,
		sizeof(rgba),
		3 * 4,
		0,
		0,
		2,
		2
	));
}

static void test_convert_rgba_rect_to_bgrx_top_down(void)
{
	const uint8_t rgba[] = {
		0x10, 0x20, 0x30, 0xff,
		0x40, 0x50, 0x60, 0xff,
		0x70, 0x80, 0x90, 0xff,
		0xa0, 0xb0, 0xc0, 0xff,
		0xd0, 0xe0, 0xf0, 0xff,
		0x01, 0x02, 0x03, 0xff
	};
	const uint8_t expected[] = {
		0x60, 0x50, 0x40, 0xff,
		0x90, 0x80, 0x70, 0xff,
		0xf0, 0xe0, 0xd0, 0xff,
		0x03, 0x02, 0x01, 0xff
	};
	uint8_t bgrx[sizeof(expected)] = { 0 };

	assert(rf_rdp_core_convert_rgba_rect_to_bgrx_top_down(
		bgrx,
		sizeof(bgrx),
		rgba,
		sizeof(rgba),
		3 * 4,
		1,
		0,
		2,
		2
	));
	assert(memcmp(bgrx, expected, sizeof(expected)) == 0);
	assert(!rf_rdp_core_convert_rgba_rect_to_bgrx_top_down(
		bgrx,
		4,
		rgba,
		sizeof(rgba),
		3 * 4,
		0,
		0,
		2,
		2
	));
}

static void test_convert_rgba_rect_to_bgr_bottom_up(void)
{
	const uint8_t rgba[] = {
		0x01, 0x02, 0x03, 0x04,
		0x05, 0x06, 0x07, 0x08,
		0x09, 0x0a, 0x0b, 0x0c,
		0x11, 0x12, 0x13, 0x14,
		0x15, 0x16, 0x17, 0x18,
		0x19, 0x1a, 0x1b, 0x1c
	};
	const uint8_t expected[] = {
		0x17, 0x16, 0x15,
		0x1b, 0x1a, 0x19,
		0x07, 0x06, 0x05,
		0x0b, 0x0a, 0x09
	};
	uint8_t bgr[sizeof(expected)] = { 0 };

	assert(rf_rdp_core_convert_rgba_rect_to_bgr_bottom_up(
		bgr,
		sizeof(bgr),
		rgba,
		sizeof(rgba),
		3 * 4,
		1,
		0,
		2,
		2
	));
	assert(memcmp(bgr, expected, sizeof(expected)) == 0);
	assert(!rf_rdp_core_convert_rgba_rect_to_bgr_bottom_up(
		bgr,
		4,
		rgba,
		sizeof(rgba),
		3 * 4,
		0,
		0,
		2,
		2
	));
}

static void test_clip_update_rect_forces_full_frame(void)
{
	struct rf_rect damage = { 10, 20, 30, 40 };
	uint16_t x = 0;
	uint16_t y = 0;
	uint16_t width = 0;
	uint16_t height = 0;

	assert(rf_rdp_core_clip_update_rect(
		800,
		600,
		&damage,
		false,
		&x,
		&y,
		&width,
		&height
	));
	assert(x == 10);
	assert(y == 20);
	assert(width == 30);
	assert(height == 40);

	assert(rf_rdp_core_clip_update_rect(
		800,
		600,
		&damage,
		true,
		&x,
		&y,
		&width,
		&height
	));
	assert(x == 0);
	assert(y == 0);
	assert(width == 800);
	assert(height == 600);
}

static void test_rgba_rect_source_points_at_damage_origin(void)
{
	size_t offset = 0;
	size_t available = 0;
	const size_t stride = 1920 * 4;
	const size_t length = stride * 1080;

	assert(rf_rdp_core_get_rgba_rect_source(
		length,
		stride,
		10,
		20,
		100,
		32,
		&offset,
		&available
	));
	assert(offset == 20 * stride + 10 * 4);
	assert(available == length - offset);
	assert(!rf_rdp_core_get_rgba_rect_source(
		length,
		stride,
		1900,
		20,
		32,
		32,
		&offset,
		&available
	));
	assert(!rf_rdp_core_get_rgba_rect_source(
		length,
		99 * 4,
		10,
		20,
		100,
		32,
		&offset,
		&available
	));
}

static void test_expand_update_rect_keeps_rect_inside_frame(void)
{
	uint16_t x = 0;
	uint16_t y = 544;
	uint16_t width = 1920;
	uint16_t height = 32;

	assert(rf_rdp_core_expand_update_rect(
		1920,
		1080,
		64,
		64,
		&x,
		&y,
		&width,
		&height
	));
	assert(x == 0);
	assert(y == 528);
	assert(width == 1920);
	assert(height == 64);

	x = 1900;
	y = 1070;
	width = 20;
	height = 10;
	assert(rf_rdp_core_expand_update_rect(
		1920,
		1080,
		64,
		64,
		&x,
		&y,
		&width,
		&height
	));
	assert(x == 1856);
	assert(y == 1016);
	assert(width == 64);
	assert(height == 64);
}

static void test_full_surface_rect_ignores_damage_rect(void)
{
	uint16_t x = 320;
	uint16_t y = 240;
	uint16_t width = 160;
	uint16_t height = 64;

	assert(rf_rdp_core_make_full_surface_rect(
		1920,
		1080,
		&x,
		&y,
		&width,
		&height
	));
	assert(x == 0);
	assert(y == 0);
	assert(width == 1920);
	assert(height == 1080);

	assert(!rf_rdp_core_make_full_surface_rect(
		0,
		1080,
		&x,
		&y,
		&width,
		&height
	));
}

static void test_frame_pacing_policy(void)
{
	const int64_t one_second = 1000000;

	assert(rf_rdp_core_frame_should_render(-1, 0, 30, false));
	assert(rf_rdp_core_frame_should_render(1000, 2000, 30, true));
	assert(!rf_rdp_core_frame_should_render(1000, 2000, 30, false));
	assert(rf_rdp_core_frame_should_render(1000, 1000 + one_second / 30, 30, false));
	assert(rf_rdp_core_frame_should_render(1000, 1001, 0, false));
}

static void test_frame_pacing_accepts_small_source_jitter(void)
{
	int64_t next_frame_time_us = -1;
	unsigned int rendered = 0;

	for (unsigned int i = 0; i < 120; ++i) {
		const int64_t now = (int64_t)i * 16660;

		if (rf_rdp_core_frame_scheduler_should_render(
			    &next_frame_time_us,
			    now,
			    60,
			    false
		    ))
			rendered++;
	}

	assert(rendered == 120);
}

static void test_frame_pacing_limits_fast_source(void)
{
	int64_t next_frame_time_us = -1;
	unsigned int rendered = 0;

	for (unsigned int i = 0; i < 120; ++i) {
		const int64_t now = (int64_t)i * 8333;

		if (rf_rdp_core_frame_scheduler_should_render(
			    &next_frame_time_us,
			    now,
			    60,
			    false
		    ))
			rendered++;
	}

	assert(rendered >= 59);
	assert(rendered <= 61);
}

static void test_adaptive_bitmap_fps_policy(void)
{
	const int64_t five_seconds = 5 * 1000000;
	const uint64_t six_mb_per_second = 6 * 1024 * 1024;

	assert(rf_rdp_core_update_adaptive_fps(
		15,
		15,
		3,
		50 * 1024 * 1024,
		five_seconds,
		six_mb_per_second,
		12000,
		true
	) == 9);
	assert(rf_rdp_core_update_adaptive_fps(
		9,
		15,
		3,
		2 * 1024 * 1024,
		five_seconds,
		six_mb_per_second,
		8000,
		true
	) == 10);
	assert(rf_rdp_core_update_adaptive_fps(
		10,
		15,
		3,
		2 * 1024 * 1024,
		five_seconds,
		six_mb_per_second,
		45000,
		true
	) == 10);
	assert(rf_rdp_core_update_adaptive_fps(
		15,
		15,
		3,
		2 * 1024 * 1024,
		five_seconds,
		six_mb_per_second,
		90000,
		true
	) == 14);
	assert(rf_rdp_core_update_adaptive_fps(
		9,
		15,
		3,
		100 * 1024 * 1024,
		five_seconds,
		six_mb_per_second,
		12000,
		false
	) == 15);
	assert(rf_rdp_core_update_adaptive_fps(
		9,
		0,
		3,
		100 * 1024 * 1024,
		five_seconds,
		six_mb_per_second,
		12000,
		true
	) == 0);
}

static void test_rdpgfx_quality_headroom_holds_fallback_fps_limit(void)
{
	assert(!rf_rdp_core_should_limit_fallback_fps_for_quality_state(
		true,
		true,
		1,
		3
	));
	assert(rf_rdp_core_should_limit_fallback_fps_for_quality_state(
		true,
		true,
		3,
		3
	));
	assert(rf_rdp_core_should_limit_fallback_fps_for_quality_state(
		true,
		false,
		1,
		3
	));
	assert(!rf_rdp_core_should_limit_fallback_fps_for_quality_state(
		false,
		true,
		1,
		3
	));
}

static void test_rdpgfx_ack_fps_policy_keeps_fps_until_queues_are_critical(void)
{
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 4) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 12) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 18) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 24) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 32) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 48) == 45);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, true, 64) == 30);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 3, true, 0) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 8, true, 0) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 16, true, 0) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 32, true, 0) == 45);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 48, true, 0) == 30);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 8, true, 18) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(45, 32, true, 0) == 33);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(45, 48, true, 0) == 22);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(60, 0, false, 18) == 60);
	assert(rf_rdp_core_rdpgfx_ack_limited_fps(0, 0, true, 32) == 0);
}

static void test_avc444_delta_policy_skips_large_damage(void)
{
	assert(rf_rdp_core_should_skip_avc444_delta(1920, 1080, 1920, 1080));
	assert(rf_rdp_core_should_skip_avc444_delta(1920, 1080, 1920, 270));
	assert(!rf_rdp_core_should_skip_avc444_delta(1920, 1080, 320, 180));
	assert(!rf_rdp_core_should_skip_avc444_delta(0, 1080, 1920, 1080));
	assert(!rf_rdp_core_should_skip_avc444_delta(1920, 1080, 0, 1080));
	assert(rf_rdp_core_should_skip_avc444_delta_for_quality(
		1920,
		1080,
		1920,
		1080,
		1
	));
	assert(!rf_rdp_core_should_skip_avc444_delta_for_quality(
		1920,
		1080,
		1920,
		1080,
		2
	));
	assert(!rf_rdp_core_should_skip_avc444_delta_for_quality(
		1920,
		1080,
		1920,
		1080,
		3
	));
}

static void test_rdpgfx_avc_quality_parameters(void)
{
	assert(rf_rdp_core_rdpgfx_avc_bit_rate(1920, 1088, 60, 3, false) == 5013504);
	assert(rf_rdp_core_rdpgfx_avc_bit_rate(1920, 1088, 60, 3, true) == 3133440);
	assert(rf_rdp_core_rdpgfx_avc_qp(3, false) == 38);
	assert(rf_rdp_core_rdpgfx_avc_qp(3, true) == 42);
	assert(rf_rdp_core_rdpgfx_avc_quality(3, false) == 55);
	assert(rf_rdp_core_rdpgfx_avc_quality(3, true) == 58);
	assert(rf_rdp_core_rdpgfx_avc_gop_size(60, 3, false) == 240);
	assert(rf_rdp_core_rdpgfx_avc_gop_size(60, 3, true) == 300);
}

static void test_rdpgfx_video_quality_policy(void)
{
	const int64_t five_seconds = 5 * 1000000;
	const uint64_t three_mb_per_second = 3 * 1024 * 1024;

	assert(rf_rdp_core_update_video_quality_level(
		0,
		3,
		7 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		26000,
		60,
		0,
		0,
		true
	) == 1);
	assert(rf_rdp_core_update_video_quality_level(
		0,
		3,
		7 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		20000,
		60,
		0,
		0,
		true
	) == 1);
	assert(rf_rdp_core_update_video_quality_level(
		0,
		3,
		7 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		42000,
		60,
		0,
		0,
		true
	) == 1);
	assert(rf_rdp_core_update_video_quality_level(
			2,
			3,
			7 * 1024 * 1024,
			five_seconds,
			three_mb_per_second,
				30,
				42000,
				60,
				0,
				0,
				true
			) == 3);
	assert(rf_rdp_core_update_video_quality_level(
		2,
		3,
		25 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		12000,
		60,
		0,
		0,
		true
	) == 3);
	assert(rf_rdp_core_update_video_quality_level(
		1,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		true
	) == 0);
	assert(rf_rdp_core_update_video_quality_level(
		1,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		14000,
		60,
		0,
		0,
		true
	) == 1);
	assert(rf_rdp_core_update_video_quality_level(
		1,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		30,
		14000,
		60,
		0,
		0,
		true
	) == 0);
	assert(rf_rdp_core_update_video_quality_level(
		3,
		3,
		6 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		true
	) == 2);
	assert(rf_rdp_core_update_video_quality_level(
		2,
		3,
		20 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		26000,
		60,
		0,
		0,
		false
	) == 0);
	assert(rf_rdp_core_update_video_quality_level(
		0,
		3,
		2 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		8,
		4,
		true
	) == 1);
	assert(rf_rdp_core_update_video_quality_level(
		1,
		3,
		2 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		10,
		true
	) == 2);
	assert(rf_rdp_core_update_video_quality_level(
		1,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		8,
		10,
		true
	) == 2);
}

static void test_rdpgfx_video_quality_policy_uses_qoe_latency(void)
{
	const int64_t five_seconds = 5 * 1000000;
	const uint64_t three_mb_per_second = 3 * 1024 * 1024;

	assert(rf_rdp_core_update_video_quality_level_with_qoe(
		0,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		40,
		10,
		true
	) == 1);
	assert(rf_rdp_core_update_video_quality_level_with_qoe(
		1,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		10,
		80,
		true
	) == 2);
	assert(rf_rdp_core_update_video_quality_level_with_qoe(
		1,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		8,
		12,
		true
	) == 0);
	assert(rf_rdp_core_update_video_quality_level_with_qoe(
		2,
		3,
		20 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		26000,
		60,
		0,
		0,
		120,
		160,
		false
	) == 0);
}

static void test_rdpgfx_video_quality_policy_holds_during_cooldown(void)
{
	const int64_t five_seconds = 5 * 1000000;
	const int64_t ten_seconds = 10 * 1000000;
	const int64_t sixteen_seconds = 16 * 1000000;
	const uint64_t three_mb_per_second = 3 * 1024 * 1024;

	assert(rf_rdp_core_update_video_quality_level_stable(
		3,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		0,
		0,
		true,
		ten_seconds
	) == 3);
	assert(rf_rdp_core_update_video_quality_level_stable(
		3,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		60,
		0,
		0,
		0,
		0,
		true,
		sixteen_seconds
	) == 2);
	assert(rf_rdp_core_update_video_quality_level_stable(
		2,
		3,
		7 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		26000,
		60,
		0,
		0,
		0,
		0,
		true,
		ten_seconds
	) == 2);
	assert(rf_rdp_core_update_video_quality_level_stable(
		2,
		3,
		7 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		26000,
		60,
		0,
		0,
		0,
		0,
		true,
		sixteen_seconds
	) == 3);
}

static void test_rdpgfx_video_quality_policy_ignores_tiny_sample_windows(void)
{
	const int64_t five_seconds = 5 * 1000000;
	const int64_t no_recent_change = 0;
	const uint64_t three_mb_per_second = 3 * 1024 * 1024;

	assert(rf_rdp_core_update_video_quality_level_stable(
		3,
		3,
		256 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		7000,
		3,
		0,
		0,
		0,
		0,
		true,
		no_recent_change
	) == 3);
	assert(rf_rdp_core_update_video_quality_level_stable(
		2,
		3,
		8 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		42000,
		2,
		0,
		0,
		0,
		0,
		true,
		no_recent_change
	) == 2);
	assert(rf_rdp_core_update_video_quality_level_stable(
		2,
		3,
		8 * 1024 * 1024,
		five_seconds,
		three_mb_per_second,
		60,
		42000,
		30,
		0,
		0,
		0,
		0,
		true,
		no_recent_change
	) == 3);
}

static void test_rdpgfx_avc444_only_used_without_avc420_fallback(void)
{
	assert(rf_rdp_core_should_use_avc444(true, false, 0));
	assert(rf_rdp_core_should_use_avc444(true, false, 3));
	assert(!rf_rdp_core_should_use_avc444(true, true, 0));
	assert(!rf_rdp_core_should_use_avc444(true, true, 1));
	assert(!rf_rdp_core_should_use_avc444(false, true, 0));
	assert(!rf_rdp_core_should_use_avc444(false, false, 3));
}

static void test_rdpgfx_avc444_lc_stats_index(void)
{
	unsigned int index = 99;

	assert(rf_rdp_core_rdpgfx_avc444_lc_index(
		RF_RDP_GFX_AVC444_LC_BOTH,
		&index
	));
	assert(index == 0);
	assert(rf_rdp_core_rdpgfx_avc444_lc_index(
		RF_RDP_GFX_AVC444_LC_SINGLE,
		&index
	));
	assert(index == 1);
	assert(rf_rdp_core_rdpgfx_avc444_lc_index(
		RF_RDP_GFX_AVC444_LC_CHROMA,
		&index
	));
	assert(index == 2);
	index = 99;
	assert(!rf_rdp_core_rdpgfx_avc444_lc_index(3, &index));
	assert(index == 99);
	assert(!rf_rdp_core_rdpgfx_avc444_lc_index(
		RF_RDP_GFX_AVC444_LC_BOTH,
		NULL
	));
}

static void test_rdpgfx_avc444_chroma_cadence_policy(void)
{
	assert(!rf_rdp_core_should_defer_avc444_chroma(
		1, 0, false, true, true
	));
	assert(rf_rdp_core_should_defer_avc444_chroma(
		1, 1, false, true, true
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma(
		3, 1, false, true, true
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma(
		1, 3, true, true, true
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma(
		1, 3, false, false, true
	));
	assert(rf_rdp_core_should_defer_avc444_chroma(
		1, 2, false, true, true
	));
	assert(rf_rdp_core_should_defer_avc444_chroma(
		2, 2, false, true, true
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma(
		3, 2, false, true, true
	));
	assert(rf_rdp_core_should_defer_avc444_chroma(
		4, 3, false, true, true
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma(
		5, 3, false, true, true
	));
}

static void test_rdpgfx_avc444_chroma_cadence_uses_chroma_damage_ratio(void)
{
	assert(rf_rdp_core_should_defer_avc444_chroma_for_damage(
		4,
		3,
		false,
		true,
		true,
		1000,
		40
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		5,
		3,
		false,
		true,
		true,
		1000,
		40
	));

	assert(rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		3,
		false,
		true,
		true,
		1000,
		150
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		3,
		3,
		false,
		true,
		true,
		1000,
		150
	));

	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		3,
		false,
		true,
		true,
		1000,
		350
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		3,
		true,
		true,
		true,
		1000,
		40
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		0,
		false,
		true,
		true,
		1000,
		40
	));
	assert(rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		1,
		false,
		true,
		true,
		1000,
		40
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		3,
		1,
		false,
		true,
		true,
		1000,
		40
	));
	assert(rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		1,
		false,
		true,
		true,
		1000,
		150
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		2,
		1,
		false,
		true,
		true,
		1000,
		150
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		1,
		false,
		true,
		true,
		1000,
		350
	));
	assert(!rf_rdp_core_should_defer_avc444_chroma_for_damage(
		1,
		3,
		false,
		true,
		true,
		0,
		40
	));
}

int main(void)
{
	test_parse_client_info_shape();
	test_write_license_valid_client();
	test_write_demand_active();
	test_write_finalization_data_pdus();
	test_parse_client_finalization_data();
	test_parse_confirm_active_capabilities();
	test_write_bitmap_update();
	test_write_compressed_bitmap_update_uses_rle24_header();
	test_write_surface_bits_fastpath_update();
	test_select_graphics_mode();
	test_defer_legacy_graphics_until_rdpgfx_caps_resolve();
	test_convert_rgba_rect_to_bgrx_bottom_up();
	test_convert_rgba_rect_to_bgrx_top_down();
	test_convert_rgba_rect_to_bgr_bottom_up();
	test_clip_update_rect_forces_full_frame();
	test_rgba_rect_source_points_at_damage_origin();
	test_expand_update_rect_keeps_rect_inside_frame();
	test_full_surface_rect_ignores_damage_rect();
	test_frame_pacing_policy();
	test_frame_pacing_accepts_small_source_jitter();
	test_frame_pacing_limits_fast_source();
	test_adaptive_bitmap_fps_policy();
	test_rdpgfx_quality_headroom_holds_fallback_fps_limit();
	test_rdpgfx_ack_fps_policy_keeps_fps_until_queues_are_critical();
	test_avc444_delta_policy_skips_large_damage();
	test_rdpgfx_avc_quality_parameters();
	test_rdpgfx_video_quality_policy();
	test_rdpgfx_video_quality_policy_uses_qoe_latency();
	test_rdpgfx_video_quality_policy_holds_during_cooldown();
	test_rdpgfx_video_quality_policy_ignores_tiny_sample_windows();
	test_rdpgfx_avc444_only_used_without_avc420_fallback();
	test_rdpgfx_avc444_lc_stats_index();
	test_rdpgfx_avc444_chroma_cadence_policy();
	test_rdpgfx_avc444_chroma_cadence_uses_chroma_damage_ratio();
	return 0;
}
