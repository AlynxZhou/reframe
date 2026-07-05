#include <assert.h>
#include <stdint.h>
#include <string.h>

#ifdef RF_HAVE_RDP_CLIPRDR_PNG
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif
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

static void write_u32_le(uint8_t *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = (value >> 24) & 0xff;
}

static GByteArray *make_channel_chunk(
	const uint8_t *payload,
	size_t payload_length,
	uint32_t total_length,
	uint32_t flags
)
{
	GByteArray *chunk = g_byte_array_sized_new(8 + payload_length);
	uint8_t header[8] = { 0 };

	write_u32_le(header, total_length);
	write_u32_le(header + 4, flags);
	g_byte_array_append(chunk, header, sizeof(header));
	if (payload_length > 0)
		g_byte_array_append(chunk, payload, payload_length);
	return chunk;
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
	assert(read_u32_le(out + 20) == RF_RDP_CLIPRDR_CAPS_USE_LONG_FORMAT_NAMES);
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
	uint8_t out[192] = { 0 };
	const uint32_t expected_formats[] = {
		RF_RDP_CLIPRDR_CF_UNICODETEXT,
		RF_RDP_CLIPRDR_CF_TEXT,
		RF_RDP_CLIPRDR_CF_OEMTEXT,
		RF_RDP_CLIPRDR_CF_LOCALE
	};
	struct rf_rdp_cliprdr_format_list formats = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_list(out, sizeof(out));

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_LIST,
		0,
		144
	);
	for (size_t i = 0; i < G_N_ELEMENTS(expected_formats); ++i) {
		const size_t offset = RF_RDP_CLIPRDR_HEADER_SIZE + i * 36;

		assert(read_u32_le(out + offset) == expected_formats[i]);
		for (size_t j = 4; j < 36; ++j)
			assert(out[offset + j] == 0);
	}
	assert(rf_rdp_cliprdr_parse_format_list(
		out + RF_RDP_CLIPRDR_HEADER_SIZE,
		144,
		&formats
	));
	assert(formats.unicode_text);
}

static void test_write_rich_format_list(void)
{
	uint8_t out[384] = { 0 };
	struct rf_rdp_cliprdr_format_list request = {
		.unicode_text = true,
		.text = true,
		.locale = true,
		.html = true,
		.dib = true,
		.dibv5 = true,
		.html_format_id = RF_RDP_CLIPRDR_FORMAT_HTML
	};
	struct rf_rdp_cliprdr_format_list parsed = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_list_for_formats(
		out,
		sizeof(out),
		&request
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_LIST,
		0,
		length - RF_RDP_CLIPRDR_HEADER_SIZE
	);
	assert(rf_rdp_cliprdr_parse_format_list(
		out + RF_RDP_CLIPRDR_HEADER_SIZE,
		length - RF_RDP_CLIPRDR_HEADER_SIZE,
		&parsed
	));
	assert(parsed.unicode_text);
	assert(parsed.text);
	assert(parsed.locale);
	assert(parsed.html);
	assert(parsed.html_format_id == RF_RDP_CLIPRDR_FORMAT_HTML);
	assert(parsed.dib);
	assert(parsed.dibv5);
}

static void test_parse_long_html_format_list(void)
{
	const uint8_t pdu[] = {
		0x10, 0xd0, 0x00, 0x00,
		'H', 0x00, 'T', 0x00, 'M', 0x00, 'L', 0x00,
		' ', 0x00, 'F', 0x00, 'o', 0x00, 'r', 0x00,
		'm', 0x00, 'a', 0x00, 't', 0x00, 0x00, 0x00,
		0x11, 0x00, 0x00, 0x00,
		0x00, 0x00
	};
	struct rf_rdp_cliprdr_format_list formats = { 0 };

	assert(rf_rdp_cliprdr_parse_format_list(
		pdu,
		sizeof(pdu),
		&formats
	));
	assert(formats.html);
	assert(formats.html_format_id == RF_RDP_CLIPRDR_FORMAT_HTML);
	assert(formats.dibv5);
}

