#include "rf-rdp-cliprdr.h"

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifdef RF_HAVE_RDP_CLIPRDR_PNG
#include <gdk-pixbuf/gdk-pixbuf.h>
#endif
#ifdef RF_HAVE_RDP_CLIPRDR_LIBPNG
#include <png.h>
#endif
#ifdef RF_HAVE_RDP_CLIPRDR_TIFF
#include <tiffio.h>
#endif
#include <glib.h>

#define RF_RDP_CLIPRDR_BI_RGB 0u
#define RF_RDP_CLIPRDR_BI_BITFIELDS 3u
#define RF_RDP_CLIPRDR_LCS_SRGB 0x73524742u

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

static int32_t read_s32_le(const uint8_t *data)
{
	return (int32_t)read_u32_le(data);
}

static bool write_header(
	uint8_t *data,
	size_t capacity,
	uint16_t msg_type,
	uint16_t msg_flags,
	size_t payload_length
)
{
	const size_t length = RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;

	if (data == NULL || capacity < length || payload_length > UINT32_MAX)
		return false;
	write_u16_le(data, msg_type);
	write_u16_le(data + 2, msg_flags);
	write_u32_le(data + 4, payload_length);
	return true;
}

static void mark_format(
	struct rf_rdp_cliprdr_format_list *formats,
	uint32_t format_id,
	const char *name
)
{
	switch (format_id) {
	case RF_RDP_CLIPRDR_CF_TIFF:
		formats->cf_tiff = true;
		break;
	case RF_RDP_CLIPRDR_CF_UNICODETEXT:
		formats->unicode_text = true;
		break;
	case RF_RDP_CLIPRDR_CF_TEXT:
		formats->text = true;
		break;
	case RF_RDP_CLIPRDR_CF_OEMTEXT:
		formats->oem_text = true;
		break;
	case RF_RDP_CLIPRDR_CF_LOCALE:
		formats->locale = true;
		break;
	case RF_RDP_CLIPRDR_CF_DIB:
		formats->dib = true;
		break;
	case RF_RDP_CLIPRDR_CF_DIBV5:
		formats->dibv5 = true;
		break;
	default:
		break;
	}
	if (name != NULL &&
	    g_ascii_strcasecmp(name, RF_RDP_CLIPRDR_HTML_FORMAT_NAME) == 0) {
		formats->html = true;
		formats->html_format_id = format_id;
	}
	if (name != NULL &&
	    (g_ascii_strcasecmp(name, RF_RDP_CLIPRDR_PNG_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(name, "PNG") == 0)) {
		formats->png = true;
		formats->png_format_id = format_id;
	}
	if (name != NULL &&
	    (g_ascii_strcasecmp(name, RF_RDP_CLIPRDR_TIFF_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(name, "TIFF") == 0)) {
		formats->tiff = true;
		formats->tiff_format_id = format_id;
	}
	if (name != NULL &&
	    (g_ascii_strcasecmp(name, RF_RDP_CLIPRDR_JPEG_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(name, "image/jpg") == 0 ||
	     g_ascii_strcasecmp(name, "JPEG") == 0 ||
	     g_ascii_strcasecmp(name, "JPG") == 0)) {
		formats->jpeg = true;
		formats->jpeg_format_id = format_id;
	}
	if (name != NULL &&
	    (g_ascii_strcasecmp(name, RF_RDP_CLIPRDR_WEBP_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(name, "WEBP") == 0)) {
		formats->webp = true;
		formats->webp_format_id = format_id;
	}
	if (name != NULL &&
	    (g_ascii_strcasecmp(name, RF_RDP_CLIPRDR_BMP_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(name, "image/x-bmp") == 0 ||
	     g_ascii_strcasecmp(name, "image/x-MS-bmp") == 0 ||
	     g_ascii_strcasecmp(name, "BMP") == 0)) {
		formats->bmp = true;
		formats->bmp_format_id = format_id;
	}
}

bool rf_rdp_cliprdr_parse_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_pdu *pdu
)
{
	uint32_t data_length = 0;

	if (data == NULL || pdu == NULL || length < RF_RDP_CLIPRDR_HEADER_SIZE)
		return false;
	data_length = read_u32_le(data + 4);
	if (data_length > length - RF_RDP_CLIPRDR_HEADER_SIZE)
		return false;

	pdu->data_offset = RF_RDP_CLIPRDR_HEADER_SIZE;
	pdu->data_length = data_length;
	pdu->msg_type = read_u16_le(data);
	pdu->msg_flags = read_u16_le(data + 2);
	return true;
}

static bool parse_channel_pdu(
	const uint8_t *data,
	size_t length,
	uint32_t *total_length,
	uint32_t *flags,
	const uint8_t **payload,
	size_t *payload_length
)
{
	if (data == NULL || total_length == NULL || flags == NULL ||
	    payload == NULL || payload_length == NULL || length < 8)
		return false;

	*total_length = read_u32_le(data);
	*flags = read_u32_le(data + 4);
	*payload = data + 8;
	*payload_length = length - 8;
	if (*total_length < *payload_length ||
	    *total_length > RF_RDP_CLIPRDR_CHANNEL_MAX_REASSEMBLED_LENGTH)
		return false;
	return true;
}

void rf_rdp_cliprdr_channel_reassembler_clear(
	struct rf_rdp_cliprdr_channel_reassembler *reassembler
)
{
	if (reassembler == NULL)
		return;
	g_clear_pointer(&reassembler->payload, g_byte_array_unref);
	reassembler->total_length = 0;
}

bool rf_rdp_cliprdr_channel_reassembler_add(
	struct rf_rdp_cliprdr_channel_reassembler *reassembler,
	const uint8_t *data,
	size_t length,
	GByteArray **payload
)
{
	uint32_t total_length = 0;
	uint32_t flags = 0;
	const uint8_t *fragment = NULL;
	size_t fragment_length = 0;
	bool first = false;
	bool last = false;

	if (payload != NULL)
		*payload = NULL;
	if (reassembler == NULL || payload == NULL ||
	    !parse_channel_pdu(
		    data,
		    length,
		    &total_length,
		    &flags,
		    &fragment,
		    &fragment_length
	    ))
		return false;

	first = (flags & RF_RDP_CLIPRDR_CHANNEL_FLAG_FIRST) != 0;
	last = (flags & RF_RDP_CLIPRDR_CHANNEL_FLAG_LAST) != 0;
	if (first) {
		rf_rdp_cliprdr_channel_reassembler_clear(reassembler);
		reassembler->total_length = total_length;
		reassembler->payload = g_byte_array_sized_new(total_length);
	} else if (reassembler->payload == NULL ||
		   reassembler->total_length != total_length) {
		rf_rdp_cliprdr_channel_reassembler_clear(reassembler);
		return false;
	}

	if (fragment_length > reassembler->total_length ||
	    reassembler->payload->len > reassembler->total_length - fragment_length) {
		rf_rdp_cliprdr_channel_reassembler_clear(reassembler);
		return false;
	}
	if (fragment_length > 0)
		g_byte_array_append(
			reassembler->payload,
			fragment,
			fragment_length
		);

	if (!last)
		return true;
	if (reassembler->payload->len != reassembler->total_length) {
		rf_rdp_cliprdr_channel_reassembler_clear(reassembler);
		return false;
	}

	*payload = g_steal_pointer(&reassembler->payload);
	reassembler->total_length = 0;
	return true;
}

static bool parse_short_format_list(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_format_list *formats
)
{
	if (length % 36 != 0)
		return false;
	for (size_t offset = 0; offset + 36 <= length; offset += 36) {
		const uint32_t format_id = read_u32_le(data + offset);

		mark_format(formats, format_id, NULL);
	}
	return true;
}

static bool parse_long_format_list(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_format_list *formats
)
{
	size_t offset = 0;

	while (offset + 4 <= length) {
		const uint32_t format_id = read_u32_le(data + offset);
		g_autofree gunichar2 *utf16 = NULL;
		g_autofree char *name = NULL;
		size_t name_units = 0;
		bool terminated = false;

		offset += 4;
		while (offset + 2 <= length) {
			const uint16_t ch = read_u16_le(data + offset);

			offset += 2;
			if (ch == 0) {
				terminated = true;
				break;
			}
			name_units++;
		}
		if (!terminated)
			return false;
		if (name_units > 0) {
			utf16 = g_new0(gunichar2, name_units + 1);
			size_t name_offset = offset - 2 - name_units * 2;

			for (size_t i = 0; i < name_units; ++i)
				utf16[i] = read_u16_le(data + name_offset + i * 2);
			name = g_utf16_to_utf8(utf16, name_units, NULL, NULL, NULL);
		}
		mark_format(formats, format_id, name);
	}
	return offset == length;
}

bool rf_rdp_cliprdr_parse_format_list(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_format_list *formats
)
{
	if (data == NULL || formats == NULL)
		return false;

	memset(formats, 0, sizeof(*formats));
	if (length == 0)
		return true;
	if (parse_long_format_list(data, length, formats))
		return true;
	return parse_short_format_list(data, length, formats);
}

static bool append_request_candidate(
	uint32_t *candidates,
	size_t capacity,
	size_t *count,
	uint32_t format_id
)
{
	if (format_id == 0 || candidates == NULL || count == NULL ||
	    *count >= capacity)
		return false;
	for (size_t i = 0; i < *count; ++i) {
		if (candidates[i] == format_id)
			return true;
	}
	candidates[*count] = format_id;
	(*count)++;
	return true;
}

static size_t request_format_candidates(
	const struct rf_rdp_cliprdr_format_list *formats,
	uint32_t *candidates,
	size_t capacity
)
{
	size_t count = 0;
	const bool has_image =
		formats != NULL &&
		(formats->dib || formats->dibv5 || formats->png ||
		 formats->cf_tiff || formats->tiff || formats->jpeg ||
		 formats->webp || formats->bmp);

	if (formats == NULL)
		return 0;
	if (formats->html && formats->html_format_id != 0 && has_image)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->html_format_id
		);
#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	if (formats->webp && formats->webp_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->webp_format_id
		);
	if (formats->jpeg && formats->jpeg_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->jpeg_format_id
		);
	if (formats->tiff && formats->tiff_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->tiff_format_id
		);
	if (formats->png && formats->png_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->png_format_id
		);
	if (formats->bmp && formats->bmp_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->bmp_format_id
		);
	if (formats->cf_tiff)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			RF_RDP_CLIPRDR_CF_TIFF
		);
#endif
	if (formats->tiff && formats->tiff_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->tiff_format_id
		);
	if (formats->cf_tiff)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			RF_RDP_CLIPRDR_CF_TIFF
		);
	if (formats->dibv5)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			RF_RDP_CLIPRDR_CF_DIBV5
		);
	if (formats->dib)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			RF_RDP_CLIPRDR_CF_DIB
		);
	if (formats->html && formats->html_format_id != 0)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			formats->html_format_id
		);
	if (formats->unicode_text)
		append_request_candidate(
			candidates,
			capacity,
			&count,
			RF_RDP_CLIPRDR_CF_UNICODETEXT
		);
	return count;
}

