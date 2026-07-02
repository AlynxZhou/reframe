#include <assert.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-gfx.h"
#include "rf-rdp-planar.h"

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
	uint16_t cmd_id
)
{
	assert(length >= RF_RDP_GFX_HEADER_SIZE);
	assert(read_u16_le(data) == cmd_id);
	assert(read_u16_le(data + 2) == 0);
	assert(read_u32_le(data + 4) == length);
}

struct bit_reader {
	const uint8_t *data;
	size_t length;
	size_t bit_index;
	size_t bit_count;
};

static uint32_t read_bits(struct bit_reader *reader, unsigned int count)
{
	uint32_t value = 0;

	assert(reader->bit_index + count <= reader->bit_count);
	for (unsigned int i = 0; i < count; ++i) {
		const size_t byte_index = reader->bit_index / 8;
		const unsigned int bit_offset = 7 - (reader->bit_index % 8);

		value = (value << 1) |
			((reader->data[byte_index] >> bit_offset) & 1);
		reader->bit_index++;
	}
	return value;
}

static void history_write(
	uint8_t *history,
	size_t history_size,
	size_t *history_index,
	const uint8_t *src,
	size_t count
)
{
	for (size_t i = 0; i < count; ++i) {
		history[*history_index] = src[i];
		*history_index = (*history_index + 1) % history_size;
	}
}

static void history_read(
	uint8_t *history,
	size_t history_size,
	size_t *history_index,
	size_t distance,
	uint8_t *dst,
	size_t count
)
{
	for (size_t i = 0; i < count; ++i) {
		const size_t index =
			(*history_index + history_size - distance) % history_size;
		dst[i] = history[index];
		history[*history_index] = dst[i];
		*history_index = (*history_index + 1) % history_size;
	}
}

static size_t literal_from_prefix(uint32_t prefix, unsigned int bits, uint8_t *value)
{
	static const struct {
		unsigned int bits;
		uint32_t prefix;
		uint8_t value;
	} literals[] = {
		{ 5, 24, 0x00 }, { 5, 25, 0x01 },
		{ 6, 52, 0x02 }, { 6, 53, 0x03 }, { 6, 54, 0xff },
		{ 7, 110, 0x04 }, { 7, 111, 0x05 }, { 7, 112, 0x06 },
		{ 7, 113, 0x07 }, { 7, 114, 0x08 }, { 7, 115, 0x09 },
		{ 7, 116, 0x0a }, { 7, 117, 0x0b }, { 7, 118, 0x3a },
		{ 7, 119, 0x3b }, { 7, 120, 0x3c }, { 7, 121, 0x3d },
		{ 7, 122, 0x3e }, { 7, 123, 0x3f }, { 7, 124, 0x40 },
		{ 7, 125, 0x80 }, { 8, 252, 0x0c }, { 8, 253, 0x38 },
		{ 8, 254, 0x39 }, { 8, 255, 0x66 }
	};

	for (size_t i = 0; i < sizeof(literals) / sizeof(literals[0]); ++i) {
		if (literals[i].bits == bits && literals[i].prefix == prefix) {
			*value = literals[i].value;
			return true;
		}
	}
	return false;
}

static bool distance_from_prefix(
	struct bit_reader *reader,
	uint32_t prefix,
	unsigned int bits,
	size_t *distance
)
{
	static const struct {
		unsigned int bits;
		uint32_t prefix;
		unsigned int value_bits;
		uint32_t base;
	} distances[] = {
		{ 5, 17, 5, 0 }, { 5, 18, 7, 32 }, { 5, 19, 9, 160 },
		{ 5, 20, 10, 672 }, { 5, 21, 12, 1696 },
		{ 6, 44, 14, 5792 }, { 6, 45, 15, 22176 },
		{ 7, 92, 18, 54944 }, { 7, 93, 20, 317088 },
		{ 8, 188, 20, 1365664 }, { 8, 189, 21, 2414240 },
		{ 9, 380, 22, 4511392 }, { 9, 381, 23, 8705696 },
		{ 9, 382, 24, 17094304 }
	};

	for (size_t i = 0; i < sizeof(distances) / sizeof(distances[0]); ++i) {
		if (distances[i].bits == bits && distances[i].prefix == prefix) {
			*distance = distances[i].base +
				read_bits(reader, distances[i].value_bits);
			return true;
		}
	}
	return false;
}

