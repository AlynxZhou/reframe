#ifndef __RF_RDP_CLIPRDR_H__
#define __RF_RDP_CLIPRDR_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>

#define RF_RDP_CLIPRDR_CHANNEL_NAME "cliprdr"
#define RF_RDP_CLIPRDR_HEADER_SIZE 8u

#define RF_RDP_CLIPRDR_CB_MONITOR_READY 0x0001u
#define RF_RDP_CLIPRDR_CB_FORMAT_LIST 0x0002u
#define RF_RDP_CLIPRDR_CB_FORMAT_LIST_RESPONSE 0x0003u
#define RF_RDP_CLIPRDR_CB_FORMAT_DATA_REQUEST 0x0004u
#define RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE 0x0005u
#define RF_RDP_CLIPRDR_CB_CLIP_CAPS 0x0007u

#define RF_RDP_CLIPRDR_CB_RESPONSE_OK 0x0001u
#define RF_RDP_CLIPRDR_CB_RESPONSE_FAIL 0x0002u

#define RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL 0x0001u
#define RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL_LEN 12u
#define RF_RDP_CLIPRDR_CAPS_VERSION_1 1u
#define RF_RDP_CLIPRDR_CAPS_VERSION_2 2u
#define RF_RDP_CLIPRDR_CAPS_USE_LONG_FORMAT_NAMES 0x00000002u

#define RF_RDP_CLIPRDR_CF_TEXT 1u
#define RF_RDP_CLIPRDR_CF_OEMTEXT 7u
#define RF_RDP_CLIPRDR_CF_DIB 8u
#define RF_RDP_CLIPRDR_CF_UNICODETEXT 13u
#define RF_RDP_CLIPRDR_CF_LOCALE 16u
#define RF_RDP_CLIPRDR_CF_DIBV5 17u
#define RF_RDP_CLIPRDR_CF_TIFF 6u

#define RF_RDP_CLIPRDR_FORMAT_HTML 0xd010u
#define RF_RDP_CLIPRDR_FORMAT_PNG 0xd035u
#define RF_RDP_CLIPRDR_FORMAT_TIFF 0xd036u
#define RF_RDP_CLIPRDR_FORMAT_JPEG 0xd037u
#define RF_RDP_CLIPRDR_FORMAT_WEBP 0xd038u
#define RF_RDP_CLIPRDR_FORMAT_BMP 0xd039u
#define RF_RDP_CLIPRDR_HTML_FORMAT_NAME "HTML Format"
#define RF_RDP_CLIPRDR_PNG_FORMAT_NAME "image/png"
#define RF_RDP_CLIPRDR_TIFF_FORMAT_NAME "image/tiff"
#define RF_RDP_CLIPRDR_JPEG_FORMAT_NAME "image/jpeg"
#define RF_RDP_CLIPRDR_WEBP_FORMAT_NAME "image/webp"
#define RF_RDP_CLIPRDR_BMP_FORMAT_NAME "image/bmp"

#define RF_RDP_CLIPRDR_CHANNEL_FLAG_FIRST 0x00000001u
#define RF_RDP_CLIPRDR_CHANNEL_FLAG_LAST 0x00000002u
#define RF_RDP_CLIPRDR_CHANNEL_FLAG_SHOW_PROTOCOL 0x00000010u
#define RF_RDP_CLIPRDR_CHANNEL_MAX_REASSEMBLED_LENGTH (64u * 1024u * 1024u)

struct rf_rdp_cliprdr_pdu {
	size_t data_offset;
	uint32_t data_length;
	uint16_t msg_type;
	uint16_t msg_flags;
};

struct rf_rdp_cliprdr_format_list {
	bool unicode_text;
	bool text;
	bool oem_text;
	bool locale;
	bool html;
	bool dib;
	bool dibv5;
	bool cf_tiff;
	bool png;
	bool tiff;
	bool jpeg;
	bool webp;
	bool bmp;
	uint32_t html_format_id;
	uint32_t png_format_id;
	uint32_t tiff_format_id;
	uint32_t jpeg_format_id;
	uint32_t webp_format_id;
	uint32_t bmp_format_id;
};

