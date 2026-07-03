#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-rfx.h"

#define WBT_SYNC 0xccc0u
#define WBT_CODEC_VERSIONS 0xccc1u
#define WBT_CHANNELS 0xccc2u
#define WBT_CONTEXT 0xccc3u
#define WBT_FRAME_BEGIN 0xccc4u
#define WBT_FRAME_END 0xccc5u
#define WBT_REGION 0xccc6u
#define WBT_EXTENSION 0xccc7u
#define CBT_TILESET 0xcac2u
#define CBT_TILE 0xcac3u
#define WF_MAGIC 0xcaccaccau
#define PWBT_FRAME_BEGIN 0xccc1u
#define PWBT_FRAME_END 0xccc2u
#define PWBT_REGION 0xccc4u
#define PWBT_TILE_SIMPLE 0xccc5u

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

static uint8_t read_u8(const uint8_t *data)
{
	return data[0];
}

static void append_expected_block(
	uint16_t *blocks,
	size_t *count,
	uint16_t block_type
)
{
	assert(*count < 16);
	blocks[(*count)++] = block_type;
}

static size_t parse_blocks(
	const GByteArray *message,
	uint16_t *blocks,
	size_t block_capacity,
	uint16_t *tile_count
)
{
	size_t offset = 0;
	size_t count = 0;

	*tile_count = 0;
	while (offset < message->len) {
		assert(message->len - offset >= 6);
		const uint16_t block_type = read_u16_le(message->data + offset);
		const uint32_t block_len = read_u32_le(message->data + offset + 2);

		assert(block_len >= 6);
		assert(block_len <= message->len - offset);
		assert(count < block_capacity);
		blocks[count++] = block_type;

		if (block_type == WBT_SYNC) {
			assert(block_len == 12);
			assert(read_u32_le(message->data + offset + 6) == WF_MAGIC);
		} else if (block_type == WBT_EXTENSION) {
			assert(block_len >= 22);
			assert(read_u16_le(message->data + offset + 8) == CBT_TILESET);
			*tile_count = read_u16_le(message->data + offset + 16);
			assert(*tile_count > 0);

			const uint32_t tiles_data_size =
				read_u32_le(message->data + offset + 18);
			size_t tile_offset = offset + 22 + 5;
			size_t tiles_data_end = tile_offset + tiles_data_size;
			assert(read_u8(message->data + offset + 14) == 1);
			assert(tiles_data_end <= offset + block_len);

			for (uint16_t i = 0; i < *tile_count; ++i) {
				assert(tiles_data_end - tile_offset >= 19);
				assert(read_u16_le(message->data + tile_offset) == CBT_TILE);
				const uint32_t tile_len =
					read_u32_le(message->data + tile_offset + 2);
				const uint16_t y_len =
					read_u16_le(message->data + tile_offset + 13);
				const uint16_t cb_len =
					read_u16_le(message->data + tile_offset + 15);
				const uint16_t cr_len =
					read_u16_le(message->data + tile_offset + 17);
				assert(tile_len == (uint32_t)19 + y_len + cb_len + cr_len);
				assert(y_len > 0);
				assert(cb_len > 0);
				assert(cr_len > 0);
				assert(tile_len <= tiles_data_end - tile_offset);
				tile_offset += tile_len;
			}
			assert(tile_offset == tiles_data_end);
		}

		offset += block_len;
	}
	return count;
}

static void assert_region_and_first_tile_are_surface_relative(
	const GByteArray *message
)
{
	size_t offset = 0;
	bool saw_region = false;
	bool saw_tileset = false;

	while (offset < message->len) {
		assert(message->len - offset >= 6);
		const uint16_t block_type = read_u16_le(message->data + offset);
		const uint32_t block_len = read_u32_le(message->data + offset + 2);

		assert(block_len >= 6);
		assert(block_len <= message->len - offset);

		if (block_type == WBT_REGION) {
			assert(read_u16_le(message->data + offset + 11) == 0);
			assert(read_u16_le(message->data + offset + 13) == 0);
			assert(read_u16_le(message->data + offset + 15) == 17);
			assert(read_u16_le(message->data + offset + 17) == 19);
			saw_region = true;
		} else if (block_type == WBT_EXTENSION) {
			const size_t tile_offset = offset + 22 + 5;

			assert(read_u16_le(message->data + tile_offset) == CBT_TILE);
			assert(read_u16_le(message->data + tile_offset + 9) == 0);
			assert(read_u16_le(message->data + tile_offset + 11) == 0);
			saw_tileset = true;
		}

		offset += block_len;
	}

	assert(saw_region);
	assert(saw_tileset);
}

