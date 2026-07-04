#include "rf-clipboard-rich.h"

#include <string.h>

#define RF_CLIPBOARD_RICH_HEADER_LENGTH 8u
#define RF_CLIPBOARD_RICH_ENTRY_HEADER_LENGTH 24u
#define RF_CLIPBOARD_RICH_VERSION 1u

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

static void write_u64_le(uint8_t *data, uint64_t value)
{
	for (size_t i = 0; i < 8; ++i)
		data[i] = (value >> (i * 8)) & 0xff;
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

static uint64_t read_u64_le(const uint8_t *data)
{
	uint64_t value = 0;

	for (size_t i = 0; i < 8; ++i)
		value |= (uint64_t)data[i] << (i * 8);
	return value;
}

void rf_clipboard_rich_payload_init(struct rf_clipboard_rich_payload *payload)
{
	g_return_if_fail(payload != NULL);

	memset(payload, 0, sizeof(*payload));
}

void rf_clipboard_rich_payload_clear(struct rf_clipboard_rich_payload *payload)
{
	if (payload == NULL)
		return;

	g_clear_pointer(&payload->text, g_free);
	g_clear_pointer(&payload->html, g_byte_array_unref);
	g_clear_pointer(&payload->image_rgba, g_byte_array_unref);
	payload->image_width = 0;
	payload->image_height = 0;
	payload->image_stride = 0;
}

bool rf_clipboard_rich_payload_set_text(
	struct rf_clipboard_rich_payload *payload,
	const char *text
)
{
	if (payload == NULL || text == NULL)
		return false;

	g_free(payload->text);
	payload->text = g_strdup(text);
	return payload->text != NULL;
}

bool rf_clipboard_rich_payload_set_html(
	struct rf_clipboard_rich_payload *payload,
	const uint8_t *html,
	size_t html_length
)
{
	if (payload == NULL || (html == NULL && html_length > 0) ||
	    html_length > RF_CLIPBOARD_RICH_MAX_BYTES)
		return false;

	g_clear_pointer(&payload->html, g_byte_array_unref);
	payload->html = g_byte_array_sized_new(html_length);
	if (html_length > 0)
		g_byte_array_append(payload->html, html, html_length);
	return true;
}

bool rf_clipboard_rich_payload_set_image_rgba(
	struct rf_clipboard_rich_payload *payload,
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride
)
{
	if (payload == NULL || rgba == NULL || width == 0 || height == 0 ||
	    stride > G_MAXUINT32 || stride < (size_t)width * 4 ||
	    rgba_length > RF_CLIPBOARD_RICH_MAX_BYTES ||
	    rgba_length < (size_t)height * stride)
		return false;

	g_clear_pointer(&payload->image_rgba, g_byte_array_unref);
	payload->image_rgba = g_byte_array_sized_new(rgba_length);
	g_byte_array_append(payload->image_rgba, rgba, rgba_length);
	payload->image_width = width;
	payload->image_height = height;
	payload->image_stride = stride;
	return true;
}

bool rf_clipboard_rich_payload_has_data(
	const struct rf_clipboard_rich_payload *payload
)
{
	return payload != NULL &&
	       ((payload->text != NULL && payload->text[0] != '\0') ||
	        payload->html != NULL || payload->image_rgba != NULL);
}

static uint16_t payload_entry_count(const struct rf_clipboard_rich_payload *payload)
{
	uint16_t count = 0;

	if (payload->text != NULL)
		count++;
	if (payload->html != NULL)
		count++;
	if (payload->image_rgba != NULL)
		count++;
	return count;
}

static bool append_entry(
	GByteArray *wire,
	uint32_t format,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	const uint8_t *data,
	size_t length
)
{
	uint8_t header[RF_CLIPBOARD_RICH_ENTRY_HEADER_LENGTH] = { 0 };

	if (length > RF_CLIPBOARD_RICH_MAX_BYTES ||
	    (data == NULL && length > 0))
		return false;

	write_u32_le(header, format);
	write_u32_le(header + 4, width);
	write_u32_le(header + 8, height);
	write_u32_le(header + 12, stride);
	write_u64_le(header + 16, length);
	g_byte_array_append(wire, header, sizeof(header));
	if (length > 0)
		g_byte_array_append(wire, data, length);
	return true;
}

GByteArray *rf_clipboard_rich_payload_serialize(
	const struct rf_clipboard_rich_payload *payload
)
{
	GByteArray *wire = NULL;
	uint8_t header[RF_CLIPBOARD_RICH_HEADER_LENGTH] = {
		'R', 'F', 'C', 'P', 0, 0, 0, 0
	};
	const uint16_t count = payload != NULL ? payload_entry_count(payload) : 0;

	if (payload == NULL || count == 0)
		return NULL;

	write_u16_le(header + 4, RF_CLIPBOARD_RICH_VERSION);
	write_u16_le(header + 6, count);
	wire = g_byte_array_new();
	g_byte_array_append(wire, header, sizeof(header));
	if (payload->text != NULL &&
	    !append_entry(
		    wire,
		    RF_CLIPBOARD_RICH_FORMAT_TEXT,
		    0,
		    0,
		    0,
		    (const uint8_t *)payload->text,
		    strlen(payload->text)
	    ))
		goto fail;
	if (payload->html != NULL &&
	    !append_entry(
		    wire,
		    RF_CLIPBOARD_RICH_FORMAT_HTML,
		    0,
		    0,
		    0,
		    payload->html->data,
		    payload->html->len
	    ))
		goto fail;
	if (payload->image_rgba != NULL &&
	    !append_entry(
		    wire,
		    RF_CLIPBOARD_RICH_FORMAT_IMAGE_RGBA,
		    payload->image_width,
		    payload->image_height,
		    (uint32_t)payload->image_stride,
		    payload->image_rgba->data,
		    payload->image_rgba->len
	    ))
		goto fail;
	return wire;

fail:
	g_byte_array_unref(wire);
	return NULL;
}

bool rf_clipboard_rich_wire_equal(
	const GByteArray *left,
	const GByteArray *right
)
{
	if (left == right)
		return true;
	if (left == NULL || right == NULL || left->len != right->len)
		return false;
	return left->len == 0 || memcmp(left->data, right->data, left->len) == 0;
}

static bool payload_set_entry(
	struct rf_clipboard_rich_payload *payload,
	uint32_t format,
	uint32_t width,
	uint32_t height,
	uint32_t stride,
	const uint8_t *data,
	size_t length
)
{
	switch (format) {
	case RF_CLIPBOARD_RICH_FORMAT_TEXT: {
		g_autofree char *text = NULL;

		if (!g_utf8_validate((const char *)data, length, NULL))
			return false;
		text = g_strndup((const char *)data, length);
		return rf_clipboard_rich_payload_set_text(payload, text);
	}
	case RF_CLIPBOARD_RICH_FORMAT_HTML:
		return rf_clipboard_rich_payload_set_html(payload, data, length);
	case RF_CLIPBOARD_RICH_FORMAT_IMAGE_RGBA:
		return rf_clipboard_rich_payload_set_image_rgba(
			payload,
			data,
			length,
			width,
			height,
			stride
		);
	default:
		return true;
	}
}

bool rf_clipboard_rich_payload_parse(
	const uint8_t *data,
	size_t length,
	struct rf_clipboard_rich_payload *payload
)
{
	uint16_t count = 0;
	size_t offset = RF_CLIPBOARD_RICH_HEADER_LENGTH;
	struct rf_clipboard_rich_payload parsed = { 0 };

	if (data == NULL || payload == NULL ||
	    length < RF_CLIPBOARD_RICH_HEADER_LENGTH ||
	    memcmp(data, "RFCP", 4) != 0 ||
	    read_u16_le(data + 4) != RF_CLIPBOARD_RICH_VERSION)
		return false;

	count = read_u16_le(data + 6);
	rf_clipboard_rich_payload_init(&parsed);
	for (uint16_t i = 0; i < count; ++i) {
		uint32_t format = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t stride = 0;
		uint64_t entry_length = 0;

		if (length - offset < RF_CLIPBOARD_RICH_ENTRY_HEADER_LENGTH)
			goto fail;
		format = read_u32_le(data + offset);
		width = read_u32_le(data + offset + 4);
		height = read_u32_le(data + offset + 8);
		stride = read_u32_le(data + offset + 12);
		entry_length = read_u64_le(data + offset + 16);
		offset += RF_CLIPBOARD_RICH_ENTRY_HEADER_LENGTH;
		if (entry_length > RF_CLIPBOARD_RICH_MAX_BYTES ||
		    entry_length > SIZE_MAX || length - offset < entry_length)
			goto fail;
		if (!payload_set_entry(
			    &parsed,
			    format,
			    width,
			    height,
			    stride,
			    data + offset,
			    (size_t)entry_length
		    ))
			goto fail;
		offset += (size_t)entry_length;
	}
	if (offset != length)
		goto fail;

	rf_clipboard_rich_payload_clear(payload);
	*payload = parsed;
	return true;

fail:
	rf_clipboard_rich_payload_clear(&parsed);
	return false;
}