static void test_parse_long_png_format_list(void)
{
	const uint8_t pdu[] = {
		0x35, 0xd0, 0x00, 0x00,
		'i', 0x00, 'm', 0x00, 'a', 0x00, 'g', 0x00,
		'e', 0x00, '/', 0x00, 'p', 0x00, 'n', 0x00,
		'g', 0x00, 0x00, 0x00,
		0x08, 0x00, 0x00, 0x00,
		0x00, 0x00
	};
	struct rf_rdp_cliprdr_format_list formats = { 0 };

	assert(rf_rdp_cliprdr_parse_format_list(
		pdu,
		sizeof(pdu),
		&formats
	));
	assert(formats.png);
	assert(formats.png_format_id == 0xd035);
	assert(formats.dib);
}

static void test_parse_long_image_format_list(void)
{
	const char *names[] = {
		RF_RDP_CLIPRDR_TIFF_FORMAT_NAME,
		RF_RDP_CLIPRDR_JPEG_FORMAT_NAME,
		RF_RDP_CLIPRDR_WEBP_FORMAT_NAME,
		RF_RDP_CLIPRDR_BMP_FORMAT_NAME
	};
	const uint32_t ids[] = { 0xd036, 0xd037, 0xd038, 0xd039 };
	g_autoptr(GByteArray) pdu = g_byte_array_new();
	struct rf_rdp_cliprdr_format_list formats = { 0 };

	for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) {
		const char *name = names[i];
		uint8_t id[4] = {
			ids[i] & 0xff,
			(ids[i] >> 8) & 0xff,
			(ids[i] >> 16) & 0xff,
			(ids[i] >> 24) & 0xff
		};

		g_byte_array_append(pdu, id, sizeof(id));
		for (size_t j = 0; name[j] != '\0'; ++j) {
			const uint8_t ch[2] = { name[j], 0 };

			g_byte_array_append(pdu, ch, sizeof(ch));
		}
		g_byte_array_append(pdu, (const uint8_t *)"\0\0", 2);
	}

	assert(rf_rdp_cliprdr_parse_format_list(
		pdu->data,
		pdu->len,
		&formats
	));
	assert(formats.tiff);
	assert(formats.tiff_format_id == 0xd036);
	assert(formats.jpeg);
	assert(formats.jpeg_format_id == 0xd037);
	assert(formats.webp);
	assert(formats.webp_format_id == 0xd038);
	assert(formats.bmp);
	assert(formats.bmp_format_id == 0xd039);
}

static void test_prefers_png_request_format_when_available(void)
{
	struct rf_rdp_cliprdr_format_list formats = {
		.dib = true,
		.dibv5 = true,
		.png = true,
		.png_format_id = 0xd035
	};

#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	assert(rf_rdp_cliprdr_choose_request_format(&formats) == 0xd035);
#else
	assert(rf_rdp_cliprdr_choose_request_format(&formats) ==
	       RF_RDP_CLIPRDR_CF_DIBV5);
#endif
}

static void test_prefers_html_request_for_image_formats(void)
{
	struct rf_rdp_cliprdr_format_list formats = {
		.html = true,
		.dib = true,
		.dibv5 = true,
		.png = true,
		.html_format_id = RF_RDP_CLIPRDR_FORMAT_HTML,
		.png_format_id = 0xd035
	};

	assert(rf_rdp_cliprdr_choose_request_format(&formats) ==
	       RF_RDP_CLIPRDR_FORMAT_HTML);
}

extern uint32_t rf_rdp_cliprdr_choose_request_format_after(
	const struct rf_rdp_cliprdr_format_list *formats,
	uint32_t previous_format_id
);

static void test_falls_back_after_failed_html_image_request(void)
{
	struct rf_rdp_cliprdr_format_list formats = {
		.unicode_text = true,
		.html = true,
		.dib = true,
		.dibv5 = true,
		.cf_tiff = true,
		.png = true,
		.tiff = true,
		.jpeg = true,
		.webp = true,
		.bmp = true,
		.html_format_id = RF_RDP_CLIPRDR_FORMAT_HTML,
		.png_format_id = 0xd135,
		.tiff_format_id = 0xd136,
		.jpeg_format_id = 0xd137,
		.webp_format_id = 0xd138,
		.bmp_format_id = 0xd139
	};

	assert(rf_rdp_cliprdr_choose_request_format(&formats) ==
	       RF_RDP_CLIPRDR_FORMAT_HTML);
#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	assert(rf_rdp_cliprdr_choose_request_format_after(
		       &formats,
		       RF_RDP_CLIPRDR_FORMAT_HTML
	       ) == 0xd138);
	assert(rf_rdp_cliprdr_choose_request_format_after(&formats, 0xd138) ==
	       0xd137);
	assert(rf_rdp_cliprdr_choose_request_format_after(&formats, 0xd137) ==
	       0xd136);
	assert(rf_rdp_cliprdr_choose_request_format_after(&formats, 0xd136) ==
	       0xd135);
	assert(rf_rdp_cliprdr_choose_request_format_after(&formats, 0xd135) ==
	       0xd139);
	assert(rf_rdp_cliprdr_choose_request_format_after(&formats, 0xd139) ==
	       RF_RDP_CLIPRDR_CF_TIFF);