uint32_t rf_rdp_cliprdr_choose_request_format(
	const struct rf_rdp_cliprdr_format_list *formats
)
{
	uint32_t candidates[16] = { 0 };
	const size_t count = request_format_candidates(
		formats,
		candidates,
		G_N_ELEMENTS(candidates)
	);

	return count > 0 ? candidates[0] : 0;
}

uint32_t rf_rdp_cliprdr_choose_request_format_after(
	const struct rf_rdp_cliprdr_format_list *formats,
	uint32_t previous_format_id
)
{
	uint32_t candidates[16] = { 0 };
	const size_t count = request_format_candidates(
		formats,
		candidates,
		G_N_ELEMENTS(candidates)
	);

	if (previous_format_id == 0)
		return count > 0 ? candidates[0] : 0;
	for (size_t i = 0; i < count; ++i) {
		if (candidates[i] == previous_format_id)
			return i + 1 < count ? candidates[i + 1] : 0;
	}
	return 0;
}

void rf_rdp_cliprdr_set_server_image_formats(
	struct rf_rdp_cliprdr_format_list *formats
)
{
	if (formats == NULL)
		return;

	formats->dib = true;
	formats->dibv5 = true;
}

bool rf_rdp_cliprdr_parse_format_data_request(
	const uint8_t *data,
	size_t length,
	uint32_t *format_id
)
{
	if (data == NULL || format_id == NULL || length != 4)
		return false;
	*format_id = read_u32_le(data);
	return true;
}

char *rf_rdp_cliprdr_parse_format_data_response_text(
	const uint8_t *data,
	size_t length,
	uint16_t msg_flags
)
{
	if (data == NULL || (msg_flags & RF_RDP_CLIPRDR_CB_RESPONSE_OK) == 0 ||
	    (length % 2) != 0)
		return NULL;

	size_t units = length / 2;
	while (units > 0 && read_u16_le(data + (units - 1) * 2) == 0)
		units--;

	g_autofree gunichar2 *utf16 = g_new0(gunichar2, units + 1);
	for (size_t i = 0; i < units; ++i)
		utf16[i] = read_u16_le(data + i * 2);
	return g_utf16_to_utf8(utf16, units, NULL, NULL, NULL);
}

size_t rf_rdp_cliprdr_write_caps(uint8_t *data, size_t capacity)
{
	const size_t payload_length = 16;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_CLIP_CAPS,
		    0,
		    payload_length
	    ))
		return 0;

	write_u16_le(data + 8, 1);
	write_u16_le(data + 10, 0);
	write_u16_le(data + 12, RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL);
	write_u16_le(data + 14, RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL_LEN);
	write_u32_le(data + 16, RF_RDP_CLIPRDR_CAPS_VERSION_2);
	write_u32_le(data + 20, RF_RDP_CLIPRDR_CAPS_USE_LONG_FORMAT_NAMES);
	return RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;
}

size_t rf_rdp_cliprdr_write_monitor_ready(uint8_t *data, size_t capacity)
{
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_MONITOR_READY,
		    0,
		    0
	    ))
		return 0;
	return RF_RDP_CLIPRDR_HEADER_SIZE;
}

size_t rf_rdp_cliprdr_write_format_list(uint8_t *data, size_t capacity)
{
	const size_t format_count = 4;
	const uint32_t formats[] = {
		RF_RDP_CLIPRDR_CF_UNICODETEXT,
		RF_RDP_CLIPRDR_CF_TEXT,
		RF_RDP_CLIPRDR_CF_OEMTEXT,
		RF_RDP_CLIPRDR_CF_LOCALE
	};
	const size_t payload_length = format_count * 36;

	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_LIST,
		    0,
		    payload_length
	    ))
		return 0;

	for (size_t i = 0; i < format_count; ++i) {
		write_u32_le(data + 8 + i * 36, formats[i]);
		memset(data + 8 + i * 36 + 4, 0, 32);
	}
	return RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;
}

static void append_u16_le(GByteArray *array, uint16_t value)
{
	uint8_t bytes[2] = { value & 0xff, value >> 8 };

	g_byte_array_append(array, bytes, sizeof(bytes));
}

static void append_u32_le(GByteArray *array, uint32_t value)
{
	uint8_t bytes[4] = {
		value & 0xff,
		(value >> 8) & 0xff,
		(value >> 16) & 0xff,
		(value >> 24) & 0xff
	};

	g_byte_array_append(array, bytes, sizeof(bytes));
}

static bool append_long_format(
	GByteArray *payload,
	uint32_t format_id,
	const char *name
)
{
	append_u32_le(payload, format_id);
	if (name != NULL && name[0] != '\0') {
		glong items_written = 0;
		g_autofree gunichar2 *utf16 =
			g_utf8_to_utf16(name, -1, NULL, &items_written, NULL);

		if (utf16 == NULL || items_written < 0)
			return false;
		for (size_t i = 0; i < (size_t)items_written; ++i)
			append_u16_le(payload, utf16[i]);
	}
	append_u16_le(payload, 0);
	return true;
}