static size_t decode_count(struct bit_reader *reader)
{
	if (read_bits(reader, 1) == 0)
		return 3;

	size_t count = 4;
	unsigned int extra = 2;
	while (read_bits(reader, 1) == 1) {
		count *= 2;
		extra++;
	}
	return count + read_bits(reader, extra);
}

static size_t decode_zgfx_segment(
	const uint8_t *segment,
	size_t segment_length,
	uint8_t *history,
	size_t history_size,
	size_t *history_index,
	uint8_t *out,
	size_t out_capacity
)
{
	assert(segment_length >= 2);

	const uint8_t flags = segment[0];
	if ((flags & RF_RDP_GFX_ZGFX_PACKET_COMPRESSED) == 0) {
		assert(segment_length - 1 <= out_capacity);
		memcpy(out, segment + 1, segment_length - 1);
		history_write(
			history,
			history_size,
			history_index,
			segment + 1,
			segment_length - 1
		);
		return segment_length - 1;
	}

	const uint8_t unused_bits = segment[segment_length - 1];
	struct bit_reader reader = {
		segment + 1,
		segment_length - 2,
		0,
		(segment_length - 2) * 8 - unused_bits
	};
	size_t out_length = 0;

	while (reader.bit_index < reader.bit_count) {
		uint32_t prefix = 0;
		for (unsigned int bits = 1; bits <= 9; ++bits) {
			prefix = (prefix << 1) | read_bits(&reader, 1);
			uint8_t literal = 0;
			size_t distance = 0;

			if (bits == 1 && prefix == 0) {
				literal = read_bits(&reader, 8);
				assert(out_length < out_capacity);
				out[out_length++] = literal;
				history_write(
					history,
					history_size,
					history_index,
					&literal,
					1
				);
				break;
			}
			if (literal_from_prefix(prefix, bits, &literal)) {
				assert(out_length < out_capacity);
				out[out_length++] = literal;
				history_write(
					history,
					history_size,
					history_index,
					&literal,
					1
				);
				break;
			}
			if (distance_from_prefix(&reader, prefix, bits, &distance)) {
				const size_t count = decode_count(&reader);
				assert(distance > 0);
				assert(out_capacity - out_length >= count);
				history_read(
					history,
					history_size,
					history_index,
					distance,
					out + out_length,
					count
				);
				out_length += count;
				break;
			}
		}
	}
	return out_length;
}

static size_t decode_zgfx(
	const uint8_t *data,
	size_t length,
	uint8_t *out,
	size_t out_capacity
)
{
	uint8_t *history = calloc(2500000, 1);
	assert(history != NULL);
	size_t history_index = 0;
	size_t out_length = 0;

	if (data[0] == RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE) {
		out_length = decode_zgfx_segment(
			data + 1,
			length - 1,
			history,
			2500000,
			&history_index,
			out,
			out_capacity
		);
	} else {
		assert(data[0] == RF_RDP_GFX_ZGFX_SEGMENTED_MULTIPART);
		const uint16_t segment_count = read_u16_le(data + 1);
		const uint32_t uncompressed_size = read_u32_le(data + 3);
		size_t offset = 7;

		for (uint16_t i = 0; i < segment_count; ++i) {
			const uint32_t segment_length = read_u32_le(data + offset);
			offset += 4;
			out_length += decode_zgfx_segment(
				data + offset,
				segment_length,
				history,
				2500000,
				&history_index,
				out + out_length,
				out_capacity - out_length
			);
			offset += segment_length;
		}
		assert(out_length == uncompressed_size);
	}

	free(history);
	return out_length;
}