#else
	assert(rf_rdp_cliprdr_choose_request_format_after(
		       &formats,
		       RF_RDP_CLIPRDR_FORMAT_HTML
	       ) == 0xd136);
	assert(rf_rdp_cliprdr_choose_request_format_after(&formats, 0xd136) ==
	       RF_RDP_CLIPRDR_CF_TIFF);
#endif
	assert(rf_rdp_cliprdr_choose_request_format_after(
		       &formats,
		       RF_RDP_CLIPRDR_CF_TIFF
	       ) == RF_RDP_CLIPRDR_CF_DIBV5);
	assert(rf_rdp_cliprdr_choose_request_format_after(
		       &formats,
		       RF_RDP_CLIPRDR_CF_DIBV5
	       ) == RF_RDP_CLIPRDR_CF_DIB);
	assert(rf_rdp_cliprdr_choose_request_format_after(
		       &formats,
		       RF_RDP_CLIPRDR_CF_DIB
	       ) == RF_RDP_CLIPRDR_CF_UNICODETEXT);
	assert(rf_rdp_cliprdr_choose_request_format_after(
		       &formats,
		       RF_RDP_CLIPRDR_CF_UNICODETEXT
	       ) == 0);
}

static void test_prefers_compressed_image_request_format_when_available(void)
{
	struct rf_rdp_cliprdr_format_list formats = {
		.dib = true,
		.dibv5 = true,
		.cf_tiff = true,
		.jpeg = true,
		.webp = true,
		.tiff = true,
		.bmp = true,
		.jpeg_format_id = 0xd037,
		.webp_format_id = 0xd038,
		.tiff_format_id = 0xd036,
		.bmp_format_id = 0xd039
	};

#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	assert(rf_rdp_cliprdr_choose_request_format(&formats) == 0xd038);
	formats.webp = false;
	assert(rf_rdp_cliprdr_choose_request_format(&formats) == 0xd037);
	formats.jpeg = false;
	assert(rf_rdp_cliprdr_choose_request_format(&formats) == 0xd036);
	formats.tiff = false;
	assert(rf_rdp_cliprdr_choose_request_format(&formats) == 0xd039);
	formats.bmp = false;
#else
	formats.webp = false;
	formats.jpeg = false;
	formats.tiff = false;
	formats.bmp = false;
#endif
	assert(rf_rdp_cliprdr_choose_request_format(&formats) ==
	       RF_RDP_CLIPRDR_CF_TIFF);
}

static void test_server_image_formats_use_standard_dib_only(void)
{
	struct rf_rdp_cliprdr_format_list formats = { 0 };
	uint8_t out[256] = { 0 };
	struct rf_rdp_cliprdr_format_list parsed = { 0 };
	size_t length = 0;

	rf_rdp_cliprdr_set_server_image_formats(&formats);

	assert(formats.dib);
	assert(formats.dibv5);
	assert(!formats.cf_tiff);
	assert(!formats.png);
	assert(!formats.tiff);
	assert(!formats.jpeg);
	assert(!formats.webp);
	assert(!formats.bmp);

	length = rf_rdp_cliprdr_write_format_list_for_formats(
		out,
		sizeof(out),
		&formats
	);
	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_LIST,
		0,
		length - RF_RDP_CLIPRDR_HEADER_SIZE
	);
	assert(rf_rdp_cliprdr_parse_format_list(
		out + RF_RDP_CLIPRDR_HEADER_SIZE,
		length - RF_RDP_CLIPRDR_HEADER_SIZE,
		&parsed
	));
	assert(parsed.dib);
	assert(parsed.dibv5);
	assert(!parsed.cf_tiff);
	assert(!parsed.png);
	assert(!parsed.tiff);
	assert(!parsed.jpeg);
	assert(!parsed.webp);
	assert(!parsed.bmp);
}