size_t rf_rdp_cliprdr_write_format_list_for_formats(
	uint8_t *data,
	size_t capacity,
	const struct rf_rdp_cliprdr_format_list *formats
)
{
	g_autoptr(GByteArray) payload = NULL;

	if (data == NULL || formats == NULL)
		return 0;

	payload = g_byte_array_new();
	if (formats->unicode_text &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_UNICODETEXT, NULL))
		return 0;
	if (formats->text &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_TEXT, NULL))
		return 0;
	if (formats->oem_text &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_OEMTEXT, NULL))
		return 0;
	if (formats->locale &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_LOCALE, NULL))
		return 0;
	if (formats->dib &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_DIB, NULL))
		return 0;
	if (formats->dibv5 &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_DIBV5, NULL))
		return 0;
	if (formats->cf_tiff &&
	    !append_long_format(payload, RF_RDP_CLIPRDR_CF_TIFF, NULL))
		return 0;
	if (formats->tiff &&
	    !append_long_format(
		    payload,
		    formats->tiff_format_id != 0 ?
			    formats->tiff_format_id :
			    RF_RDP_CLIPRDR_FORMAT_TIFF,
		    RF_RDP_CLIPRDR_TIFF_FORMAT_NAME
	    ))
		return 0;
	if (formats->png &&
	    !append_long_format(
		    payload,
		    formats->png_format_id != 0 ?
			    formats->png_format_id :
			    RF_RDP_CLIPRDR_FORMAT_PNG,
		    RF_RDP_CLIPRDR_PNG_FORMAT_NAME
	    ))
		return 0;
	if (formats->jpeg &&
	    !append_long_format(
		    payload,
		    formats->jpeg_format_id != 0 ?
			    formats->jpeg_format_id :
			    RF_RDP_CLIPRDR_FORMAT_JPEG,
		    RF_RDP_CLIPRDR_JPEG_FORMAT_NAME
	    ))
		return 0;
	if (formats->webp &&
	    !append_long_format(
		    payload,
		    formats->webp_format_id != 0 ?
			    formats->webp_format_id :
			    RF_RDP_CLIPRDR_FORMAT_WEBP,
		    RF_RDP_CLIPRDR_WEBP_FORMAT_NAME
	    ))
		return 0;
	if (formats->bmp &&
	    !append_long_format(
		    payload,
		    formats->bmp_format_id != 0 ?
			    formats->bmp_format_id :
			    RF_RDP_CLIPRDR_FORMAT_BMP,
		    RF_RDP_CLIPRDR_BMP_FORMAT_NAME
	    ))
		return 0;
	if (formats->html &&
	    !append_long_format(
		    payload,
		    formats->html_format_id != 0 ?
			    formats->html_format_id :
			    RF_RDP_CLIPRDR_FORMAT_HTML,
		    RF_RDP_CLIPRDR_HTML_FORMAT_NAME
	    ))
		return 0;

	if (payload->len > UINT32_MAX ||
	    !write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_LIST,
		    0,
		    payload->len
	    ))
		return 0;
	memcpy(data + RF_RDP_CLIPRDR_HEADER_SIZE, payload->data, payload->len);
	return RF_RDP_CLIPRDR_HEADER_SIZE + payload->len;
}

size_t rf_rdp_cliprdr_write_format_list_response(
	uint8_t *data,
	size_t capacity,
	bool ok
)
{
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_LIST_RESPONSE,
		    ok ? RF_RDP_CLIPRDR_CB_RESPONSE_OK :
			 RF_RDP_CLIPRDR_CB_RESPONSE_FAIL,
		    0
	    ))
		return 0;
	return RF_RDP_CLIPRDR_HEADER_SIZE;
}

size_t rf_rdp_cliprdr_write_format_data_request(
	uint8_t *data,
	size_t capacity,
	uint32_t format_id
)
{
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_DATA_REQUEST,
		    0,
		    4
	    ))
		return 0;
	write_u32_le(data + 8, format_id);
	return RF_RDP_CLIPRDR_HEADER_SIZE + 4;
}

size_t rf_rdp_cliprdr_write_format_data_response_text(
	uint8_t *data,
	size_t capacity,
	const char *text
)
{
	glong items_written = 0;
	g_autofree gunichar2 *utf16 = NULL;
	size_t payload_length = 0;

	if (data == NULL || text == NULL)
		return 0;

	utf16 = g_utf8_to_utf16(text, -1, NULL, &items_written, NULL);
	if (utf16 == NULL || items_written < 0)
		return 0;
	payload_length = ((size_t)items_written + 1) * 2;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		    RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		    payload_length
	    ))
		return 0;

	for (size_t i = 0; i < (size_t)items_written + 1; ++i)
		write_u16_le(data + RF_RDP_CLIPRDR_HEADER_SIZE + i * 2, utf16[i]);
	return RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;
}

size_t rf_rdp_cliprdr_write_format_data_response_utf8_text(
	uint8_t *data,
	size_t capacity,
	const char *text
)
{
	size_t payload_length = 0;

	if (data == NULL || text == NULL)
		return 0;

	payload_length = strlen(text) + 1;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		    RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		    payload_length
	    ))
		return 0;

	memcpy(data + RF_RDP_CLIPRDR_HEADER_SIZE, text, payload_length);
	return RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;
}

size_t rf_rdp_cliprdr_write_format_data_response_locale(
	uint8_t *data,
	size_t capacity,
	uint32_t locale_id
)
{
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		    RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		    4
	    ))
		return 0;
	write_u32_le(data + RF_RDP_CLIPRDR_HEADER_SIZE, locale_id);
	return RF_RDP_CLIPRDR_HEADER_SIZE + 4;
}

size_t rf_rdp_cliprdr_write_format_data_response_bytes(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (data == NULL || (payload == NULL && payload_length > 0))
		return 0;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		    RF_RDP_CLIPRDR_CB_RESPONSE_OK,
		    payload_length
	    ))
		return 0;
	if (payload_length > 0)
		memcpy(data + RF_RDP_CLIPRDR_HEADER_SIZE, payload, payload_length);
	return RF_RDP_CLIPRDR_HEADER_SIZE + payload_length;
}

size_t rf_rdp_cliprdr_write_format_data_response_fail(
	uint8_t *data,
	size_t capacity
)
{
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE,
		    RF_RDP_CLIPRDR_CB_RESPONSE_FAIL,
		    0
	    ))
		return 0;
	return RF_RDP_CLIPRDR_HEADER_SIZE;
}

static bool decimal10_at(const uint8_t *data, size_t length, size_t offset, size_t *value)
{
	size_t parsed = 0;

	if (offset > length || length - offset < 10 || value == NULL)
		return false;
	for (size_t i = 0; i < 10; ++i) {
		const uint8_t ch = data[offset + i];

		if (!g_ascii_isdigit(ch))
			return false;
		parsed = parsed * 10 + ch - '0';
	}
	*value = parsed;
	return true;
}

static bool find_cf_html_offset(
	const uint8_t *data,
	size_t length,
	const char *key,
	size_t *value
)
{
	const char *found = g_strstr_len((const char *)data, length, key);

	if (found == NULL)
		return false;
	return decimal10_at(
		data,
		length,
		(size_t)(found - (const char *)data) + strlen(key),
		value
	);
}

static void patch_decimal10(uint8_t *data, size_t offset, size_t value)
{
	char digits[11] = { 0 };

	g_snprintf(digits, sizeof(digits), "%010zu", value);
	memcpy(data + offset, digits, 10);
}

GByteArray *rf_rdp_cliprdr_html_format_wrap(
	const uint8_t *html,
	size_t html_length
)
{
	static const char header[] =
		"Version:0.9\r\n"
		"StartHTML:0000000000\r\n"
		"EndHTML:0000000000\r\n"
		"StartFragment:0000000000\r\n"
		"EndFragment:0000000000\r\n";
	static const char start[] = "<html><body><!--StartFragment-->";
	static const char end[] = "<!--EndFragment--></body></html>";
	GByteArray *wrapped = NULL;
	const size_t start_html = strlen(header);
	const size_t start_fragment = start_html + strlen(start);
	const size_t end_fragment = start_fragment + html_length;
	const size_t end_html = end_fragment + strlen(end);

	if (html == NULL && html_length > 0)
		return NULL;

	wrapped = g_byte_array_sized_new(end_html);
	g_byte_array_append(wrapped, (const uint8_t *)header, strlen(header));
	g_byte_array_append(wrapped, (const uint8_t *)start, strlen(start));
	if (html_length > 0)
		g_byte_array_append(wrapped, html, html_length);
	g_byte_array_append(wrapped, (const uint8_t *)end, strlen(end));

	patch_decimal10(wrapped->data, strlen("Version:0.9\r\nStartHTML:"), start_html);
	patch_decimal10(
		wrapped->data,
		strlen("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:"),
		end_html
	);
	patch_decimal10(
		wrapped->data,
		strlen("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:"),
		start_fragment
	);
	patch_decimal10(
		wrapped->data,
		strlen("Version:0.9\r\nStartHTML:0000000000\r\nEndHTML:0000000000\r\nStartFragment:0000000000\r\nEndFragment:"),
		end_fragment
	);
	return wrapped;
}

