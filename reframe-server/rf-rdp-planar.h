#ifndef __RF_RDP_PLANAR_H__
#define __RF_RDP_PLANAR_H__

#include <stddef.h>
#include <stdint.h>

#define RF_RDP_PLANAR_FORMAT_HEADER_CS (1u << 3)
#define RF_RDP_PLANAR_FORMAT_HEADER_RLE (1u << 4)
#define RF_RDP_PLANAR_FORMAT_HEADER_NA (1u << 5)
#define RF_RDP_PLANAR_FORMAT_HEADER_CLL_MASK 0x07u

size_t rf_rdp_planar_rgba_size(uint16_t width, uint16_t height);
size_t rf_rdp_planar_encode_rgba(
	uint8_t *data,
	size_t capacity,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
);
size_t rf_rdp_planar_encode_rgba_best(
	uint8_t *data,
	size_t capacity,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
);

#endif