static void test_parse_ambiguous_long_format_list(void)
{
	const uint32_t format_ids[] = {
		RF_RDP_CLIPRDR_CF_UNICODETEXT,
		RF_RDP_CLIPRDR_CF_TEXT,
		RF_RDP_CLIPRDR_CF_OEMTEXT,
		RF_RDP_CLIPRDR_CF_LOCALE,
		RF_RDP_CLIPRDR_CF_DIB,
		RF_RDP_CLIPRDR_CF_DIBV5
	};
	uint8_t pdu[36] = { 0 };
	struct rf_rdp_cliprdr_format_list formats = { 0 };

	for (size_t i = 0; i < G_N_ELEMENTS(format_ids); ++i) {
		pdu[i * 6 + 0] = format_ids[i] & 0xff;
		pdu[i * 6 + 1] = (format_ids[i] >> 8) & 0xff;
		pdu[i * 6 + 2] = (format_ids[i] >> 16) & 0xff;
		pdu[i * 6 + 3] = (format_ids[i] >> 24) & 0xff;
	}

	assert(rf_rdp_cliprdr_parse_format_list(
		pdu,
		sizeof(pdu),
		&formats
	));
	assert(formats.unicode_text);
	assert(formats.text);
	assert(formats.oem_text);
	assert(formats.locale);
	assert(formats.dib);
	assert(formats.dibv5);
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

static void test_write_utf8_text_response(void)
{
	uint8_t out[64] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_data_response_utf8_text(
		out,
		sizeof(out),
		"A\xce\xbb"
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		4
	);
	assert(memcmp(out + RF_RDP_CLIPRDR_HEADER_SIZE, "A\xce\xbb", 4) == 0);
}

static void test_write_locale_response(void)
{
	uint8_t out[16] = { 0 };
	const size_t length = rf_rdp_cliprdr_write_format_data_response_locale(
		out,
		sizeof(out),
		0x00000409
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		4
	);
	assert(read_u32_le(out + RF_RDP_CLIPRDR_HEADER_SIZE) == 0x00000409);
}

static void test_write_bytes_response(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t payload[] = { 1, 2, 3, 4, 5 };
	const size_t length = rf_rdp_cliprdr_write_format_data_response_bytes(
		out,
		sizeof(out),
		payload,
		sizeof(payload)
	);

	assert_header(
		out,
		length,
		RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		sizeof(payload)
	);
	assert(memcmp(out + RF_RDP_CLIPRDR_HEADER_SIZE, payload, sizeof(payload)) == 0);
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

static void test_html_format_wrap_and_unwrap(void)
{
	const char fragment[] = "<p>Hello <b>RDP</b></p>";
	g_autoptr(GByteArray) wrapped = rf_rdp_cliprdr_html_format_wrap(
		(const uint8_t *)fragment,
		strlen(fragment)
	);
	g_autoptr(GByteArray) unwrapped = NULL;

	assert(wrapped != NULL);
	assert(memmem(wrapped->data, wrapped->len, "StartHTML:", 10) != NULL);
	assert(memmem(wrapped->data, wrapped->len, "StartFragment:", 14) != NULL);
	assert(memmem(wrapped->data, wrapped->len, fragment, strlen(fragment)) != NULL);

	unwrapped = rf_rdp_cliprdr_html_format_unwrap(wrapped->data, wrapped->len);
	assert(unwrapped != NULL);
	assert(unwrapped->len == strlen(fragment));
	assert(memcmp(unwrapped->data, fragment, strlen(fragment)) == 0);
}

static void test_html_unwrap_accepts_raw_html(void)
{
	const char html[] = "<span>raw</span>";
	g_autoptr(GByteArray) unwrapped = rf_rdp_cliprdr_html_format_unwrap(
		(const uint8_t *)html,
		strlen(html)
	);

	assert(unwrapped != NULL);
	assert(unwrapped->len == strlen(html));
	assert(memcmp(unwrapped->data, html, strlen(html)) == 0);
}

static void test_html_fragment_to_text(void)
{
	const char html[] =
		"<p>Hello <b>RDP</b>&nbsp;&amp; &#x4e16;&#30028;<br>next</p>";
	g_autofree char *text = rf_rdp_cliprdr_html_fragment_to_text(
		(const uint8_t *)html,
		strlen(html)
	);

	assert(g_strcmp0(text, "Hello RDP & \xe4\xb8\x96\xe7\x95\x8c\nnext") == 0);
}

static void test_reassembles_fragmented_static_channel_pdu(void)
{
	struct rf_rdp_cliprdr_channel_reassembler reassembler = { 0 };
	uint8_t payload[3464] = { 0 };
	g_autoptr(GByteArray) first_chunk = NULL;
	g_autoptr(GByteArray) middle_chunk = NULL;
	g_autoptr(GByteArray) last_chunk = NULL;
	g_autoptr(GByteArray) complete = NULL;

	for (size_t i = 0; i < sizeof(payload); ++i)
		payload[i] = (uint8_t)i;

	first_chunk = make_channel_chunk(
		payload,
		1600,
		sizeof(payload),
		RF_RDP_CLIPRDR_CHANNEL_FLAG_FIRST |
			RF_RDP_CLIPRDR_CHANNEL_FLAG_SHOW_PROTOCOL
	);
	middle_chunk = make_channel_chunk(
		payload + 1600,
		1600,
		sizeof(payload),
		RF_RDP_CLIPRDR_CHANNEL_FLAG_SHOW_PROTOCOL
	);
	last_chunk = make_channel_chunk(
		payload + 3200,
		sizeof(payload) - 3200,
		sizeof(payload),
		RF_RDP_CLIPRDR_CHANNEL_FLAG_LAST |
			RF_RDP_CLIPRDR_CHANNEL_FLAG_SHOW_PROTOCOL
	);

	assert(rf_rdp_cliprdr_channel_reassembler_add(
		&reassembler,
		first_chunk->data,
		first_chunk->len,
		&complete
	));
	assert(complete == NULL);
	assert(rf_rdp_cliprdr_channel_reassembler_add(
		&reassembler,
		middle_chunk->data,
		middle_chunk->len,
		&complete
	));
	assert(complete == NULL);
	assert(rf_rdp_cliprdr_channel_reassembler_add(
		&reassembler,
		last_chunk->data,
		last_chunk->len,
		&complete
	));
	assert(complete != NULL);
	assert(complete->len == sizeof(payload));
	assert(memcmp(complete->data, payload, sizeof(payload)) == 0);
	assert(reassembler.payload == NULL);
	assert(reassembler.total_length == 0);
	rf_rdp_cliprdr_channel_reassembler_clear(&reassembler);
}

static void test_rgba_to_dib_and_back(void)
{
	const uint8_t rgba[] = {
		0x10, 0x20, 0x30, 0xff,
		0x40, 0x50, 0x60, 0x80,
		0x70, 0x80, 0x90, 0x40,
		0xa0, 0xb0, 0xc0, 0x00
	};
	uint32_t width = 0;
	uint32_t height = 0;
	size_t stride = 0;
	g_autoptr(GByteArray) dib = rf_rdp_cliprdr_rgba_to_dib(
		rgba,
		sizeof(rgba),
		2,
		2,
		2 * 4,
		false
	);
	g_autoptr(GByteArray) roundtrip = NULL;

	assert(dib != NULL);
	assert(read_u32_le(dib->data) == 40);
	assert(read_u32_le(dib->data + 4) == 2);
	assert(read_u32_le(dib->data + 8) == (uint32_t)-2);
	assert(read_u16_le(dib->data + 14) == 32);
	assert(read_u32_le(dib->data + 16) == 0);
	assert(dib->data[40] == 0x30);
	assert(dib->data[41] == 0x20);
	assert(dib->data[42] == 0x10);
	assert(dib->data[43] == 0xff);

	roundtrip = rf_rdp_cliprdr_dib_to_rgba(
		dib->data,
		dib->len,
		&width,
		&height,
		&stride
	);
	assert(roundtrip != NULL);
	assert(width == 2);
	assert(height == 2);
	assert(stride == 2 * 4);
	assert(roundtrip->len == sizeof(rgba));
	assert(memcmp(roundtrip->data, rgba, sizeof(rgba)) == 0);
}

static void test_rgba_to_dibv5_header(void)
{
	const uint8_t rgba[] = { 0x01, 0x02, 0x03, 0x04 };
	g_autoptr(GByteArray) dib = rf_rdp_cliprdr_rgba_to_dib(
		rgba,
		sizeof(rgba),
		1,
		1,
		4,
		true
	);

	assert(dib != NULL);
	assert(read_u32_le(dib->data) == 124);
	assert(read_u32_le(dib->data + 4) == 1);
	assert(read_u32_le(dib->data + 8) == (uint32_t)-1);
	assert(read_u16_le(dib->data + 14) == 32);
	assert(read_u32_le(dib->data + 16) == 3);
	assert(read_u32_le(dib->data + 40) == 0x00ff0000);
	assert(read_u32_le(dib->data + 44) == 0x0000ff00);
	assert(read_u32_le(dib->data + 48) == 0x000000ff);
	assert(read_u32_le(dib->data + 52) == 0xff000000);
	assert(dib->data[124] == 0x03);
	assert(dib->data[125] == 0x02);
	assert(dib->data[126] == 0x01);
	assert(dib->data[127] == 0x04);
}

static void test_png_to_rgba(void)
{
#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0xff, 0x00, 0x80
	};
	g_autoptr(GBytes) pixels = g_bytes_new(rgba, sizeof(rgba));
	g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_bytes(
		pixels,
		GDK_COLORSPACE_RGB,
		TRUE,
		8,
		2,
		1,
		2 * 4
	);
	g_autofree gchar *png = NULL;
	gsize png_length = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	size_t stride = 0;
	g_autoptr(GByteArray) decoded = NULL;

	assert(pixbuf != NULL);
	assert(gdk_pixbuf_save_to_buffer(
		pixbuf,
		&png,
		&png_length,
		"png",
		NULL,
		NULL
	));

	decoded = rf_rdp_cliprdr_png_to_rgba(
		(const uint8_t *)png,
		png_length,
		&width,
		&height,
		&stride
	);
	assert(decoded != NULL);
	assert(width == 2);
	assert(height == 1);
	assert(stride == 2 * 4);
	assert(decoded->len == sizeof(rgba));
	assert(memcmp(decoded->data, rgba, sizeof(rgba)) == 0);
#endif
}

static void test_html_image_to_rgba(void)
{
#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0x00, 0xff, 0x80
	};
	g_autoptr(GBytes) pixels = g_bytes_new(rgba, sizeof(rgba));
	g_autoptr(GdkPixbuf) pixbuf = gdk_pixbuf_new_from_bytes(
		pixels,
		GDK_COLORSPACE_RGB,
		TRUE,
		8,
		2,
		1,
		2 * 4
	);
	g_autofree gchar *png = NULL;
	g_autofree gchar *base64 = NULL;
	g_autofree gchar *fragment = NULL;
	g_autoptr(GByteArray) html = NULL;
	gsize png_length = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	size_t stride = 0;
	g_autoptr(GByteArray) decoded = NULL;

	assert(pixbuf != NULL);
	assert(gdk_pixbuf_save_to_buffer(
		pixbuf,
		&png,
		&png_length,
		"png",
		NULL,
		NULL
	));
	base64 = g_base64_encode((const guchar *)png, png_length);
	fragment = g_strdup_printf(
		"<body><img alt=\"FreeRDP clipboard image\" "
		"src=\"data:image/png;base64,%s\"/></body>",
		base64
	);
	html = rf_rdp_cliprdr_html_format_wrap(
		(const uint8_t *)fragment,
		strlen(fragment)
	);
	assert(html != NULL);

	decoded = rf_rdp_cliprdr_html_image_to_rgba(
		html->data,
		html->len,
		&width,
		&height,
		&stride
	);
	assert(decoded != NULL);
	assert(width == 2);
	assert(height == 1);
	assert(stride == 2 * 4);
	assert(decoded->len == sizeof(rgba));
	assert(memcmp(decoded->data, rgba, sizeof(rgba)) == 0);
#endif
}