static size_t decode_planar_rle_plane(
	const uint8_t *data,
	size_t length,
	uint16_t width,
	uint16_t height,
	uint8_t *plane
)
{
	size_t offset = 0;

	for (uint16_t y = 0; y < height; ++y) {
		uint16_t x = 0;
		int16_t pixel = 0;

		while (x < width) {
			assert(offset < length);
			const uint8_t control = data[offset++];
			uint32_t run_length = control & 0x0f;
			uint32_t raw_bytes = (control >> 4) & 0x0f;

			if (run_length == 1) {
				run_length = raw_bytes + 16;
				raw_bytes = 0;
			} else if (run_length == 2) {
				run_length = raw_bytes + 32;
				raw_bytes = 0;
			}
			assert((uint32_t)x + raw_bytes + run_length <= width);

			while (raw_bytes > 0) {
				assert(offset < length);
				if (y == 0) {
					pixel = data[offset++];
					plane[(size_t)y * width + x] = (uint8_t)pixel;
				} else {
					uint8_t delta = data[offset++];
					if ((delta & 1) != 0)
						pixel = -(int16_t)((delta >> 1) + 1);
					else
						pixel = delta >> 1;
					plane[(size_t)y * width + x] =
						(uint8_t)(plane[(size_t)(y - 1) * width + x] + pixel);
				}
				x++;
				raw_bytes--;
			}

			while (run_length > 0) {
				if (y == 0)
					plane[(size_t)y * width + x] = (uint8_t)pixel;
				else
					plane[(size_t)y * width + x] =
						(uint8_t)(plane[(size_t)(y - 1) * width + x] + pixel);
				x++;
				run_length--;
			}
		}
	}

	return offset;
}

static void test_write_caps_confirm(void)
{
	uint8_t out[32] = { 0 };

	const size_t length = rf_rdp_gfx_write_caps_confirm(
		out,
		sizeof(out),
		RF_RDP_GFX_CAPVERSION_8,
		0
	);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 12);
	assert_header(out, length, RF_RDP_GFX_CMDID_CAPSCONFIRM);
	assert(read_u32_le(out + 8) == RF_RDP_GFX_CAPVERSION_8);
	assert(read_u32_le(out + 12) == 4);
	assert(read_u32_le(out + 16) == 0);
}

static void test_parse_caps_advertise_selects_highest_supported(void)
{
	const uint8_t pdu[] = {
		0x12, 0x00, 0x00, 0x00,
		0x22, 0x00, 0x00, 0x00,
		0x02, 0x00,
		0x04, 0x00, 0x08, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x0a, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x20, 0x00, 0x00, 0x00
	};
	struct rf_rdp_gfx_caps caps = { 0 };

	assert(rf_rdp_gfx_parse_caps_advertise(pdu, sizeof(pdu), &caps));
	assert(caps.count == 2);
	assert(caps.selected_version == RF_RDP_GFX_CAPVERSION_10);
	assert(caps.selected_flags == RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED);
}

static void test_parse_caps_advertise_selects_avc444_capversion(void)
{
	const uint8_t pdu[] = {
		0x12, 0x00, 0x00, 0x00,
		0x22, 0x00, 0x00, 0x00,
		0x02, 0x00,
		0x05, 0x01, 0x08, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x10, 0x00, 0x00, 0x00,
		0x00, 0x06, 0x0a, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	struct rf_rdp_gfx_caps caps = { 0 };

	assert(rf_rdp_gfx_parse_caps_advertise(pdu, sizeof(pdu), &caps));
	assert(caps.count == 2);
	assert(caps.selected_version == RF_RDP_GFX_CAPVERSION_106);
	assert(caps.selected_flags == 0);
	assert(caps.avc420);
}

static void test_parse_caps_advertise_keeps_freerdp_avc420_flag(void)
{
	const uint8_t pdu[] = {
		0x12, 0x00, 0x00, 0x00,
		0x22, 0x00, 0x00, 0x00,
		0x02, 0x00,
		0x05, 0x01, 0x08, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x10, 0x00, 0x00, 0x00,
		0x01, 0x07, 0x0a, 0x00,
		0x04, 0x00, 0x00, 0x00,
		0x02, 0x00, 0x00, 0x00
	};
	struct rf_rdp_gfx_caps caps = { 0 };

	assert(rf_rdp_gfx_parse_caps_advertise(pdu, sizeof(pdu), &caps));
	assert(caps.count == 2);
	assert(caps.selected_version == RF_RDP_GFX_CAPVERSION_107);
	assert(caps.selected_flags == 0x00000002u);
	assert(caps.avc420);
}

static void test_parse_frame_acknowledge(void)
{
	const uint8_t pdu[] = {
		0x0d, 0x00, 0x00, 0x00,
		0x14, 0x00, 0x00, 0x00,
		0x03, 0x00, 0x00, 0x00,
		0x2a, 0x00, 0x00, 0x00,
		0x80, 0x00, 0x00, 0x00
	};
	struct rf_rdp_gfx_frame_ack ack = { 0 };

	assert(rf_rdp_gfx_parse_frame_acknowledge(pdu, sizeof(pdu), &ack));
	assert(ack.queue_depth == 3);
	assert(ack.frame_id == 42);
	assert(ack.total_frames_decoded == 128);
	assert(!rf_rdp_gfx_parse_frame_acknowledge(pdu, sizeof(pdu) - 1, &ack));
	assert(!rf_rdp_gfx_parse_qoe_frame_acknowledge(pdu, sizeof(pdu), NULL));
}

static void test_parse_qoe_frame_acknowledge(void)
{
	const uint8_t pdu[] = {
		0x16, 0x00, 0x00, 0x00,
		0x14, 0x00, 0x00, 0x00,
		0x2a, 0x00, 0x00, 0x00,
		0x78, 0x56, 0x34, 0x12,
		0x11, 0x00,
		0x22, 0x00
	};
	struct rf_rdp_gfx_qoe_frame_ack ack = { 0 };

	assert(rf_rdp_gfx_parse_qoe_frame_acknowledge(pdu, sizeof(pdu), &ack));
	assert(ack.frame_id == 42);
	assert(ack.timestamp == 0x12345678);
	assert(ack.time_diff_se == 0x0011);
	assert(ack.time_diff_edr == 0x0022);
	assert(!rf_rdp_gfx_parse_qoe_frame_acknowledge(pdu, sizeof(pdu) - 1, &ack));
	assert(!rf_rdp_gfx_parse_frame_acknowledge(pdu, sizeof(pdu), NULL));
}

static void test_write_create_surface(void)
{
	uint8_t out[32] = { 0 };

	const size_t length = rf_rdp_gfx_write_create_surface(
		out,
		sizeof(out),
		1,
		1920,
		1080,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888
	);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 7);
	assert_header(out, length, RF_RDP_GFX_CMDID_CREATESURFACE);
	assert(read_u16_le(out + 8) == 1);
	assert(read_u16_le(out + 10) == 1920);
	assert(read_u16_le(out + 12) == 1080);
	assert(out[14] == RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888);
}

