#include "rf-rdp-planar.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

static uint8_t planar_control_byte(size_t run_length, size_t raw_bytes)
{
	return (uint8_t)((run_length & 0x0f) | ((raw_bytes & 0x0f) << 4));
}

static size_t write_rle_bytes(
	const uint8_t *input,
	size_t raw_bytes,
	size_t run_length,
	uint8_t *output,
	size_t output_capacity
)
{
	const uint8_t *in = input;
	uint8_t *out = output;

	if (raw_bytes == 0 && run_length == 0)
		return 0;

	if (run_length < 3) {
		raw_bytes += run_length;
		run_length = 0;
	}

	while (raw_bytes > 0) {
		uint8_t control = 0;
		size_t bytes_to_write = 0;

		if (raw_bytes < 16) {
			if (run_length > 15) {
				if (run_length < 18) {
					control = planar_control_byte(13, raw_bytes);
					run_length -= 13;
					raw_bytes = 0;
				} else {
					control = planar_control_byte(15, raw_bytes);
					run_length -= 15;
					raw_bytes = 0;
				}
			} else {
				control = planar_control_byte(run_length, raw_bytes);
				run_length = 0;
				raw_bytes = 0;
			}
		} else {
			control = planar_control_byte(0, 15);
			raw_bytes -= 15;
		}

		if (output_capacity < 1)
			return 0;
		*out++ = control;
		output_capacity--;

		bytes_to_write = control >> 4;
		if (bytes_to_write > 0) {
			if (output_capacity < bytes_to_write)
				return 0;
			memcpy(out, in, bytes_to_write);
			out += bytes_to_write;
			in += bytes_to_write;
			output_capacity -= bytes_to_write;
		}
	}

	while (run_length > 0) {
		uint8_t control = 0;

		if (run_length > 47) {
			if (run_length < 50) {
				control = planar_control_byte(2, 13);
				run_length -= 45;
			} else {
				control = planar_control_byte(2, 15);
				run_length -= 47;
			}
		} else if (run_length > 31) {
			control = planar_control_byte(2, run_length - 32);
			run_length = 0;
		} else if (run_length > 15) {
			control = planar_control_byte(1, run_length - 16);
			run_length = 0;
		} else {
			control = planar_control_byte(run_length, 0);
			run_length = 0;
		}

		if (output_capacity < 1)
			return 0;
		*out++ = control;
		output_capacity--;
	}

	return (size_t)(out - output);
}

static size_t encode_rle_row(
	const uint8_t *input,
	size_t input_length,
	uint8_t *output,
	size_t output_capacity
)
{
	uint8_t symbol = 0;
	const uint8_t *in = input;
	const uint8_t *bytes = NULL;
	uint8_t *out = output;
	size_t raw_bytes = 0;
	size_t run_length = 0;
	size_t remaining = input_length;

	if (output_capacity == 0)
		return 0;

	while (remaining > 0) {
		const bool symbol_match = symbol == *in;
		symbol = *in;
		in++;
		remaining--;

		if (run_length > 0 && !symbol_match) {
			if (run_length < 3) {
				raw_bytes += run_length;
				run_length = 0;
			} else {
				const size_t bytes_length = raw_bytes + run_length + 1;
				size_t bytes_written = 0;

				if ((size_t)(in - input) < bytes_length)
					return 0;
				bytes = in - bytes_length;
				bytes_written = write_rle_bytes(
					bytes,
					raw_bytes,
					run_length,
					out,
					output_capacity
				);
				if (bytes_written == 0 || bytes_written > output_capacity)
					return 0;
				out += bytes_written;
				output_capacity -= bytes_written;
				raw_bytes = 0;
				run_length = 0;
			}
		}

		if (symbol_match)
			run_length++;
		else
			raw_bytes++;
	}

	if (raw_bytes > 0 || run_length > 0) {
		const size_t bytes_length = raw_bytes + run_length;
		size_t bytes_written = 0;

		if ((size_t)(in - input) < bytes_length)
			return 0;
		bytes = in - bytes_length;
		bytes_written = write_rle_bytes(
			bytes,
			raw_bytes,
			run_length,
			out,
			output_capacity
		);
		if (bytes_written == 0 || bytes_written > output_capacity)
			return 0;
		out += bytes_written;
	}

	return (size_t)(out - output);
}

static void delta_encode_plane(
	const uint8_t *plane,
	uint16_t width,
	uint16_t height,
	uint8_t *delta
)
{
	memcpy(delta, plane, width);

	for (uint16_t y = 1; y < height; ++y) {
		const size_t row_offset = (size_t)y * width;
		const uint8_t *src = plane + row_offset;
		const uint8_t *previous = src - width;
		uint8_t *dst = delta + row_offset;

		for (uint16_t x = 0; x < width; ++x) {
			const int raw_delta = (int)src[x] - (int)previous[x];
			const int8_t signed_delta = (int8_t)raw_delta;

			if (signed_delta >= 0)
				dst[x] = (uint8_t)((uint8_t)signed_delta << 1);
			else
				dst[x] = (uint8_t)(((-signed_delta) << 1) - 1);
		}
	}
}

