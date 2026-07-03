#include "rf-rdp-cliprdr.h"

#include <string.h>

#include <glib.h>

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

		if (format_id == RF_RDP_CLIPRDR_CF_UNICODETEXT)
			formats->unicode_text = true;
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
		bool terminated = false;

		offset += 4;
		while (offset + 2 <= length) {
			const uint16_t ch = read_u16_le(data + offset);

			offset += 2;
			if (ch == 0) {
				terminated = true;
				break;
			}
		}
		if (!terminated)
			return false;
		if (format_id == RF_RDP_CLIPRDR_CF_UNICODETEXT)
			formats->unicode_text = true;
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
	if (parse_short_format_list(data, length, formats))
		return true;
	return parse_long_format_list(data, length, formats);
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
	write_u32_le(data + 20, 0);
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
