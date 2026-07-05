#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rf-clipboard-rich.h"

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

static void test_serialize_and_parse_all_formats(void)
{
	g_auto(RfClipboardRichPayload) payload;
	g_auto(RfClipboardRichPayload) parsed;
	const uint8_t html[] = "<b>RDP</b>";
	const uint8_t image[] = {
		0x10, 0x20, 0x30, 0xff,
		0x40, 0x50, 0x60, 0x80
	};
	g_autoptr(GByteArray) wire = NULL;

	rf_clipboard_rich_payload_init(&payload);
	rf_clipboard_rich_payload_init(&parsed);
	assert(rf_clipboard_rich_payload_set_text(&payload, "plain"));
	assert(rf_clipboard_rich_payload_set_html(&payload, html, sizeof(html) - 1));
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		image,
		sizeof(image),
		2,
		1,
		2 * 4
	));
	assert(rf_clipboard_rich_payload_has_data(&payload));

	wire = rf_clipboard_rich_payload_serialize(&payload);
	assert(wire != NULL);
	assert(wire->len > 16);
	assert(memcmp(wire->data, "RFCP", 4) == 0);
	assert(read_u16_le(wire->data + 4) == 1);
	assert(read_u16_le(wire->data + 6) == 3);
	assert(read_u32_le(wire->data + 8) == RF_CLIPBOARD_RICH_FORMAT_TEXT);
	assert(read_u64_le(wire->data + 24) == strlen("plain"));

	assert(rf_clipboard_rich_payload_parse(wire->data, wire->len, &parsed));
	assert(g_strcmp0(parsed.text, "plain") == 0);
	assert(parsed.html != NULL);
	assert(parsed.html->len == sizeof(html) - 1);
	assert(memcmp(parsed.html->data, html, sizeof(html) - 1) == 0);
	assert(parsed.image_rgba != NULL);
	assert(parsed.image_width == 2);
	assert(parsed.image_height == 1);
	assert(parsed.image_stride == 2 * 4);
	assert(parsed.image_rgba->len == sizeof(image));
	assert(memcmp(parsed.image_rgba->data, image, sizeof(image)) == 0);
}

static void test_parse_rejects_truncated_entry(void)
{
	const uint8_t wire[] = {
		'R', 'F', 'C', 'P',
		1, 0,
		1, 0,
		RF_CLIPBOARD_RICH_FORMAT_TEXT, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
		4, 0, 0, 0, 0, 0, 0, 0,
		'a', 'b'
	};
	g_auto(RfClipboardRichPayload) parsed;

	rf_clipboard_rich_payload_init(&parsed);
	assert(!rf_clipboard_rich_payload_parse(wire, sizeof(wire), &parsed));
}

static void test_parse_rejects_oversized_payload(void)
{
	uint8_t wire[] = {
		'R', 'F', 'C', 'P',
		1, 0,
		1, 0,
		RF_CLIPBOARD_RICH_FORMAT_HTML, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0
	};
	g_auto(RfClipboardRichPayload) parsed;

	wire[24] = 1;
	wire[27] = 4;
	rf_clipboard_rich_payload_init(&parsed);
	assert(!rf_clipboard_rich_payload_parse(wire, sizeof(wire), &parsed));
}

static void test_wire_equal_compares_serialized_payloads(void)
{
	g_auto(RfClipboardRichPayload) left;
	g_auto(RfClipboardRichPayload) right;
	g_autoptr(GByteArray) left_wire = NULL;
	g_autoptr(GByteArray) same_wire = NULL;
	g_autoptr(GByteArray) different_wire = NULL;

	rf_clipboard_rich_payload_init(&left);
	rf_clipboard_rich_payload_init(&right);
	assert(rf_clipboard_rich_payload_set_text(&left, "same"));
	assert(rf_clipboard_rich_payload_set_text(&right, "same"));
	left_wire = rf_clipboard_rich_payload_serialize(&left);
	same_wire = rf_clipboard_rich_payload_serialize(&right);
	assert(rf_clipboard_rich_wire_equal(left_wire, same_wire));
	assert(rf_clipboard_rich_wire_equal(NULL, NULL));
	assert(!rf_clipboard_rich_wire_equal(left_wire, NULL));
	assert(!rf_clipboard_rich_wire_equal(NULL, same_wire));

	assert(rf_clipboard_rich_payload_set_text(&right, "different"));
	different_wire = rf_clipboard_rich_payload_serialize(&right);
	assert(!rf_clipboard_rich_wire_equal(left_wire, different_wire));
}

int main(void)
{
	test_serialize_and_parse_all_formats();
	test_parse_rejects_truncated_entry();
	test_parse_rejects_oversized_payload();
	test_wire_equal_compares_serialized_payloads();
	return 0;
}