static size_t encode_plane_rle(
	const uint8_t *plane,
	uint16_t width,
	uint16_t height,
	uint8_t *delta,
	uint8_t *output,
	size_t output_capacity
)
{
	size_t output_length = 0;

	delta_encode_plane(plane, width, height, delta);
	for (uint16_t y = 0; y < height; ++y) {
		const size_t row_length = encode_rle_row(
			delta + (size_t)y * width,
			width,
			output + output_length,
			output_capacity - output_length
		);
		if (row_length == 0 || row_length > output_capacity - output_length)
			return 0;
		output_length += row_length;
	}

	return output_length;
}

static size_t try_encode_rle(
	uint8_t *data,
	size_t raw_length,
	uint16_t width,
	uint16_t height
)
{
	const size_t pixel_count = (size_t)width * height;
	const uint8_t *red = data + 1;
	const uint8_t *green = red + pixel_count;
	const uint8_t *blue = green + pixel_count;
	size_t rle_capacity = raw_length - 1;
	uint8_t *delta = NULL;
	uint8_t *rle = NULL;
	size_t rle_length = 0;
	size_t encoded_length = 0;
	size_t plane_length = 0;

	delta = malloc(pixel_count);
	rle = malloc(rle_capacity);
	if (delta == NULL || rle == NULL)
		goto out;

	plane_length = encode_plane_rle(
		red,
		width,
		height,
		delta,
		rle + rle_length,
		rle_capacity - rle_length
	);
	if (plane_length == 0)
		goto out;
	rle_length += plane_length;

	plane_length = encode_plane_rle(
		green,
		width,
		height,
		delta,
		rle + rle_length,
		rle_capacity - rle_length
	);
	if (plane_length == 0)
		goto out;
	rle_length += plane_length;

	plane_length = encode_plane_rle(
		blue,
		width,
		height,
		delta,
		rle + rle_length,
		rle_capacity - rle_length
	);
	if (plane_length == 0)
		goto out;
	rle_length += plane_length;

	if (rle_length + 1 >= raw_length)
		goto out;

	data[0] = RF_RDP_PLANAR_FORMAT_HEADER_NA | RF_RDP_PLANAR_FORMAT_HEADER_RLE;
	memcpy(data + 1, rle, rle_length);
	encoded_length = rle_length + 1;

out:
	free(delta);
	free(rle);
	return encoded_length;
}

size_t rf_rdp_planar_rgba_size(uint16_t width, uint16_t height)
{
	if (width == 0 || height == 0)
		return 0;

	const size_t pixel_count = (size_t)width * height;
	if (pixel_count > (SIZE_MAX - 1) / 3)
		return 0;
	return 1 + pixel_count * 3;
}

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
)
{
	if (data == NULL || rgba == NULL || width == 0 || height == 0)
		return 0;

	const size_t row_bytes = (size_t)width * 4;
	if (row_bytes / 4 != width)
		return 0;

	const size_t x_bytes = (size_t)x * 4;
	if (x_bytes > SIZE_MAX - row_bytes)
		return 0;

	const size_t required_row_bytes = x_bytes + row_bytes;
	if (rgba_stride < required_row_bytes)
		return 0;

	const size_t last_row = (size_t)y + height - 1;
	if (last_row > (SIZE_MAX - required_row_bytes) / rgba_stride)
		return 0;

	const size_t required_length = last_row * rgba_stride + required_row_bytes;
	if (required_length > rgba_length)
		return 0;

	const size_t output_length = rf_rdp_planar_rgba_size(width, height);
	if (output_length == 0 || capacity < output_length)
		return 0;

	const size_t pixel_count = (size_t)width * height;
	uint8_t *red = data + 1;
	uint8_t *green = red + pixel_count;
	uint8_t *blue = green + pixel_count;

	data[0] = RF_RDP_PLANAR_FORMAT_HEADER_NA;
	for (uint16_t row = 0; row < height; ++row) {
		const uint8_t *src =
			rgba + ((size_t)y + row) * rgba_stride + x_bytes;
		const size_t plane_offset = (size_t)row * width;

		for (uint16_t column = 0; column < width; ++column) {
			const size_t src_offset = (size_t)column * 4;
			const size_t dst_offset = plane_offset + column;

			red[dst_offset] = src[src_offset];
			green[dst_offset] = src[src_offset + 1];
			blue[dst_offset] = src[src_offset + 2];
		}
	}

	return output_length;
}

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
)
{
	const size_t raw_length = rf_rdp_planar_encode_rgba(
		data,
		capacity,
		rgba,
		rgba_length,
		rgba_stride,
		x,
		y,
		width,
		height
	);
	if (raw_length == 0)
		return 0;

	const size_t rle_length = try_encode_rle(data, raw_length, width, height);
	if (rle_length > 0)
		return rle_length;
	return raw_length;
}