static void assert_progressive_simple_message(
	const GByteArray *message,
	uint16_t expected_x,
	uint16_t expected_y,
	uint16_t expected_width,
	uint16_t expected_height,
	uint16_t expected_tiles,
	uint16_t expected_first_tile_x,
	uint16_t expected_first_tile_y
)
{
	size_t offset = 0;
	bool saw_sync = false;
	bool saw_context = false;
	bool saw_frame_begin = false;
	bool saw_region = false;
	bool saw_frame_end = false;

	while (offset < message->len) {
		assert(message->len - offset >= 6);
		const uint16_t block_type = read_u16_le(message->data + offset);
		const uint32_t block_len = read_u32_le(message->data + offset + 2);

		assert(block_len >= 6);
		assert(block_len <= message->len - offset);

		if (block_type == WBT_SYNC) {
			assert(block_len == 12);
			assert(read_u32_le(message->data + offset + 6) == WF_MAGIC);
			saw_sync = true;
		} else if (block_type == WBT_CONTEXT) {
			assert(block_len == 10);
			assert(read_u8(message->data + offset + 6) == 0);
			assert(read_u16_le(message->data + offset + 7) == 64);
			assert(read_u8(message->data + offset + 9) == 0);
			saw_context = true;
		} else if (block_type == PWBT_FRAME_BEGIN) {
			assert(block_len == 12);
			assert(read_u16_le(message->data + offset + 10) == 1);
			saw_frame_begin = true;
		} else if (block_type == PWBT_REGION) {
			assert(block_len >= 18 + 8 + 5);
			assert(read_u8(message->data + offset + 6) == 64);
			assert(read_u16_le(message->data + offset + 7) == 1);
			assert(read_u8(message->data + offset + 9) == 1);
			assert(read_u8(message->data + offset + 10) == 0);
			assert(read_u8(message->data + offset + 11) == 0);
			assert(read_u16_le(message->data + offset + 12) == expected_tiles);
			assert(read_u16_le(message->data + offset + 18) == expected_x);
			assert(read_u16_le(message->data + offset + 20) == expected_y);
			assert(read_u16_le(message->data + offset + 22) == expected_width);
			assert(read_u16_le(message->data + offset + 24) == expected_height);
			assert(read_u8(message->data + offset + 26) == 0x66);
			assert(read_u8(message->data + offset + 27) == 0x66);
			assert(read_u8(message->data + offset + 28) == 0x77);
			assert(read_u8(message->data + offset + 29) == 0x88);
			assert(read_u8(message->data + offset + 30) == 0x98);

			const uint32_t tiles_data_size =
				read_u32_le(message->data + offset + 14);
			size_t tile_offset = offset + 18 + 8 + 5;
			const size_t tiles_data_end = tile_offset + tiles_data_size;

			assert(tiles_data_end == offset + block_len);
			for (uint16_t i = 0; i < expected_tiles; ++i) {
				assert(tiles_data_end - tile_offset >= 22);
				assert(read_u16_le(message->data + tile_offset) ==
				       PWBT_TILE_SIMPLE);
				const uint32_t tile_len =
					read_u32_le(message->data + tile_offset + 2);
				const uint16_t y_len =
					read_u16_le(message->data + tile_offset + 14);
				const uint16_t cb_len =
					read_u16_le(message->data + tile_offset + 16);
				const uint16_t cr_len =
					read_u16_le(message->data + tile_offset + 18);
				const uint16_t tail_len =
					read_u16_le(message->data + tile_offset + 20);

				assert(read_u8(message->data + tile_offset + 6) == 0);
				assert(read_u8(message->data + tile_offset + 7) == 0);
				assert(read_u8(message->data + tile_offset + 8) == 0);
				if (i == 0) {
					assert(read_u16_le(message->data + tile_offset + 9) ==
					       expected_first_tile_x);
					assert(read_u16_le(message->data + tile_offset + 11) ==
					       expected_first_tile_y);
				}
				assert(read_u8(message->data + tile_offset + 13) == 0);
				assert(tail_len == 0);
				assert(tile_len == (uint32_t)22 + y_len + cb_len + cr_len);
				assert(tile_len <= tiles_data_end - tile_offset);
				tile_offset += tile_len;
			}
			assert(tile_offset == tiles_data_end);
			saw_region = true;
		} else if (block_type == PWBT_FRAME_END) {
			assert(block_len == 6);
			saw_frame_end = true;
		}

		offset += block_len;
	}

	assert(saw_sync);
	assert(saw_context);
	assert(saw_frame_begin);
	assert(saw_region);
	assert(saw_frame_end);
}

static void copy_tileset_quants(
	const GByteArray *message,
	uint8_t quants[5]
)
{
	size_t offset = 0;

	while (offset < message->len) {
		assert(message->len - offset >= 6);
		const uint16_t block_type = read_u16_le(message->data + offset);
		const uint32_t block_len = read_u32_le(message->data + offset + 2);

		assert(block_len >= 6);
		assert(block_len <= message->len - offset);

		if (block_type == WBT_EXTENSION) {
			assert(block_len >= 27);
			assert(read_u16_le(message->data + offset + 8) == CBT_TILESET);
			memcpy(quants, message->data + offset + 22, 5);
			return;
		}

		offset += block_len;
	}

	assert(false);
}