static void test_html_tiff_image_to_rgba(void)
{
#if defined(RF_HAVE_RDP_CLIPRDR_PNG) || defined(RF_HAVE_RDP_CLIPRDR_TIFF)
	const char tiff_base64[] =
		"TU0AKgAAAAgADAEAAAQAAAABAAAAAQEBAAQAAAABAAAAAQECAAMAAAAD"
		"AAAAngEDAAMAAAABAAEAAAEGAAMAAAABAAIAAAERAAQAAAABAAAAqgES"
		"AAMAAAABAAEAAAEVAAMAAAABAAMAAAEWAAQAAAABAAAAAQEXAAQAAAAB"
		"AAAAAwEcAAMAAAABAAEAAAFTAAMAAAADAAAApAAAAAAACAAIAAgAAQAB"
		"AAH/AAA=";
	g_autofree guchar *tiff = NULL;
	g_autofree gchar *fragment = NULL;
	g_autoptr(GByteArray) html = NULL;
	gsize tiff_length = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	size_t stride = 0;
	g_autoptr(GByteArray) decoded = NULL;

	tiff = g_base64_decode(tiff_base64, &tiff_length);
	assert(tiff != NULL);
	assert(tiff_length > 0);
	fragment = g_strdup_printf(
		"<body><img alt=\"FreeRDP clipboard image\" "
		"src=\"data:image/tiff;base64,%s\"/></body>",
		tiff_base64
	);
	html = rf_rdp_cliprdr_html_format_wrap(
		(const uint8_t *)fragment,
		strlen(fragment)
	);
	assert(html != NULL);

	decoded = rf_rdp_cliprdr_html_image_to_rgba(
		html->data,
		html->len,
		&width,
		&height,
		&stride
	);
	assert(decoded != NULL);
	assert(width == 1);
	assert(height == 1);
	assert(stride == 4);
	assert(decoded->len == 4);
	assert(decoded->data[0] == 0xff);
	assert(decoded->data[1] == 0x00);
	assert(decoded->data[2] == 0x00);
	assert(decoded->data[3] == 0xff);
#endif
}