static void test_write_map_surface_to_output(void)
{
	uint8_t out[32] = { 0 };

	const size_t length = rf_rdp_gfx_write_map_surface_to_output(
		out, sizeof(out), 1, 0, 0
	);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 12);
	assert_header(out, length, RF_RDP_GFX_CMDID_MAPSURFACETOOUTPUT);
	assert(read_u16_le(out + 8) == 1);
	assert(read_u16_le(out + 10) == 0);
	assert(read_u32_le(out + 12) == 0);
	assert(read_u32_le(out + 16) == 0);
}

static void test_write_reset_graphics(void)
{
	uint8_t out[512] = { 0xff };

	const size_t length = rf_rdp_gfx_write_reset_graphics(
		out, sizeof(out), 1920, 1080
	);
	assert(length == 340);
	assert_header(out, length, RF_RDP_GFX_CMDID_RESETGRAPHICS);
	assert(read_u32_le(out + 8) == 1920);
	assert(read_u32_le(out + 12) == 1080);
	assert(read_u32_le(out + 16) == 1);
	assert(read_u32_le(out + 20) == 0);
	assert(read_u32_le(out + 24) == 0);
	assert(read_u32_le(out + 28) == 1919);
	assert(read_u32_le(out + 32) == 1079);
	assert(read_u32_le(out + 36) == 1);
	assert(out[339] == 0);
}

static void test_write_start_and_end_frame(void)
{
	uint8_t out[32] = { 0 };

	size_t length = rf_rdp_gfx_write_start_frame(out, sizeof(out), 7, 42);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 8);
	assert_header(out, length, RF_RDP_GFX_CMDID_STARTFRAME);
	assert(read_u32_le(out + 8) == 42);
	assert(read_u32_le(out + 12) == 7);

	memset(out, 0, sizeof(out));
	length = rf_rdp_gfx_write_end_frame(out, sizeof(out), 7);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 4);
	assert_header(out, length, RF_RDP_GFX_CMDID_ENDFRAME);
	assert(read_u32_le(out + 8) == 7);
}