static bool quant_nibbles_are_greater_or_equal(
	const uint8_t baseline[5],
	const uint8_t candidate[5]
)
{
	bool increased = false;

	for (size_t i = 0; i < 5; ++i) {
		const uint8_t baseline_low = baseline[i] & 0x0f;
		const uint8_t baseline_high = baseline[i] >> 4;
		const uint8_t candidate_low = candidate[i] & 0x0f;
		const uint8_t candidate_high = candidate[i] >> 4;

		assert(candidate_low >= baseline_low);
		assert(candidate_high >= baseline_high);
		increased = increased ||
			candidate_low > baseline_low ||
			candidate_high > baseline_high;
	}

	return increased;
}

static void fill_frame(uint8_t *rgba, uint16_t width, uint16_t height)
{
	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			uint8_t *p = rgba + ((size_t)y * width + x) * 4;
			p[0] = (uint8_t)(x * 3 + y);
			p[1] = (uint8_t)(x + y * 5);
			p[2] = (uint8_t)(x * 7 + y * 11);
			p[3] = 0xff;
		}
	}
}

static void assert_blocks(
	const GByteArray *message,
	const uint16_t *expected,
	size_t expected_count,
	uint16_t expected_tiles
)
{
	uint16_t blocks[16] = { 0 };
	uint16_t tile_count = 0;
	const size_t count = parse_blocks(
		message,
		blocks,
		G_N_ELEMENTS(blocks),
		&tile_count
	);

	assert(count == expected_count);
	assert(memcmp(blocks, expected, expected_count * sizeof(uint16_t)) == 0);
	assert(tile_count == expected_tiles);
}

static void test_encode_first_frame_writes_headers_and_tiles(void)
{
	g_autoptr(RfRdpRfxContext) context = rf_rdp_rfx_context_new();
	uint8_t rgba[65 * 65 * 4] = { 0 };
	uint16_t expected[8] = { 0 };
	size_t expected_count = 0;

	assert(context != NULL);
	fill_frame(rgba, 65, 65);

	g_autoptr(GByteArray) message = rf_rdp_rfx_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		65 * 4,
		65,
		65,
		0,
		0,
		65,
		65
	);
	assert(message != NULL);

	append_expected_block(expected, &expected_count, WBT_SYNC);
	append_expected_block(expected, &expected_count, WBT_CONTEXT);
	append_expected_block(expected, &expected_count, WBT_CODEC_VERSIONS);
	append_expected_block(expected, &expected_count, WBT_CHANNELS);
	append_expected_block(expected, &expected_count, WBT_FRAME_BEGIN);
	append_expected_block(expected, &expected_count, WBT_REGION);
	append_expected_block(expected, &expected_count, WBT_EXTENSION);
	append_expected_block(expected, &expected_count, WBT_FRAME_END);
	assert_blocks(message, expected, expected_count, 4);
}

static void test_encode_second_frame_omits_headers(void)
{
	g_autoptr(RfRdpRfxContext) context = rf_rdp_rfx_context_new();
	uint8_t rgba[64 * 64 * 4] = { 0 };
	const uint16_t expected[] = {
		WBT_FRAME_BEGIN,
		WBT_REGION,
		WBT_EXTENSION,
		WBT_FRAME_END
	};

	assert(context != NULL);
	fill_frame(rgba, 64, 64);

	g_autoptr(GByteArray) first = rf_rdp_rfx_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		64 * 4,
		64,
		64,
		0,
		0,
		64,
		64
	);
	assert(first != NULL);

	g_autoptr(GByteArray) second = rf_rdp_rfx_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		64 * 4,
		64,
		64,
		0,
		0,
		64,
		64
	);
	assert(second != NULL);
	assert_blocks(second, expected, G_N_ELEMENTS(expected), 1);
}

static void test_encode_damage_rect_uses_surface_relative_message_coords(void)
{
	g_autoptr(RfRdpRfxContext) context = rf_rdp_rfx_context_new();
	uint8_t rgba[128 * 96 * 4] = { 0 };

	assert(context != NULL);
	fill_frame(rgba, 128, 96);

	g_autoptr(GByteArray) message = rf_rdp_rfx_encode_rgba(
		context,
		rgba,
		sizeof(rgba),
		128 * 4,
		128,
		96,
		70,
		65,
		17,
		19
	);
	assert(message != NULL);
	assert_region_and_first_tile_are_surface_relative(message);
}