GByteArray *rf_rdp_cliprdr_html_format_unwrap(
	const uint8_t *html_format,
	size_t html_format_length
)
{
	size_t start = 0;
	size_t end = 0;
	size_t raw_length = html_format_length;
	GByteArray *unwrapped = NULL;

	if (html_format == NULL && html_format_length > 0)
		return NULL;
	if (html_format_length == 0)
		return g_byte_array_new();

	if (find_cf_html_offset(
		    html_format,
		    html_format_length,
		    "StartFragment:",
		    &start
	    ) &&
	    find_cf_html_offset(
		    html_format,
		    html_format_length,
		    "EndFragment:",
		    &end
	    ) &&
	    start <= end && end <= html_format_length) {
		unwrapped = g_byte_array_sized_new(end - start);
		if (end > start)
			g_byte_array_append(
				unwrapped,
				html_format + start,
				end - start
			);
		return unwrapped;
	}

	if (raw_length > 0 && html_format[raw_length - 1] == 0)
		raw_length--;
	unwrapped = g_byte_array_sized_new(raw_length);
	if (raw_length > 0)
		g_byte_array_append(unwrapped, html_format, raw_length);
	return unwrapped;
}

static void append_text_newline(GString *text)
{
	if (text->len > 0 && text->str[text->len - 1] != '\n')
		g_string_append_c(text, '\n');
}

static bool append_named_entity(GString *text, const char *entity, size_t length)
{
	if (length == 2 && g_ascii_strncasecmp(entity, "lt", length) == 0)
		g_string_append_c(text, '<');
	else if (length == 2 && g_ascii_strncasecmp(entity, "gt", length) == 0)
		g_string_append_c(text, '>');
	else if (length == 3 && g_ascii_strncasecmp(entity, "amp", length) == 0)
		g_string_append_c(text, '&');
	else if (length == 4 && g_ascii_strncasecmp(entity, "quot", length) == 0)
		g_string_append_c(text, '"');
	else if (length == 4 && g_ascii_strncasecmp(entity, "nbsp", length) == 0)
		g_string_append_c(text, ' ');
	else if (length == 4 && g_ascii_strncasecmp(entity, "apos", length) == 0)
		g_string_append_c(text, '\'');
	else
		return false;
	return true;
}

static bool append_numeric_entity(GString *text, const char *entity, size_t length)
{
	uint32_t codepoint = 0;
	size_t offset = 1;
	bool hex = false;
	char utf8[6] = { 0 };
	int utf8_length = 0;

	if (length < 2 || entity[0] != '#')
		return false;
	if (entity[offset] == 'x' || entity[offset] == 'X') {
		hex = true;
		offset++;
	}
	if (offset >= length)
		return false;
	for (size_t i = offset; i < length; ++i) {
		const char ch = entity[i];
		uint32_t digit = 0;

		if (hex && g_ascii_isxdigit(ch))
			digit = g_ascii_xdigit_value(ch);
		else if (!hex && g_ascii_isdigit(ch))
			digit = ch - '0';
		else
			return false;
		if (codepoint > (G_MAXUINT32 - digit) / (hex ? 16u : 10u))
			return false;
		codepoint = codepoint * (hex ? 16u : 10u) + digit;
	}
	if (codepoint == 0 || !g_unichar_validate(codepoint))
		return false;
	utf8_length = g_unichar_to_utf8(codepoint, utf8);
	g_string_append_len(text, utf8, utf8_length);
	return true;
}

static void append_html_entity(
	GString *text,
	const uint8_t *html,
	size_t length,
	size_t *offset
)
{
	const size_t start = *offset + 1;
	size_t end = start;

	while (end < length && html[end] != ';' && end - start < 32)
		end++;
	if (end >= length || html[end] != ';') {
		g_string_append_c(text, '&');
		return;
	}

	const char *entity = (const char *)html + start;
	const size_t entity_length = end - start;
	if (!append_named_entity(text, entity, entity_length) &&
	    !append_numeric_entity(text, entity, entity_length)) {
		g_string_append_c(text, '&');
		g_string_append_len(text, entity, entity_length);
		g_string_append_c(text, ';');
	}
	*offset = end;
}

static bool tag_matches(const uint8_t *tag, size_t tag_length, const char *name)
{
	size_t offset = 0;
	const size_t name_length = strlen(name);

	while (offset < tag_length && g_ascii_isspace(tag[offset]))
		offset++;
	if (offset < tag_length && tag[offset] == '/')
		offset++;
	while (offset < tag_length && g_ascii_isspace(tag[offset]))
		offset++;
	if (tag_length - offset < name_length ||
	    g_ascii_strncasecmp((const char *)tag + offset, name, name_length) != 0)
		return false;
	offset += name_length;
	return offset == tag_length ||
	       g_ascii_isspace(tag[offset]) ||
	       tag[offset] == '/' ||
	       tag[offset] == '>';
}

static void handle_html_tag(
	GString *text,
	const uint8_t *html,
	size_t length,
	size_t *offset
)
{
	const size_t start = *offset + 1;
	size_t end = start;

	while (end < length && html[end] != '>')
		end++;
	if (end >= length) {
		g_string_append_c(text, '<');
		return;
	}
	if (tag_matches(html + start, end - start, "br") ||
	    tag_matches(html + start, end - start, "p") ||
	    tag_matches(html + start, end - start, "div") ||
	    tag_matches(html + start, end - start, "li"))
		append_text_newline(text);
	*offset = end;
}

char *rf_rdp_cliprdr_html_fragment_to_text(
	const uint8_t *html,
	size_t html_length
)
{
	g_autoptr(GString) text = NULL;

	if (html == NULL && html_length > 0)
		return NULL;

	text = g_string_sized_new(html_length);
	for (size_t i = 0; i < html_length; ++i) {
		if (html[i] == '<') {
			handle_html_tag(text, html, html_length, &i);
		} else if (html[i] == '&') {
			append_html_entity(text, html, html_length, &i);
		} else {
			g_string_append_c(text, html[i]);
		}
	}
	while (text->len > 0 && text->str[text->len - 1] == '\n')
		g_string_truncate(text, text->len - 1);
	return g_string_free(g_steal_pointer(&text), FALSE);
}

static const uint8_t *find_ascii_ci(
	const uint8_t *haystack,
	size_t haystack_length,
	const char *needle
)
{
	const size_t needle_length = strlen(needle);

	if (needle_length == 0)
		return haystack;
	if (haystack == NULL || haystack_length < needle_length)
		return NULL;
	for (size_t i = 0; i <= haystack_length - needle_length; ++i) {
		if (g_ascii_strncasecmp(
			    (const char *)haystack + i,
			    needle,
			    needle_length
		    ) == 0)
			return haystack + i;
	}
	return NULL;
}

static bool data_uri_image_mime_is_supported(
	const uint8_t *mime,
	size_t mime_length
)
{
	static const char image_prefix[] = "image/";

	return mime_length > strlen(image_prefix) &&
	       g_ascii_strncasecmp(
		       (const char *)mime,
		       image_prefix,
		       strlen(image_prefix)
	       ) == 0;
}

static GByteArray *extract_html_image_data_uri(
	const uint8_t *html,
	size_t html_length
)
{
	static const char marker[] = "data:";
	static const char base64_marker[] = ";base64,";
	const uint8_t *cursor = html;
	size_t remaining = html_length;

	if (html == NULL && html_length > 0)
		return NULL;
	while (remaining > 0) {
		const uint8_t *uri = find_ascii_ci(cursor, remaining, marker);
		const uint8_t *metadata = NULL;
		const uint8_t *payload = NULL;
		const uint8_t *end = NULL;
		g_autofree char *base64 = NULL;
		g_autofree guchar *decoded = NULL;
		gsize decoded_length = 0;

		if (uri == NULL)
			return NULL;
		metadata = uri + strlen(marker);
		payload = find_ascii_ci(
			metadata,
			(size_t)((html + html_length) - metadata),
			base64_marker
		);
		if (payload == NULL)
			return NULL;
		if (!data_uri_image_mime_is_supported(
			    metadata,
			    (size_t)(payload - metadata)
		    )) {
			cursor = uri + strlen(marker);
			remaining = (size_t)((html + html_length) - cursor);
			continue;
		}

		payload += strlen(base64_marker);
		end = payload;
		while (end < html + html_length &&
		       (g_ascii_isalnum(*end) || *end == '+' || *end == '/' ||
			*end == '=' || *end == '\r' || *end == '\n' ||
			*end == '\t' || *end == ' '))
			end++;
		if (end == payload)
			return NULL;

		base64 = g_strndup((const char *)payload, (size_t)(end - payload));
		decoded = g_base64_decode(base64, &decoded_length);
		if (decoded == NULL || decoded_length == 0)
			return NULL;
		return g_byte_array_new_take(g_steal_pointer(&decoded), decoded_length);
	}
	return NULL;
}