static void test_write_wire_to_surface_uncompressed(void)
{
	uint8_t out[64] = { 0 };
	const uint8_t pixels[] = {
		0x11, 0x22, 0x33, 0xff,
		0x44, 0x55, 0x66, 0xff
	};

	const size_t length = rf_rdp_gfx_write_wire_to_surface_1(
		out,
		sizeof(out),
		1,
		RF_RDP_GFX_CODECID_UNCOMPRESSED,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		10,
		20,
		12,
		21,
		pixels,
		sizeof(pixels)
	);

	assert(length == RF_RDP_GFX_HEADER_SIZE + 17 + sizeof(pixels));
	assert_header(out, length, RF_RDP_GFX_CMDID_WIRETOSURFACE_1);
	assert(read_u16_le(out + 8) == 1);
	assert(read_u16_le(out + 10) == RF_RDP_GFX_CODECID_UNCOMPRESSED);
	assert(out[12] == RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888);
	assert(read_u16_le(out + 13) == 10);
	assert(read_u16_le(out + 15) == 20);
	assert(read_u16_le(out + 17) == 12);
	assert(read_u16_le(out + 19) == 21);
	assert(read_u32_le(out + 21) == sizeof(pixels));
	assert(memcmp(out + 25, pixels, sizeof(pixels)) == 0);
}

static void test_write_wire_to_surface_planar_codec_id(void)
{
	uint8_t out[64] = { 0 };
	const uint8_t planar[] = {
		0x20,
		0x10, 0x40,
		0x20, 0x50,
		0x30, 0x60
	};

	const size_t length = rf_rdp_gfx_write_wire_to_surface_1(
		out,
		sizeof(out),
		1,
		RF_RDP_GFX_CODECID_PLANAR,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		10,
		20,
		12,
		21,
		planar,
		sizeof(planar)
	);

	assert(length == RF_RDP_GFX_HEADER_SIZE + 17 + sizeof(planar));
	assert_header(out, length, RF_RDP_GFX_CMDID_WIRETOSURFACE_1);
	assert(read_u16_le(out + 10) == RF_RDP_GFX_CODECID_PLANAR);
	assert(read_u32_le(out + 21) == sizeof(planar));
	assert(memcmp(out + 25, planar, sizeof(planar)) == 0);
}

static void test_write_avc420_bitmap_stream(void)
{
	uint8_t out[64] = { 0 };
	const uint8_t h264[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84
	};

	const size_t length = rf_rdp_gfx_write_avc420_bitmap_stream(
		out,
		sizeof(out),
		10,
		20,
		138,
		92,
		26,
		100,
		h264,
		sizeof(h264)
	);

	assert(length == 4 + 8 + 2 + sizeof(h264));
	assert(read_u32_le(out) == 1);
	assert(read_u16_le(out + 4) == 10);
	assert(read_u16_le(out + 6) == 20);
	assert(read_u16_le(out + 8) == 138);
	assert(read_u16_le(out + 10) == 92);
	assert(out[12] == 26);
	assert(out[13] == 100);
	assert(memcmp(out + 14, h264, sizeof(h264)) == 0);
}

static void test_write_wire_to_surface_avc420(void)
{
	uint8_t out[96] = { 0 };
	uint8_t avc[32] = { 0 };
	const uint8_t h264[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84
	};
	const size_t avc_length = rf_rdp_gfx_write_avc420_bitmap_stream(
		avc,
		sizeof(avc),
		0,
		0,
		128,
		72,
		26,
		100,
		h264,
		sizeof(h264)
	);

	const size_t length = rf_rdp_gfx_write_wire_to_surface_1(
		out,
		sizeof(out),
		1,
		RF_RDP_GFX_CODECID_AVC420,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		0,
		0,
		128,
		72,
		avc,
		avc_length
	);

	assert(avc_length > 0);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 17 + avc_length);
	assert_header(out, length, RF_RDP_GFX_CMDID_WIRETOSURFACE_1);
	assert(read_u16_le(out + 10) == RF_RDP_GFX_CODECID_AVC420);
	assert(read_u32_le(out + 21) == avc_length);
	assert(memcmp(out + 25, avc, avc_length) == 0);
}

static void test_write_avc444_bitmap_stream_single_bitstream(void)
{
	uint8_t out[96] = { 0 };
	const uint8_t h264[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0x99, 0x42
	};
	const size_t avc420_length = 4 + 8 + 2 + sizeof(h264);

	const size_t length = rf_rdp_gfx_write_avc444_bitmap_stream(
		out,
		sizeof(out),
		RF_RDP_GFX_AVC444_LC_SINGLE,
		0,
		0,
		128,
		72,
		26,
		100,
		h264,
		sizeof(h264)
	);

	assert(length == 4 + avc420_length);
	assert(read_u32_le(out) == (avc420_length | (1u << 30)));
	assert(read_u32_le(out + 4) == 1);
	assert(read_u16_le(out + 8) == 0);
	assert(read_u16_le(out + 10) == 0);
	assert(read_u16_le(out + 12) == 128);
	assert(read_u16_le(out + 14) == 72);
	assert(out[16] == 26);
	assert(out[17] == 100);
	assert(memcmp(out + 18, h264, sizeof(h264)) == 0);
}