static void test_encode_progressive_simple_writes_rdpegfx_blocks(void)
{
	g_autoptr(RfRdpRfxContext) context = rf_rdp_rfx_context_new();
	uint8_t rgba[65 * 65 * 4] = { 0 };

	assert(context != NULL);
	fill_frame(rgba, 65, 65);

	g_autoptr(GByteArray) message = rf_rdp_rfx_encode_progressive_rgba(
		context,
		rgba,
		sizeof(rgba),
		65 * 4,
		65,
		65,
		0,
		0,
		65,
		65
	);

	assert(message != NULL);
	assert_progressive_simple_message(message, 0, 0, 65, 65, 4, 0, 0);
}

static void test_encode_progressive_damage_rect_uses_surface_tile_grid(void)
{
	g_autoptr(RfRdpRfxContext) context = rf_rdp_rfx_context_new();
	uint8_t rgba[128 * 96 * 4] = { 0 };

	assert(context != NULL);
	fill_frame(rgba, 128, 96);

	g_autoptr(GByteArray) message = rf_rdp_rfx_encode_progressive_rgba(
		context,
		rgba,
		sizeof(rgba),
		128 * 4,
		128,
		96,
		70,
		65,
		17,
		19
	);

	assert(message != NULL);
	assert_progressive_simple_message(message, 70, 65, 17, 19, 1, 1, 1);
}

static void test_quality_level_changes_quant_table_and_reduces_size(void)
{
	g_autoptr(RfRdpRfxContext) high = rf_rdp_rfx_context_new();
	g_autoptr(RfRdpRfxContext) low = rf_rdp_rfx_context_new();
	uint8_t rgba[128 * 128 * 4] = { 0 };
	uint8_t high_quants[5] = { 0 };
	uint8_t low_quants[5] = { 0 };

	assert(high != NULL);
	assert(low != NULL);
	fill_frame(rgba, 128, 128);

	rf_rdp_rfx_context_set_thread_count(high, 1);
	rf_rdp_rfx_context_set_thread_count(low, 1);
	rf_rdp_rfx_context_set_quality_level(low, 3, 3);

	g_autoptr(GByteArray) high_message = rf_rdp_rfx_encode_rgba(
		high,
		rgba,
		sizeof(rgba),
		128 * 4,
		128,
		128,
		0,
		0,
		128,
		128
	);
	g_autoptr(GByteArray) low_message = rf_rdp_rfx_encode_rgba(
		low,
		rgba,
		sizeof(rgba),
		128 * 4,
		128,
		128,
		0,
		0,
		128,
		128
	);

	assert(high_message != NULL);
	assert(low_message != NULL);
	copy_tileset_quants(high_message, high_quants);
	copy_tileset_quants(low_message, low_quants);
	assert(quant_nibbles_are_greater_or_equal(high_quants, low_quants));
	assert(low_message->len <= high_message->len);
	assert(rf_rdp_rfx_context_get_quality_level(low) == 3);
}

static void test_parallel_encoding_matches_single_thread_output(void)
{
	g_autoptr(RfRdpRfxContext) single = rf_rdp_rfx_context_new();
	g_autoptr(RfRdpRfxContext) parallel = rf_rdp_rfx_context_new();
	uint8_t rgba[130 * 130 * 4] = { 0 };

	assert(single != NULL);
	assert(parallel != NULL);
	fill_frame(rgba, 130, 130);

	rf_rdp_rfx_context_set_thread_count(single, 1);
	rf_rdp_rfx_context_set_thread_count(parallel, 4);
	rf_rdp_rfx_context_set_quality_level(single, 2, 3);
	rf_rdp_rfx_context_set_quality_level(parallel, 2, 3);

	g_autoptr(GByteArray) single_message = rf_rdp_rfx_encode_rgba(
		single,
		rgba,
		sizeof(rgba),
		130 * 4,
		130,
		130,
		0,
		0,
		130,
		130
	);
	g_autoptr(GByteArray) parallel_message = rf_rdp_rfx_encode_rgba(
		parallel,
		rgba,
		sizeof(rgba),
		130 * 4,
		130,
		130,
		0,
		0,
		130,
		130
	);

	assert(single_message != NULL);
	assert(parallel_message != NULL);
	assert(single_message->len == parallel_message->len);
	assert(memcmp(
		       single_message->data,
		       parallel_message->data,
		       single_message->len
	       ) == 0);
}

int main(void)
{
	test_encode_first_frame_writes_headers_and_tiles();
	test_encode_second_frame_omits_headers();
	test_encode_damage_rect_uses_surface_relative_message_coords();
	test_encode_progressive_simple_writes_rdpegfx_blocks();
	test_encode_progressive_damage_rect_uses_surface_tile_grid();
	test_quality_level_changes_quant_table_and_reduces_size();
	test_parallel_encoding_matches_single_thread_output();
	return 0;
}