static bool checked_image_size(
	uint32_t width,
	uint32_t height,
	size_t stride,
	size_t *pixels_length
)
{
	if (width == 0 || height == 0 || width > INT32_MAX ||
	    height > INT32_MAX || stride < (size_t)width * 4)
		return false;
	if (height > SIZE_MAX / stride)
		return false;
	*pixels_length = (size_t)height * stride;
	return true;
}

GByteArray *rf_rdp_cliprdr_rgba_to_dib(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride,
	bool v5
)
{
	size_t source_length = 0;
	const size_t header_length = v5 ? 124 : 40;
	const size_t row_bytes = (size_t)width * 4;
	size_t pixel_length = 0;
	GByteArray *dib = NULL;

	if (rgba == NULL ||
	    !checked_image_size(width, height, stride, &source_length) ||
	    rgba_length < source_length || height > (uint32_t)INT32_MAX)
		return NULL;
	if (height > SIZE_MAX / row_bytes)
		return NULL;
	pixel_length = row_bytes * height;
	if (header_length > SIZE_MAX - pixel_length)
		return NULL;

	dib = g_byte_array_sized_new(header_length + pixel_length);
	g_byte_array_set_size(dib, header_length + pixel_length);
	memset(dib->data, 0, header_length);
	write_u32_le(dib->data, header_length);
	write_u32_le(dib->data + 4, width);
	write_u32_le(dib->data + 8, (uint32_t)(-(int32_t)height));
	write_u16_le(dib->data + 12, 1);
	write_u16_le(dib->data + 14, 32);
	write_u32_le(
		dib->data + 16,
		v5 ? RF_RDP_CLIPRDR_BI_BITFIELDS : RF_RDP_CLIPRDR_BI_RGB
	);
	write_u32_le(dib->data + 20, pixel_length);
	if (v5) {
		write_u32_le(dib->data + 40, 0x00ff0000);
		write_u32_le(dib->data + 44, 0x0000ff00);
		write_u32_le(dib->data + 48, 0x000000ff);
		write_u32_le(dib->data + 52, 0xff000000);
		write_u32_le(dib->data + 56, RF_RDP_CLIPRDR_LCS_SRGB);
	}

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *src = rgba + (size_t)y * stride;
		uint8_t *dst = dib->data + header_length + (size_t)y * row_bytes;

		for (uint32_t x = 0; x < width; ++x) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}
	return dib;
}

GByteArray *rf_rdp_cliprdr_dib_to_rgba(
	const uint8_t *dib,
	size_t dib_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	const uint32_t header_length = dib_length >= 4 ? read_u32_le(dib) : 0;
	const int32_t signed_width = dib_length >= 8 ? read_s32_le(dib + 4) : 0;
	const int32_t signed_height = dib_length >= 12 ? read_s32_le(dib + 8) : 0;
	const uint16_t planes = dib_length >= 14 ? read_u16_le(dib + 12) : 0;
	const uint16_t bit_count = dib_length >= 16 ? read_u16_le(dib + 14) : 0;
	const uint32_t compression = dib_length >= 20 ? read_u32_le(dib + 16) : 0;
	size_t pixel_offset = header_length;
	uint32_t abs_height = 0;
	size_t output_stride = 0;
	size_t output_length = 0;
	GByteArray *rgba = NULL;

	if (dib == NULL || width == NULL || height == NULL || stride == NULL ||
	    dib_length < 40 || header_length < 40 || header_length > dib_length ||
	    signed_width <= 0 || signed_height == 0 || planes != 1 ||
	    bit_count != 32 ||
	    (compression != RF_RDP_CLIPRDR_BI_RGB &&
	     compression != RF_RDP_CLIPRDR_BI_BITFIELDS))
		return NULL;

	if (signed_height == INT32_MIN)
		return NULL;
	abs_height = signed_height < 0 ? (uint32_t)-signed_height :
					 (uint32_t)signed_height;
	if (compression == RF_RDP_CLIPRDR_BI_BITFIELDS && header_length == 40) {
		if (dib_length < pixel_offset + 12)
			return NULL;
		pixel_offset += 12;
	}
	output_stride = (size_t)signed_width * 4;
	if (!checked_image_size(
		    (uint32_t)signed_width,
		    abs_height,
		    output_stride,
		    &output_length
	    ) || dib_length - pixel_offset < output_length)
		return NULL;

	rgba = g_byte_array_sized_new(output_length);
	g_byte_array_set_size(rgba, output_length);
	for (uint32_t y = 0; y < abs_height; ++y) {
		const uint32_t src_y = signed_height < 0 ? y : abs_height - 1 - y;
		const uint8_t *src = dib + pixel_offset + (size_t)src_y * output_stride;
		uint8_t *dst = rgba->data + (size_t)y * output_stride;

		for (uint32_t x = 0; x < (uint32_t)signed_width; ++x) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}

	*width = (uint32_t)signed_width;
	*height = abs_height;
	*stride = output_stride;
	return rgba;
}

#ifdef RF_HAVE_RDP_CLIPRDR_PNG
static const char *pixbuf_loader_type_for_image_bytes(
	const uint8_t *image,
	size_t image_length
)
{
	static const uint8_t png_signature[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
	};

	if (image == NULL)
		return NULL;
	if (image_length >= sizeof(png_signature) &&
	    memcmp(image, png_signature, sizeof(png_signature)) == 0)
		return "png";
	if (image_length >= 3 &&
	    image[0] == 0xff && image[1] == 0xd8 && image[2] == 0xff)
		return "jpeg";
	if (image_length >= 12 &&
	    memcmp(image, "RIFF", 4) == 0 && memcmp(image + 8, "WEBP", 4) == 0)
		return "webp";
	if (image_length >= 2 && image[0] == 'B' && image[1] == 'M')
		return "bmp";
	return NULL;
}

static bool image_bytes_are_png(const uint8_t *image, size_t image_length)
{
	static const uint8_t png_signature[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
	};

	return image != NULL && image_length >= sizeof(png_signature) &&
	       memcmp(image, png_signature, sizeof(png_signature)) == 0;
}

#ifdef RF_HAVE_RDP_CLIPRDR_LIBPNG
static GByteArray *png_bytes_to_rgba_libpng(
	const uint8_t *image,
	size_t image_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	png_image png = { 0 };
	size_t output_stride = 0;
	size_t output_length = 0;
	GByteArray *rgba = NULL;

	if (!image_bytes_are_png(image, image_length))
		return NULL;

	png.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_memory(&png, image, image_length)) {
		g_message(
			"RDP: cliprdr PNG libpng begin failed length=%zu error=%s.",
			image_length,
			png.message
		);
		return NULL;
	}

	png.format = PNG_FORMAT_RGBA;
	output_stride = PNG_IMAGE_ROW_STRIDE(png);
	if (png.width == 0 || png.height == 0 ||
	    !checked_image_size(png.width, png.height, output_stride, &output_length)) {
		g_message(
			"RDP: cliprdr PNG libpng unsupported dimensions %ux%u stride=%zu.",
			png.width,
			png.height,
			output_stride
		);
		png_image_free(&png);
		return NULL;
	}

	rgba = g_byte_array_sized_new(output_length);
	g_byte_array_set_size(rgba, output_length);
	if (!png_image_finish_read(&png, NULL, rgba->data, (png_int_32)output_stride, NULL)) {
		g_message(
			"RDP: cliprdr PNG libpng finish failed %ux%u length=%zu error=%s.",
			png.width,
			png.height,
			image_length,
			png.message
		);
		g_byte_array_unref(rgba);
		png_image_free(&png);
		return NULL;
	}

	*width = png.width;
	*height = png.height;
	*stride = output_stride;
	png_image_free(&png);
	return rgba;
}
#endif