static void test_write_avc444_bitmap_stream_pair(void)
{
	uint8_t out[128] = { 0 };
	const uint8_t luma[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0x11, 0x22
	};
	const uint8_t chroma[] = {
		0x00, 0x00, 0x00, 0x01, 0x41, 0x33, 0x44, 0x55
	};
	const size_t avc420_luma_length = 4 + 8 + 2 + sizeof(luma);
	const size_t avc420_chroma_length = 4 + 8 + 2 + sizeof(chroma);

	const size_t length = rf_rdp_gfx_write_avc444_bitmap_stream_pair(
		out,
		sizeof(out),
		0,
		0,
		128,
		72,
		26,
		100,
		luma,
		sizeof(luma),
		chroma,
		sizeof(chroma)
	);

	assert(length == 4 + avc420_luma_length + avc420_chroma_length);
	assert(read_u32_le(out) == avc420_luma_length);
	assert(read_u32_le(out + 4) == 1);
	assert(read_u16_le(out + 8) == 0);
	assert(read_u16_le(out + 10) == 0);
	assert(read_u16_le(out + 12) == 128);
	assert(read_u16_le(out + 14) == 72);
	assert(out[16] == 26);
	assert(out[17] == 100);
	assert(memcmp(out + 18, luma, sizeof(luma)) == 0);

	const size_t second = 4 + avc420_luma_length;
	assert(read_u32_le(out + second) == 1);
	assert(read_u16_le(out + second + 4) == 0);
	assert(read_u16_le(out + second + 6) == 0);
	assert(read_u16_le(out + second + 8) == 128);
	assert(read_u16_le(out + second + 10) == 72);
	assert(out[second + 12] == 26);
	assert(out[second + 13] == 100);
	assert(memcmp(out + second + 14, chroma, sizeof(chroma)) == 0);
}

static void test_write_wire_to_surface_avc444(void)
{
	uint8_t out[128] = { 0 };
	uint8_t avc[64] = { 0 };
	const uint8_t h264[] = {
		0x00, 0x00, 0x00, 0x01, 0x65, 0x99, 0x42
	};
	const size_t avc_length = rf_rdp_gfx_write_avc444_bitmap_stream(
		avc,
		sizeof(avc),
		RF_RDP_GFX_AVC444_LC_SINGLE,
		0,
		0,
		128,
		72,
		26,
		100,
		h264,
		sizeof(h264)
	);

	const size_t length = rf_rdp_gfx_write_wire_to_surface_1(
		out,
		sizeof(out),
		1,
		RF_RDP_GFX_CODECID_AVC444,
		RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888,
		0,
		0,
		128,
		72,
		avc,
		avc_length
	);

	assert(avc_length > 0);
	assert(length == RF_RDP_GFX_HEADER_SIZE + 17 + avc_length);
	assert_header(out, length, RF_RDP_GFX_CMDID_WIRETOSURFACE_1);
	assert(read_u16_le(out + 10) == RF_RDP_GFX_CODECID_AVC444);
	assert(read_u32_le(out + 21) == avc_length);
	assert(memcmp(out + 25, avc, avc_length) == 0);
}

static void test_encode_planar_rgba_rect_noalpha_raw_rgb_planes(void)
{
	const uint8_t rgba[] = {
		0x01, 0x02, 0x03, 0xff,
		0x11, 0x12, 0x13, 0xff,
		0x21, 0x22, 0x23, 0xff,
		0x31, 0x32, 0x33, 0xff,
		0x41, 0x42, 0x43, 0xff,
		0x51, 0x52, 0x53, 0xff
	};
	uint8_t planar[1 + 3 * 4] = { 0 };

	const size_t length = rf_rdp_planar_encode_rgba(
		planar,
		sizeof(planar),
		rgba,
		sizeof(rgba),
		3 * 4,
		1,
		0,
		2,
		2
	);

	const uint8_t expected[] = {
		RF_RDP_PLANAR_FORMAT_HEADER_NA,
		0x11, 0x21, 0x41, 0x51,
		0x12, 0x22, 0x42, 0x52,
		0x13, 0x23, 0x43, 0x53
	};
	assert(length == sizeof(expected));
	assert(memcmp(planar, expected, sizeof(expected)) == 0);
}

