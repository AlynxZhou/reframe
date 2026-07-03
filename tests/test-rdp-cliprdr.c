#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>

#include "rf-rdp-cliprdr.h"

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

static void assert_header(
	const uint8_t *data,
	size_t length,
	uint16_t msg_type,
	uint16_t msg_flags,
	uint32_t data_length
)
{
	struct rf_rdp_cliprdr_pdu pdu = { 0 };

	assert(length == RF_RDP_CLIPRDR_HEADER_SIZE + data_length);
	assert(read_u16_le(data) == msg_type);
	assert(read_u16_le(data + 2) == msg_flags);
	assert(read_u32_le(data + 4) == data_length);
	assert(rf_rdp_cliprdr_parse_pdu(data, length, &pdu));
	assert(pdu.msg_type == msg_type);
	assert(pdu.msg_flags == msg_flags);
	assert(pdu.data_offset == RF_RDP_CLIPRDR_HEADER_SIZE);
	assert(pdu.data_length == data_length);
}

static void test_write_caps(void)
{
	uint8_t out[64] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_caps(out, sizeof(out));

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_CLIP_CAPS,
		0,
		16
	);
	assert(read_u16_le(out + 8) == 1);
	assert(read_u16_le(out + 10) == 0);
	assert(read_u16_le(out + 12) == RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL);
	assert(read_u16_le(out + 14) == RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL_LEN);
	assert(read_u32_le(out + 16) == RF_RDP_CLIPRDR_CAPS_VERSION_2);
	assert(read_u32_le(out + 20) == 0);
}

static void test_write_monitor_ready(void)
{
	uint8_t out[16] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_monitor_ready(
		out,
		sizeof(out)
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_MONITOR_READY,
		0,
		0
	);
}

static void test_write_format_list(void)
{
	uint8_t out[64] = { 0 };
	struct rf_rdp_cliprdr_format_list formats = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_list(out, sizeof(out));

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_LIST,
		0,
		36
	);
	assert(read_u32_le(out + 8) == RF_RDP_CLIPRDR_CF_UNICODETEXT);
	for (size_t i = 12; i < length; ++i)
		assert(out[i] == 0);
	assert(rf_rdp_cliprdr_parse_format_list(
		out + RF_RDP_CLIPRDR_HEADER_SIZE,
		36,
		&formats
	));
	assert(formats.unicode_text);
}

static void test_write_responses_and_request(void)
{
	uint8_t out[32] = { 0 };
	size_t length = rf_rdp_cliprdr_write_format_list_response(
		out,
		sizeof(out),
		true
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_LIST_RESPONSE,
		RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		0
	);

	memset(out, 0, sizeof(out));
	length = rf_rdp_cliprdr_write_format_data_request(
		out,
		sizeof(out),
		RF_RDP_CLIPRDR_CF_UNICODETEXT
	);
	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_DATA_REQUEST,
		0,
		4
	);
	assert(read_u32_le(out + 8) == RF_RDP_CLIPRDR_CF_UNICODETEXT);

	uint32_t format_id = 0;
	assert(rf_rdp_cliprdr_parse_format_data_request(
		out + RF_RDP_CLIPRDR_HEADER_SIZE,
		4,
		&format_id
	));
	assert(format_id == RF_RDP_CLIPRDR_CF_UNICODETEXT);
}

static void test_write_and_parse_unicode_text_response(void)
{
	uint8_t out[64] = { 0 };
	g_autofree char *text = NULL;
	const size_t length = rf_rdp_cliprdr_write_format_data_response_text(
		out,
		sizeof(out),
		"A\xce\xbb"
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		6
	);
	assert(out[8] == 'A');
	assert(out[9] == 0);
	assert(out[10] == 0xbb);
	assert(out[11] == 0x03);
	assert(out[12] == 0);
	assert(out[13] == 0);

	text = rf_rdp_cliprdr_parse_format_data_response_text(
		out + RF_RDP_CLIPRDR_HEADER_SIZE,
		6,
		RF_RDP_CLIPRDR_CB_RESPONSE_OK
	);
	assert(g_strcmp0(text, "A\xce\xbb") == 0);
}

static void test_parse_long_format_list(void)
{
	const uint8_t pdu[] = {
		0x0d, 0x00, 0x00, 0x00,
		'T', 0x00, 'e', 0x00, 'x', 0x00, 't', 0x00,
		0x00, 0x00
	};
	struct rf_rdp_cliprdr_format_list formats = { 0 };

	assert(rf_rdp_cliprdr_parse_format_list(
		pdu,
		sizeof(pdu),
		&formats
	));
	assert(formats.unicode_text);
}

int main(void)
{
	test_write_caps();
	test_write_monitor_ready();
	test_write_format_list();
	test_write_responses_and_request();
	test_write_and_parse_unicode_text_response();
	test_parse_long_format_list();
	return 0;
}