static GByteArray *image_bytes_to_rgba_gdk_pixbuf(
	const uint8_t *image,
	size_t image_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	g_autoptr(GdkPixbufLoader) loader = NULL;
	GdkPixbuf *pixbuf = NULL;
	g_autoptr(GError) error = NULL;
	int bits_per_sample = 0;
	int pixbuf_width = 0;
	int pixbuf_height = 0;
	int channels = 0;
	int rowstride = 0;
	size_t output_stride = 0;
	size_t output_length = 0;
	GByteArray *rgba = NULL;
	const uint8_t *pixels = NULL;
	const char *loader_type = pixbuf_loader_type_for_image_bytes(
		image,
		image_length
	);

	if (loader_type != NULL)
		loader = gdk_pixbuf_loader_new_with_type(loader_type, &error);
	else
		loader = gdk_pixbuf_loader_new();
	if (loader == NULL) {
		g_message(
			"RDP: cliprdr image decode loader unavailable type=%s length=%zu head=%02x%02x%02x%02x error=%s.",
			loader_type != NULL ? loader_type : "auto",
			image_length,
			image_length > 0 ? image[0] : 0,
			image_length > 1 ? image[1] : 0,
			image_length > 2 ? image[2] : 0,
			image_length > 3 ? image[3] : 0,
			error != NULL ? error->message : "unknown"
		);
		return NULL;
	}
	if (!gdk_pixbuf_loader_write(loader, image, image_length, &error)) {
		g_message(
			"RDP: cliprdr image decode write failed type=%s length=%zu head=%02x%02x%02x%02x error=%s.",
			loader_type != NULL ? loader_type : "auto",
			image_length,
			image_length > 0 ? image[0] : 0,
			image_length > 1 ? image[1] : 0,
			image_length > 2 ? image[2] : 0,
			image_length > 3 ? image[3] : 0,
			error != NULL ? error->message : "unknown"
		);
		return NULL;
	}
	if (!gdk_pixbuf_loader_close(loader, &error)) {
		g_message(
			"RDP: cliprdr image decode close failed type=%s length=%zu head=%02x%02x%02x%02x error=%s.",
			loader_type != NULL ? loader_type : "auto",
			image_length,
			image_length > 0 ? image[0] : 0,
			image_length > 1 ? image[1] : 0,
			image_length > 2 ? image[2] : 0,
			image_length > 3 ? image[3] : 0,
			error != NULL ? error->message : "unknown"
		);
		return NULL;
	}

	pixbuf = gdk_pixbuf_loader_get_pixbuf(loader);
	if (pixbuf == NULL ||
	    gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB) {
		g_message(
			"RDP: cliprdr image decode produced unsupported pixbuf type=%s length=%zu pixbuf=%s colorspace=%d.",
			loader_type != NULL ? loader_type : "auto",
			image_length,
			pixbuf != NULL ? "yes" : "no",
			pixbuf != NULL ? gdk_pixbuf_get_colorspace(pixbuf) : -1
		);
		return NULL;
	}

	pixbuf_width = gdk_pixbuf_get_width(pixbuf);
	pixbuf_height = gdk_pixbuf_get_height(pixbuf);
	channels = gdk_pixbuf_get_n_channels(pixbuf);
	rowstride = gdk_pixbuf_get_rowstride(pixbuf);
	bits_per_sample = gdk_pixbuf_get_bits_per_sample(pixbuf);
	if (pixbuf_width <= 0 || pixbuf_height <= 0 ||
	    (channels != 3 && channels != 4) ||
	    bits_per_sample != 8)
		return NULL;
	output_stride = (size_t)pixbuf_width * 4;
	if (!checked_image_size(
		    (uint32_t)pixbuf_width,
		    (uint32_t)pixbuf_height,
		    output_stride,
		    &output_length
	    ))
		return NULL;

	pixels = gdk_pixbuf_get_pixels(pixbuf);
	if (pixels == NULL || rowstride < pixbuf_width * channels)
		return NULL;

	rgba = g_byte_array_sized_new(output_length);
	g_byte_array_set_size(rgba, output_length);
	for (int y = 0; y < pixbuf_height; ++y) {
		const uint8_t *src = pixels + (size_t)y * rowstride;
		uint8_t *dst = rgba->data + (size_t)y * output_stride;

		for (int x = 0; x < pixbuf_width; ++x) {
			dst[x * 4 + 0] = src[x * channels + 0];
			dst[x * 4 + 1] = src[x * channels + 1];
			dst[x * 4 + 2] = src[x * channels + 2];
			dst[x * 4 + 3] = channels == 4 ? src[x * channels + 3] : 0xff;
		}
	}

	*width = (uint32_t)pixbuf_width;
	*height = (uint32_t)pixbuf_height;
	*stride = output_stride;
	return rgba;
}

static GdkPixbuf *rgba_to_pixbuf(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride,
	bool alpha
)
{
	size_t source_length = 0;
	g_autoptr(GBytes) bytes = NULL;
	g_autofree uint8_t *rgb = NULL;

	if (rgba == NULL ||
	    !checked_image_size(width, height, stride, &source_length) ||
	    rgba_length < source_length || width > INT_MAX || height > INT_MAX ||
	    stride > INT_MAX)
		return NULL;

	if (!alpha) {
		const size_t rgb_stride = (size_t)width * 3;
		const size_t rgb_length = rgb_stride * height;

		if (height > SIZE_MAX / rgb_stride || rgb_stride > INT_MAX)
			return NULL;
		rgb = g_malloc0(rgb_length);
		for (uint32_t y = 0; y < height; ++y) {
			const uint8_t *src = rgba + (size_t)y * stride;
			uint8_t *dst = rgb + (size_t)y * rgb_stride;

			for (uint32_t x = 0; x < width; ++x) {
				dst[x * 3 + 0] = src[x * 4 + 0];
				dst[x * 3 + 1] = src[x * 4 + 1];
				dst[x * 3 + 2] = src[x * 4 + 2];
			}
		}
		bytes = g_bytes_new_take(g_steal_pointer(&rgb), rgb_length);
		return gdk_pixbuf_new_from_bytes(
			bytes,
			GDK_COLORSPACE_RGB,
			FALSE,
			8,
			(int)width,
			(int)height,
			(int)rgb_stride
		);
	}

	bytes = g_bytes_new(rgba, source_length);
	return gdk_pixbuf_new_from_bytes(
		bytes,
		GDK_COLORSPACE_RGB,
		TRUE,
		8,
		(int)width,
		(int)height,
		(int)stride
	);
}

static const char *pixbuf_save_type_for_mime(const char *format_name)
{
	if (format_name == NULL)
		return NULL;
	if (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_PNG_FORMAT_NAME) == 0 ||
	    g_ascii_strcasecmp(format_name, "PNG") == 0)
		return "png";
	if (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_TIFF_FORMAT_NAME) == 0 ||
	    g_ascii_strcasecmp(format_name, "TIFF") == 0)
		return "tiff";
	if (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_JPEG_FORMAT_NAME) == 0 ||
	    g_ascii_strcasecmp(format_name, "image/jpg") == 0 ||
	    g_ascii_strcasecmp(format_name, "JPEG") == 0 ||
	    g_ascii_strcasecmp(format_name, "JPG") == 0)
		return "jpeg";
	if (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_WEBP_FORMAT_NAME) == 0 ||
	    g_ascii_strcasecmp(format_name, "WEBP") == 0)
		return "webp";
	if (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_BMP_FORMAT_NAME) == 0 ||
	    g_ascii_strcasecmp(format_name, "image/x-bmp") == 0 ||
	    g_ascii_strcasecmp(format_name, "image/x-MS-bmp") == 0 ||
	    g_ascii_strcasecmp(format_name, "BMP") == 0)
		return "bmp";
	return NULL;
}
#endif

#ifdef RF_HAVE_RDP_CLIPRDR_LIBPNG
static GByteArray *rgba_to_png_libpng(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride
)
{
	png_image png = { 0 };
	png_alloc_size_t png_length = 0;
	GByteArray *encoded = NULL;
	size_t source_length = 0;

	if (rgba == NULL ||
	    !checked_image_size(width, height, stride, &source_length) ||
	    rgba_length < source_length || stride > INT32_MAX)
		return NULL;

	png.version = PNG_IMAGE_VERSION;
	png.width = width;
	png.height = height;
	png.format = PNG_FORMAT_RGBA;
	if (!png_image_write_to_memory(
		    &png,
		    NULL,
		    &png_length,
		    0,
		    rgba,
		    (png_int_32)stride,
		    NULL
	    ) ||
	    png_length == 0)
		return NULL;

	encoded = g_byte_array_sized_new(png_length);
	g_byte_array_set_size(encoded, png_length);
	if (!png_image_write_to_memory(
		    &png,
		    encoded->data,
		    &png_length,
		    0,
		    rgba,
		    (png_int_32)stride,
		    NULL
	    )) {
		g_byte_array_unref(encoded);
		return NULL;
	}
	g_byte_array_set_size(encoded, png_length);
	return encoded;
}
#endif