static void test_encode_planar_rejects_short_buffers(void)
{
	const uint8_t rgba[] = {
		0x01, 0x02, 0x03, 0xff,
		0x11, 0x12, 0x13, 0xff
	};
	uint8_t planar[1 + 3] = { 0 };

	assert(rf_rdp_planar_encode_rgba(
		       planar,
		       sizeof(planar),
		       rgba,
		       sizeof(rgba),
		       2 * 4,
		       0,
		       0,
		       2,
		       1
	       ) == 0);
	assert(rf_rdp_planar_encode_rgba(
		       planar,
		       sizeof(planar),
		       rgba,
		       sizeof(rgba) - 1,
		       2 * 4,
		       1,
		       0,
		       1,
		       1
	       ) == 0);
}

static void test_encode_planar_raw_function_does_not_try_rle(void)
{
	uint8_t rgba[4 * 2 * 4] = { 0 };
	uint8_t planar[1 + 3 * 8] = { 0 };

	for (size_t i = 0; i < sizeof(rgba); i += 4) {
		rgba[i] = 0x10;
		rgba[i + 1] = 0x20;
		rgba[i + 2] = 0x30;
		rgba[i + 3] = 0xff;
	}

	const size_t length = rf_rdp_planar_encode_rgba(
		planar,
		sizeof(planar),
		rgba,
		sizeof(rgba),
		4 * 4,
		0,
		0,
		4,
		2
	);

	assert(length == rf_rdp_planar_rgba_size(4, 2));
	assert(planar[0] == RF_RDP_PLANAR_FORMAT_HEADER_NA);
}

static void test_encode_planar_uses_rle_when_smaller(void)
{
	uint8_t rgba[4 * 2 * 4] = { 0 };
	uint8_t planar[64] = { 0 };
	uint8_t red[8] = { 0 };
	uint8_t green[8] = { 0 };
	uint8_t blue[8] = { 0 };
	const uint8_t expected_red[8] = {
		0x10, 0x10, 0x10, 0x10,
		0x10, 0x10, 0x10, 0x10
	};
	const uint8_t expected_green[8] = {
		0x20, 0x20, 0x20, 0x20,
		0x20, 0x20, 0x20, 0x20
	};
	const uint8_t expected_blue[8] = {
		0x30, 0x30, 0x30, 0x30,
		0x30, 0x30, 0x30, 0x30
	};

	for (size_t i = 0; i < sizeof(rgba); i += 4) {
		rgba[i] = 0x10;
		rgba[i + 1] = 0x20;
		rgba[i + 2] = 0x30;
		rgba[i + 3] = 0xff;
	}

	const size_t length = rf_rdp_planar_encode_rgba_best(
		planar,
		sizeof(planar),
		rgba,
		sizeof(rgba),
		4 * 4,
		0,
		0,
		4,
		2
	);

	assert(length > 0);
	assert(length < rf_rdp_planar_rgba_size(4, 2));
	assert(planar[0] == (RF_RDP_PLANAR_FORMAT_HEADER_NA |
			     RF_RDP_PLANAR_FORMAT_HEADER_RLE));

	size_t offset = 1;
	offset += decode_planar_rle_plane(
		planar + offset, length - offset, 4, 2, red
	);
	offset += decode_planar_rle_plane(
		planar + offset, length - offset, 4, 2, green
	);
	offset += decode_planar_rle_plane(
		planar + offset, length - offset, 4, 2, blue
	);

	assert(offset == length);
	assert(memcmp(red, expected_red, sizeof(red)) == 0);
	assert(memcmp(green, expected_green, sizeof(green)) == 0);
	assert(memcmp(blue, expected_blue, sizeof(blue)) == 0);
}

static void test_write_zgfx_single_uncompressed(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t payload[] = { 0x09, 0x00, 0x00, 0x00 };

	const size_t length = rf_rdp_gfx_write_zgfx_uncompressed(
		out, sizeof(out), payload, sizeof(payload)
	);

	assert(length == sizeof(payload) + 2);
	assert(out[0] == RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE);
	assert(out[1] == RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8);
	assert(memcmp(out + 2, payload, sizeof(payload)) == 0);
}