static void test_rgba_to_image_formats(void)
{
#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0x00, 0xff, 0xff
	};
	const struct {
		const char *name;
		const uint8_t *signature;
		size_t signature_length;
	} cases[] = {
		{
			RF_RDP_CLIPRDR_PNG_FORMAT_NAME,
			(const uint8_t *)"\x89PNG\r\n\x1a\n",
			8
		},
		{
			RF_RDP_CLIPRDR_TIFF_FORMAT_NAME,
			(const uint8_t *)"II",
			2
		},
		{
			RF_RDP_CLIPRDR_JPEG_FORMAT_NAME,
			(const uint8_t *)"\xff\xd8",
			2
		},
		{
			RF_RDP_CLIPRDR_WEBP_FORMAT_NAME,
			(const uint8_t *)"RIFF",
			4
		},
		{
			RF_RDP_CLIPRDR_BMP_FORMAT_NAME,
			(const uint8_t *)"BM",
			2
		}
	};

	for (size_t i = 0; i < G_N_ELEMENTS(cases); ++i) {
		uint32_t width = 0;
		uint32_t height = 0;
		size_t stride = 0;
		g_autoptr(GByteArray) encoded = rf_rdp_cliprdr_rgba_to_image_format(
			rgba,
			sizeof(rgba),
			2,
			1,
			2 * 4,
			cases[i].name
		);
		g_autoptr(GByteArray) decoded = NULL;

		assert(encoded != NULL);
		assert(encoded->len > cases[i].signature_length);
		assert(memcmp(
			       encoded->data,
			       cases[i].signature,
			       cases[i].signature_length
		       ) == 0);

		decoded = rf_rdp_cliprdr_image_format_to_rgba(
			encoded->data,
			encoded->len,
			&width,
			&height,
			&stride
		);
		assert(decoded != NULL);
		assert(width == 2);
		assert(height == 1);
		assert(stride == 2 * 4);
		assert(decoded->len == sizeof(rgba));
	}
#endif
}

int main(void)
{
	test_write_caps();
	test_write_monitor_ready();
	test_write_format_list();
	test_write_rich_format_list();
	test_parse_long_html_format_list();
	test_parse_long_png_format_list();
	test_parse_long_image_format_list();
	test_prefers_png_request_format_when_available();
	test_prefers_html_request_for_image_formats();
	test_falls_back_after_failed_html_image_request();
	test_prefers_compressed_image_request_format_when_available();
	test_server_image_formats_use_standard_dib_only();
	test_parse_ambiguous_long_format_list();
	test_write_responses_and_request();
	test_write_and_parse_unicode_text_response();
	test_write_utf8_text_response();
	test_write_locale_response();
	test_write_bytes_response();
	test_parse_long_format_list();
	test_html_format_wrap_and_unwrap();
	test_html_unwrap_accepts_raw_html();
	test_html_fragment_to_text();
	test_reassembles_fragmented_static_channel_pdu();
	test_rgba_to_dib_and_back();
	test_rgba_to_dibv5_header();
	test_png_to_rgba();
	test_html_image_to_rgba();
	test_html_tiff_image_to_rgba();
	test_rgba_to_image_formats();
	return 0;
}