static GByteArray *rgba_to_bmp_file(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride
)
{
	g_autoptr(GByteArray) dib = rf_rdp_cliprdr_rgba_to_dib(
		rgba,
		rgba_length,
		width,
		height,
		stride,
		false
	);
	GByteArray *bmp = NULL;
	const uint32_t file_header_length = 14;
	const uint32_t pixel_offset = file_header_length + 40;
	uint32_t file_length = 0;

	if (dib == NULL || dib->len < 40 ||
	    dib->len > UINT32_MAX - file_header_length)
		return NULL;
	file_length = file_header_length + dib->len;
	bmp = g_byte_array_sized_new(file_length);
	g_byte_array_set_size(bmp, file_header_length);
	bmp->data[0] = 'B';
	bmp->data[1] = 'M';
	write_u32_le(bmp->data + 2, file_length);
	write_u32_le(bmp->data + 10, pixel_offset);
	g_byte_array_append(bmp, dib->data, dib->len);
	return bmp;
}

#ifdef RF_HAVE_RDP_CLIPRDR_TIFF
struct tiff_memory_writer {
	GByteArray *data;
	size_t offset;
};

struct tiff_memory_reader {
	const uint8_t *data;
	size_t length;
	size_t offset;
};

static tmsize_t tiff_writer_read(thandle_t handle, void *buffer, tmsize_t size)
{
	struct tiff_memory_writer *writer = (struct tiff_memory_writer *)handle;
	size_t available = 0;
	size_t requested = 0;

	if (writer == NULL || writer->data == NULL || buffer == NULL || size <= 0)
		return 0;
	if (writer->offset >= writer->data->len)
		return 0;
	available = writer->data->len - writer->offset;
	requested = (size_t)size;
	if (requested > available)
		requested = available;
	memcpy(buffer, writer->data->data + writer->offset, requested);
	writer->offset += requested;
	return (tmsize_t)requested;
}

static tmsize_t tiff_writer_write(thandle_t handle, void *buffer, tmsize_t size)
{
	struct tiff_memory_writer *writer = (struct tiff_memory_writer *)handle;
	size_t requested = 0;
	size_t end = 0;

	if (writer == NULL || writer->data == NULL || buffer == NULL || size <= 0)
		return 0;
	requested = (size_t)size;
	if (writer->offset > SIZE_MAX - requested)
		return 0;
	end = writer->offset + requested;
	if (end > writer->data->len)
		g_byte_array_set_size(writer->data, end);
	memcpy(writer->data->data + writer->offset, buffer, requested);
	writer->offset = end;
	return (tmsize_t)requested;
}

static toff_t tiff_writer_seek(thandle_t handle, toff_t offset, int whence)
{
	struct tiff_memory_writer *writer = (struct tiff_memory_writer *)handle;
	size_t base = 0;
	size_t next = 0;

	if (writer == NULL || writer->data == NULL || offset > (toff_t)SIZE_MAX)
		return (toff_t)-1;
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = writer->offset;
		break;
	case SEEK_END:
		base = writer->data->len;
		break;
	default:
		return (toff_t)-1;
	}
	if (base > SIZE_MAX - (size_t)offset)
		return (toff_t)-1;
	next = base + (size_t)offset;
	writer->offset = next;
	return (toff_t)writer->offset;
}

static int tiff_writer_close(thandle_t handle)
{
	(void)handle;
	return 0;
}

static toff_t tiff_writer_size(thandle_t handle)
{
	const struct tiff_memory_writer *writer =
		(const struct tiff_memory_writer *)handle;

	return writer != NULL && writer->data != NULL ?
		(toff_t)writer->data->len :
		0;
}

static int tiff_writer_map(thandle_t handle, void **base, toff_t *size)
{
	(void)handle;
	(void)base;
	(void)size;
	return 0;
}

static void tiff_writer_unmap(thandle_t handle, void *base, toff_t size)
{
	(void)handle;
	(void)base;
	(void)size;
}