static void test_write_zgfx_multipart_uncompressed(void)
{
	uint8_t payload[65536] = { 0 };
	uint8_t out[65560] = { 0 };

	payload[0] = 0x11;
	payload[65535] = 0x22;

	const size_t length = rf_rdp_gfx_write_zgfx_uncompressed(
		out, sizeof(out), payload, sizeof(payload)
	);

	assert(length == sizeof(payload) + 7 + 10);
	assert(out[0] == RF_RDP_GFX_ZGFX_SEGMENTED_MULTIPART);
	assert(read_u16_le(out + 1) == 2);
	assert(read_u32_le(out + 3) == sizeof(payload));
	assert(read_u32_le(out + 7) == 65536);
	assert(out[11] == RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8);
	assert(out[12] == 0x11);
	assert(out[65546] == payload[65534]);
	assert(read_u32_le(out + 65547) == 2);
	assert(out[65551] == RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8);
	assert(out[65552] == 0x22);
}

static void test_write_zgfx_compresses_repeated_payload(void)
{
	uint8_t payload[512] = { 0 };
	uint8_t out[1024] = { 0 };
	uint8_t decoded[sizeof(payload)] = { 0 };
	bool compressed = false;

	memset(payload, 0xff, sizeof(payload));
	for (size_t i = 0; i < sizeof(payload); i += 4) {
		payload[i] = 0x20;
		payload[i + 1] = 0x40;
		payload[i + 2] = 0x60;
	}

	const size_t raw_length = rf_rdp_gfx_write_zgfx_uncompressed(
		out, sizeof(out), payload, sizeof(payload)
	);
	memset(out, 0, sizeof(out));
	const size_t length = rf_rdp_gfx_write_zgfx(
		out, sizeof(out), payload, sizeof(payload), &compressed
	);

	assert(compressed);
	assert(length > 0);
	assert(length < raw_length);
	assert(out[0] == RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE);
	assert((out[1] & RF_RDP_GFX_ZGFX_PACKET_COMPRESSED) != 0);
	assert(decode_zgfx(out, length, decoded, sizeof(decoded)) == sizeof(payload));
	assert(memcmp(decoded, payload, sizeof(payload)) == 0);
}

static void test_write_zgfx_falls_back_when_not_smaller(void)
{
	uint8_t payload[64] = { 0 };
	uint8_t out[128] = { 0 };
	bool compressed = true;

	for (size_t i = 0; i < sizeof(payload); ++i)
		payload[i] = (uint8_t)(i * 37 + 11);

	const size_t raw_length = rf_rdp_gfx_write_zgfx_uncompressed(
		out, sizeof(out), payload, sizeof(payload)
	);
	memset(out, 0, sizeof(out));
	const size_t length = rf_rdp_gfx_write_zgfx(
		out, sizeof(out), payload, sizeof(payload), &compressed
	);

	assert(!compressed);
	assert(length == raw_length);
	assert(out[0] == RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE);
	assert(out[1] == RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8);
	assert(memcmp(out + 2, payload, sizeof(payload)) == 0);
}

int main(void)
{
	test_write_caps_confirm();
	test_parse_caps_advertise_selects_highest_supported();
	test_parse_caps_advertise_selects_avc444_capversion();
	test_parse_caps_advertise_keeps_freerdp_avc420_flag();
	test_parse_frame_acknowledge();
	test_parse_qoe_frame_acknowledge();
	test_write_create_surface();
	test_write_map_surface_to_output();
	test_write_reset_graphics();
	test_write_start_and_end_frame();
	test_write_wire_to_surface_uncompressed();
	test_write_wire_to_surface_planar_codec_id();
	test_write_avc420_bitmap_stream();
	test_write_wire_to_surface_avc420();
	test_write_avc444_bitmap_stream_single_bitstream();
	test_write_avc444_bitmap_stream_pair();
	test_write_wire_to_surface_avc444();
	test_encode_planar_rgba_rect_noalpha_raw_rgb_planes();
	test_encode_planar_rejects_short_buffers();
	test_encode_planar_raw_function_does_not_try_rle();
	test_encode_planar_uses_rle_when_smaller();
	test_write_zgfx_single_uncompressed();
	test_write_zgfx_multipart_uncompressed();
	test_write_zgfx_compresses_repeated_payload();
	test_write_zgfx_falls_back_when_not_smaller();
	return 0;
}