struct rf_rdp_cliprdr_channel_reassembler {
	GByteArray *payload;
	uint32_t total_length;
};

bool rf_rdp_cliprdr_parse_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_pdu *pdu
);
bool rf_rdp_cliprdr_channel_reassembler_add(
	struct rf_rdp_cliprdr_channel_reassembler *reassembler,
	const uint8_t *data,
	size_t length,
	GByteArray **payload
);
void rf_rdp_cliprdr_channel_reassembler_clear(
	struct rf_rdp_cliprdr_channel_reassembler *reassembler
);
bool rf_rdp_cliprdr_parse_format_list(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_format_list *formats
);
uint32_t rf_rdp_cliprdr_choose_request_format(
	const struct rf_rdp_cliprdr_format_list *formats
);
uint32_t rf_rdp_cliprdr_choose_request_format_after(
	const struct rf_rdp_cliprdr_format_list *formats,
	uint32_t previous_format_id
);
void rf_rdp_cliprdr_set_server_image_formats(
	struct rf_rdp_cliprdr_format_list *formats
);
bool rf_rdp_cliprdr_parse_format_data_request(
	const uint8_t *data,
	size_t length,
	uint32_t *format_id
);
char *rf_rdp_cliprdr_parse_format_data_response_text(
	const uint8_t *data,
	size_t length,
	uint16_t msg_flags
);
size_t rf_rdp_cliprdr_write_caps(uint8_t *data, size_t capacity);
size_t rf_rdp_cliprdr_write_monitor_ready(uint8_t *data, size_t capacity);
size_t rf_rdp_cliprdr_write_format_list(uint8_t *data, size_t capacity);
size_t rf_rdp_cliprdr_write_format_list_for_formats(
	uint8_t *data,
	size_t capacity,
	const struct rf_rdp_cliprdr_format_list *formats
);
size_t rf_rdp_cliprdr_write_format_list_response(
	uint8_t *data,
	size_t capacity,
	bool ok
);
size_t rf_rdp_cliprdr_write_format_data_request(
	uint8_t *data,
	size_t capacity,
	uint32_t format_id
);
size_t rf_rdp_cliprdr_write_format_data_response_text(
	uint8_t *data,
	size_t capacity,
	const char *text
);
size_t rf_rdp_cliprdr_write_format_data_response_utf8_text(
	uint8_t *data,
	size_t capacity,
	const char *text
);
size_t rf_rdp_cliprdr_write_format_data_response_locale(
	uint8_t *data,
	size_t capacity,
	uint32_t locale_id
);
size_t rf_rdp_cliprdr_write_format_data_response_bytes(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
);
size_t rf_rdp_cliprdr_write_format_data_response_fail(
	uint8_t *data,
	size_t capacity
);
GByteArray *rf_rdp_cliprdr_html_format_wrap(
	const uint8_t *html,
	size_t html_length
);
GByteArray *rf_rdp_cliprdr_html_format_unwrap(
	const uint8_t *html_format,
	size_t html_format_length
);
char *rf_rdp_cliprdr_html_fragment_to_text(
	const uint8_t *html,
	size_t html_length
);
GByteArray *rf_rdp_cliprdr_rgba_to_dib(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride,
	bool v5
);
GByteArray *rf_rdp_cliprdr_dib_to_rgba(
	const uint8_t *dib,
	size_t dib_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
);
GByteArray *rf_rdp_cliprdr_png_to_rgba(
	const uint8_t *png,
	size_t png_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
);
GByteArray *rf_rdp_cliprdr_image_format_to_rgba(
	const uint8_t *image,
	size_t image_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
);
GByteArray *rf_rdp_cliprdr_rgba_to_image_format(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride,
	const char *format_name
);
GByteArray *rf_rdp_cliprdr_html_image_to_rgba(
	const uint8_t *html_format,
	size_t html_format_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
);

#endif
