#include <assert.h>
#include <stdint.h>
#include <string.h>

#include <glib.h>

#include "rf-rdp-nsc.h"

static uint32_t read_u32_le(const uint8_t *data)
{
	return data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void test_encode_opaque_black_pixel(void)
{
	const uint8_t rgba[] = { 0x00, 0x00, 0x00, 0xff };
	const uint8_t expected[] = {
		0x07, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00,
		0x03, 0x01, 0x00, 0x00,
		0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0xff
	};
	RfRdpNscContext *context = rf_rdp_nsc_context_new();

	assert(context != NULL);
	GByteArray *encoded = rf_rdp_nsc_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		4,
		1,
		1
	);
	assert(encoded != NULL);
	assert(encoded->len == sizeof(expected));
	assert(memcmp(encoded->data, expected, sizeof(expected)) == 0);

	g_byte_array_unref(encoded);
	rf_rdp_nsc_context_free(context);
}

static void test_encode_mixed_rect_is_deterministic(void)
{
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0xff, 0x00, 0xff,
		0x00, 0x00, 0xff, 0xff,
		0x7f, 0x7f, 0x7f, 0xff
	};
	RfRdpNscContext *context = rf_rdp_nsc_context_new();

	assert(context != NULL);
	GByteArray *first = rf_rdp_nsc_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		8,
		2,
		2
	);
	GByteArray *second = rf_rdp_nsc_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		8,
		2,
		2
	);

	assert(first != NULL);
	assert(second != NULL);
	assert(first->len == second->len);
	assert(memcmp(first->data, second->data, first->len) == 0);
	assert(first->len >= 20);
	assert(first->data[16] == 3);
	assert(first->data[17] == 1);
	assert(first->data[18] == 0);
	assert(first->data[19] == 0);

	const uint32_t y = read_u32_le(first->data);
	const uint32_t co = read_u32_le(first->data + 4);
	const uint32_t cg = read_u32_le(first->data + 8);
	const uint32_t a = read_u32_le(first->data + 12);
	assert((size_t)y + co + cg + a == first->len - 20);
	assert(y <= 16);
	assert(co <= 4);
	assert(cg <= 4);
	assert(a <= 4);

	g_byte_array_unref(first);
	g_byte_array_unref(second);
	rf_rdp_nsc_context_free(context);
}

static void test_rejects_invalid_input(void)
{
	const uint8_t rgba[] = { 0x00, 0x00, 0x00, 0xff };
	RfRdpNscContext *context = rf_rdp_nsc_context_new();

	assert(context != NULL);
	assert(rf_rdp_nsc_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		3,
		1,
		1
	) == NULL);
	assert(rf_rdp_nsc_encode_rgba(
		context,
		rgba,
		0,
		4,
		1,
		1
	) == NULL);
	rf_rdp_nsc_context_free(context);
}

int main(void)
{
	test_encode_opaque_black_pixel();
	test_encode_mixed_rect_is_deterministic();
	test_rejects_invalid_input();
	return 0;
}