static GByteArray *rgba_to_tiff_libtiff(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride
)
{
	struct tiff_memory_writer writer = { 0 };
	TIFF *tiff = NULL;
	size_t source_length = 0;
	uint16_t extra_sample = EXTRASAMPLE_UNASSALPHA;

	if (rgba == NULL ||
	    !checked_image_size(width, height, stride, &source_length) ||
	    rgba_length < source_length || width > INT32_MAX || height > INT32_MAX)
		return NULL;

	writer.data = g_byte_array_new();
	tiff = TIFFClientOpen(
		"reframe-cliprdr-memory",
		"w",
		(thandle_t)&writer,
		tiff_writer_read,
		tiff_writer_write,
		tiff_writer_seek,
		tiff_writer_close,
		tiff_writer_size,
		tiff_writer_map,
		tiff_writer_unmap
	);
	if (tiff == NULL) {
		g_byte_array_unref(writer.data);
		return NULL;
	}

	TIFFSetField(tiff, TIFFTAG_IMAGEWIDTH, width);
	TIFFSetField(tiff, TIFFTAG_IMAGELENGTH, height);
	TIFFSetField(tiff, TIFFTAG_SAMPLESPERPIXEL, 4);
	TIFFSetField(tiff, TIFFTAG_BITSPERSAMPLE, 8);
	TIFFSetField(tiff, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
	TIFFSetField(tiff, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
	TIFFSetField(tiff, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
	TIFFSetField(tiff, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
	TIFFSetField(tiff, TIFFTAG_EXTRASAMPLES, 1, &extra_sample);
	TIFFSetField(tiff, TIFFTAG_ROWSPERSTRIP, TIFFDefaultStripSize(tiff, 0));

	for (uint32_t y = 0; y < height; ++y) {
		const uint8_t *row = rgba + (size_t)y * stride;

		if (TIFFWriteScanline(tiff, (void *)row, y, 0) < 0) {
			TIFFClose(tiff);
			g_byte_array_unref(writer.data);
			return NULL;
		}
	}
	TIFFClose(tiff);
	if (writer.data->len == 0) {
		g_byte_array_unref(writer.data);
		return NULL;
	}
	return writer.data;
}

static tmsize_t tiff_memory_read(thandle_t handle, void *buffer, tmsize_t size)
{
	struct tiff_memory_reader *reader = (struct tiff_memory_reader *)handle;
	size_t available = 0;
	size_t requested = 0;

	if (reader == NULL || buffer == NULL || size <= 0)
		return 0;
	if (reader->offset >= reader->length)
		return 0;
	available = reader->length - reader->offset;
	requested = (size_t)size;
	if (requested > available)
		requested = available;
	memcpy(buffer, reader->data + reader->offset, requested);
	reader->offset += requested;
	return (tmsize_t)requested;
}

static tmsize_t tiff_memory_write(
	thandle_t handle,
	void *buffer,
	tmsize_t size
)
{
	(void)handle;
	(void)buffer;
	(void)size;
	return 0;
}

static toff_t tiff_memory_seek(thandle_t handle, toff_t offset, int whence)
{
	struct tiff_memory_reader *reader = (struct tiff_memory_reader *)handle;
	size_t base = 0;
	size_t next = 0;

	if (reader == NULL || offset > (toff_t)SIZE_MAX)
		return (toff_t)-1;
	switch (whence) {
	case SEEK_SET:
		base = 0;
		break;
	case SEEK_CUR:
		base = reader->offset;
		break;
	case SEEK_END:
		base = reader->length;
		break;
	default:
		return (toff_t)-1;
	}
	if (base > SIZE_MAX - (size_t)offset)
		return (toff_t)-1;
	next = base + (size_t)offset;
	if (next > reader->length)
		return (toff_t)-1;
	reader->offset = next;
	return (toff_t)reader->offset;
}

static int tiff_memory_close(thandle_t handle)
{
	(void)handle;
	return 0;
}

static toff_t tiff_memory_size(thandle_t handle)
{
	const struct tiff_memory_reader *reader =
		(const struct tiff_memory_reader *)handle;

	return reader != NULL ? (toff_t)reader->length : 0;
}

static int tiff_memory_map(thandle_t handle, void **base, toff_t *size)
{
	(void)handle;
	(void)base;
	(void)size;
	return 0;
}

static void tiff_memory_unmap(thandle_t handle, void *base, toff_t size)
{
	(void)handle;
	(void)base;
	(void)size;
}

static GByteArray *image_bytes_to_rgba_libtiff(
	const uint8_t *image,
	size_t image_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	struct tiff_memory_reader reader = {
		.data = image,
		.length = image_length,
		.offset = 0
	};
	TIFF *tiff = NULL;
	uint32_t tiff_width = 0;
	uint32_t tiff_height = 0;
	size_t output_stride = 0;
	size_t output_length = 0;
	g_autofree uint32_t *raster = NULL;
	GByteArray *rgba = NULL;

	if (image_length < 4 ||
	    !((image[0] == 'I' && image[1] == 'I' && image[2] == 0x2a &&
	       image[3] == 0x00) ||
	      (image[0] == 'M' && image[1] == 'M' && image[2] == 0x00 &&
	       image[3] == 0x2a)))
		return NULL;

	tiff = TIFFClientOpen(
		"reframe-clipboard",
		"r",
		(thandle_t)&reader,
		tiff_memory_read,
		tiff_memory_write,
		tiff_memory_seek,
		tiff_memory_close,
		tiff_memory_size,
		tiff_memory_map,
		tiff_memory_unmap
	);
	if (tiff == NULL)
		return NULL;
	if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &tiff_width) ||
	    !TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &tiff_height) ||
	    tiff_width == 0 || tiff_height == 0) {
		TIFFClose(tiff);
		return NULL;
	}

	output_stride = (size_t)tiff_width * 4;
	if (!checked_image_size(tiff_width, tiff_height, output_stride, &output_length)) {
		TIFFClose(tiff);
		return NULL;
	}
	if ((size_t)tiff_height > SIZE_MAX / (sizeof(uint32_t) * (size_t)tiff_width)) {
		TIFFClose(tiff);
		return NULL;
	}

	raster = g_new(uint32_t, (size_t)tiff_width * (size_t)tiff_height);
	if (!TIFFReadRGBAImageOriented(
		    tiff,
		    tiff_width,
		    tiff_height,
		    raster,
		    ORIENTATION_TOPLEFT,
		    0
	    )) {
		TIFFClose(tiff);
		return NULL;
	}
	TIFFClose(tiff);

	rgba = g_byte_array_sized_new(output_length);
	g_byte_array_set_size(rgba, output_length);
	for (uint32_t y = 0; y < tiff_height; ++y) {
		uint8_t *dst = rgba->data + (size_t)y * output_stride;

		for (uint32_t x = 0; x < tiff_width; ++x) {
			const uint32_t pixel = raster[(size_t)y * tiff_width + x];

			dst[x * 4 + 0] = TIFFGetR(pixel);
			dst[x * 4 + 1] = TIFFGetG(pixel);
			dst[x * 4 + 2] = TIFFGetB(pixel);
			dst[x * 4 + 3] = TIFFGetA(pixel);
		}
	}

	*width = tiff_width;
	*height = tiff_height;
	*stride = output_stride;
	return rgba;
}
#endif

static GByteArray *image_bytes_to_rgba(
	const uint8_t *image,
	size_t image_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	if (image == NULL || image_length == 0 || width == NULL || height == NULL ||
	    stride == NULL)
		return NULL;

#ifdef RF_HAVE_RDP_CLIPRDR_LIBPNG
	{
		g_autoptr(GByteArray) rgba = png_bytes_to_rgba_libpng(
			image,
			image_length,
			width,
			height,
			stride
		);

		if (rgba != NULL)
			return g_steal_pointer(&rgba);
	}
#endif
#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	{
		g_autoptr(GByteArray) rgba = image_bytes_to_rgba_gdk_pixbuf(
			image,
			image_length,
			width,
			height,
			stride
		);

		if (rgba != NULL)
			return g_steal_pointer(&rgba);
	}
#endif
#ifdef RF_HAVE_RDP_CLIPRDR_TIFF
	return image_bytes_to_rgba_libtiff(
		image,
		image_length,
		width,
		height,
		stride
	);
#else
	(void)image;
	(void)image_length;
	(void)width;
	(void)height;
	(void)stride;
	return NULL;
#endif
}

GByteArray *rf_rdp_cliprdr_png_to_rgba(
	const uint8_t *png,
	size_t png_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	return image_bytes_to_rgba(png, png_length, width, height, stride);
}

GByteArray *rf_rdp_cliprdr_image_format_to_rgba(
	const uint8_t *image,
	size_t image_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	return image_bytes_to_rgba(image, image_length, width, height, stride);
}

GByteArray *rf_rdp_cliprdr_rgba_to_image_format(
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride,
	const char *format_name
)
{
#ifdef RF_HAVE_RDP_CLIPRDR_LIBPNG
	if (format_name != NULL &&
	    (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_PNG_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(format_name, "PNG") == 0))
		return rgba_to_png_libpng(
			rgba,
			rgba_length,
			width,
			height,
			stride
		);
#endif
	if (format_name != NULL &&
	    (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_BMP_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(format_name, "image/x-bmp") == 0 ||
	     g_ascii_strcasecmp(format_name, "image/x-MS-bmp") == 0 ||
	     g_ascii_strcasecmp(format_name, "BMP") == 0))
		return rgba_to_bmp_file(
			rgba,
			rgba_length,
			width,
			height,
			stride
		);
#ifdef RF_HAVE_RDP_CLIPRDR_TIFF
	if (format_name != NULL &&
	    (g_ascii_strcasecmp(format_name, RF_RDP_CLIPRDR_TIFF_FORMAT_NAME) == 0 ||
	     g_ascii_strcasecmp(format_name, "TIFF") == 0))
		return rgba_to_tiff_libtiff(
			rgba,
			rgba_length,
			width,
			height,
			stride
		);
#endif

#ifdef RF_HAVE_RDP_CLIPRDR_PNG
	const char *type = pixbuf_save_type_for_mime(format_name);
	const bool alpha =
		type != NULL && g_strcmp0(type, "png") == 0;
	g_autoptr(GdkPixbuf) pixbuf = NULL;
	g_autofree gchar *buffer = NULL;
	gsize buffer_length = 0;
	g_autoptr(GError) error = NULL;
	GByteArray *encoded = NULL;

	if (type == NULL)
		return NULL;
	pixbuf = rgba_to_pixbuf(rgba, rgba_length, width, height, stride, alpha);
	if (pixbuf == NULL)
		return NULL;
	if (!gdk_pixbuf_save_to_buffer(
		    pixbuf,
		    &buffer,
		    &buffer_length,
		    type,
		    &error,
		    NULL
	    )) {
		g_message(
			"RDP: cliprdr image encode failed format=%s type=%s error=%s.",
			format_name != NULL ? format_name : "unknown",
			type,
			error != NULL ? error->message : "unknown"
		);
		return NULL;
	}
	if (buffer == NULL || buffer_length == 0)
		return NULL;

	encoded = g_byte_array_sized_new(buffer_length);
	g_byte_array_append(encoded, (const uint8_t *)buffer, buffer_length);
	return encoded;
#else
	(void)rgba;
	(void)rgba_length;
	(void)width;
	(void)height;
	(void)stride;
	(void)format_name;
	return NULL;
#endif
}

GByteArray *rf_rdp_cliprdr_html_image_to_rgba(
	const uint8_t *html_format,
	size_t html_format_length,
	uint32_t *width,
	uint32_t *height,
	size_t *stride
)
{
	g_autoptr(GByteArray) html = NULL;
	g_autoptr(GByteArray) image = NULL;

	if (html_format == NULL && html_format_length > 0)
		return NULL;
	if (width == NULL || height == NULL || stride == NULL)
		return NULL;

	html = rf_rdp_cliprdr_html_format_unwrap(html_format, html_format_length);
	if (html == NULL)
		return NULL;
	image = extract_html_image_data_uri(html->data, html->len);
	if (image == NULL)
		return NULL;
	return image_bytes_to_rgba(image->data, image->len, width, height, stride);
}
