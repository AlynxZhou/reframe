#include <stdbool.h>
#include <string.h>

#include "rf-rdp-nsc.h"

#define NSC_COLOR_LOSS_LEVEL 3u
#define NSC_CHROMA_SUBSAMPLING_LEVEL 1u

struct rf_rdp_nsc_context {
	uint8_t *planes[5];
	size_t plane_capacity;
	uint32_t org_byte_count[4];
	uint32_t plane_byte_count[4];
};

static uint32_t round_up_to(uint32_t value, uint32_t multiple)
{
	return ((value + multiple - 1) / multiple) * multiple;
}

static int16_t arithmetic_shift_right(int16_t value, unsigned int bits)
{
	if (value >= 0)
		return value >> bits;
	return -(((-value) + ((1 << bits) - 1)) >> bits);
}

static void append_u8(GByteArray *array, uint8_t value)
{
	g_byte_array_append(array, &value, 1);
}

static void append_u16_le(GByteArray *array, uint16_t value)
{
	append_u8(array, value & 0xff);
	append_u8(array, value >> 8);
}

static void append_u32_le(GByteArray *array, uint32_t value)
{
	append_u8(array, value & 0xff);
	append_u8(array, (value >> 8) & 0xff);
	append_u8(array, (value >> 16) & 0xff);
	append_u8(array, (value >> 24) & 0xff);
}

static bool initialize_planes(
	RfRdpNscContext *context,
	uint16_t width,
	uint16_t height,
	uint32_t *temp_width,
	uint32_t *temp_height
)
{
	*temp_width = round_up_to(width, 8);
	*temp_height = round_up_to(height, 2);
	if (*temp_width == 0 || *temp_height == 0 ||
	    *temp_width > (UINT32_MAX - 16) / *temp_height)
		return false;

	const size_t length = (size_t)(*temp_width * *temp_height) + 16;
	if (length > context->plane_capacity) {
		for (size_t i = 0; i < G_N_ELEMENTS(context->planes); ++i)
			context->planes[i] = g_realloc_n(context->planes[i], length, 1);
		context->plane_capacity = length;
	}
	for (size_t i = 0; i < G_N_ELEMENTS(context->planes); ++i)
		memset(context->planes[i], 0, context->plane_capacity);

	context->org_byte_count[0] = *temp_width * height;
	context->org_byte_count[1] = *temp_width * *temp_height / 4;
	context->org_byte_count[2] = *temp_width * *temp_height / 4;
	context->org_byte_count[3] = (uint32_t)width * height;
	return true;
}

static bool validate_input(
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t width,
	uint16_t height
)
{
	if (rgba == NULL || width == 0 || height == 0)
		return false;

	const size_t row_bytes = (size_t)width * 4;
	if (rgba_stride < row_bytes)
		return false;
	if ((size_t)height > SIZE_MAX / rgba_stride)
		return false;
	if (((size_t)height - 1) * rgba_stride + row_bytes > rgba_length)
		return false;
	return true;
}

static void encode_argb_to_aycocg(
	RfRdpNscContext *context,
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t width,
	uint16_t height,
	uint32_t temp_width
)
{
	for (uint16_t y = 0; y < height; ++y) {
		const uint8_t *src = rgba + (size_t)(height - 1 - y) * rgba_stride;
		uint8_t *yplane = context->planes[0] + (size_t)y * temp_width;
		uint8_t *coplane = context->planes[1] + (size_t)y * temp_width;
		uint8_t *cgplane = context->planes[2] + (size_t)y * temp_width;
		uint8_t *aplane = context->planes[3] + (size_t)y * width;

		for (uint16_t x = 0; x < width; ++x) {
			const int16_t r = src[0];
			const int16_t g = src[1];
			const int16_t b = src[2];
			const uint8_t a = src[3];
			src += 4;

			*yplane++ = (uint8_t)((r >> 2) + (g >> 1) + (b >> 2));
			*coplane++ = (uint8_t)arithmetic_shift_right(
				r - b,
				NSC_COLOR_LOSS_LEVEL
			);
			*cgplane++ = (uint8_t)arithmetic_shift_right(
				-(r >> 1) + g - (b >> 1),
				NSC_COLOR_LOSS_LEVEL
			);
			*aplane++ = a;
		}

		if ((width % 2) == 1) {
			*yplane = *(yplane - 1);
			*coplane = *(coplane - 1);
			*cgplane = *(cgplane - 1);
		}
	}

	if ((height % 2) == 1) {
		memcpy(
			context->planes[0] + (size_t)height * temp_width,
			context->planes[0] + (size_t)(height - 1) * temp_width,
			temp_width
		);
		memcpy(
			context->planes[1] + (size_t)height * temp_width,
			context->planes[1] + (size_t)(height - 1) * temp_width,
			temp_width
		);
		memcpy(
			context->planes[2] + (size_t)height * temp_width,
			context->planes[2] + (size_t)(height - 1) * temp_width,
			temp_width
		);
	}
}

static void subsample_chroma(RfRdpNscContext *context, uint32_t temp_width, uint32_t temp_height)
{
	const uint32_t dst_width = temp_width / 2;

	for (uint32_t y = 0; y < temp_height / 2; ++y) {
		uint8_t *co_dst = context->planes[1] + (size_t)y * dst_width;
		uint8_t *cg_dst = context->planes[2] + (size_t)y * dst_width;
		const int8_t *co_src0 = (int8_t *)context->planes[1] +
					(size_t)(y * 2) * temp_width;
		const int8_t *co_src1 = co_src0 + temp_width;
		const int8_t *cg_src0 = (int8_t *)context->planes[2] +
					(size_t)(y * 2) * temp_width;
		const int8_t *cg_src1 = cg_src0 + temp_width;

		for (uint32_t x = 0; x < dst_width; ++x) {
			const int16_t co = (int16_t)co_src0[0] + co_src0[1] +
					   co_src1[0] + co_src1[1];
			const int16_t cg = (int16_t)cg_src0[0] + cg_src0[1] +
					   cg_src1[0] + cg_src1[1];
			*co_dst++ = (uint8_t)arithmetic_shift_right(co, 2);
			*cg_dst++ = (uint8_t)arithmetic_shift_right(cg, 2);
			co_src0 += 2;
			co_src1 += 2;
			cg_src0 += 2;
			cg_src1 += 2;
		}
	}
}

static uint32_t nsc_rle_encode(const uint8_t *in, uint8_t *out, uint32_t original_size)
{
	uint32_t left = original_size;
	uint32_t run_length = 1;
	uint32_t plane_size = 0;

	while (left > 4 && plane_size < original_size - 4) {
		if (left > 5 && *in == *(in + 1)) {
			run_length++;
		} else if (run_length == 1) {
			*out++ = *in;
			plane_size++;
		} else if (run_length < 256) {
			*out++ = *in;
			*out++ = *in;
			*out++ = run_length - 2;
			run_length = 1;
			plane_size += 3;
		} else {
			*out++ = *in;
			*out++ = *in;
			*out++ = 0xff;
			*out++ = run_length & 0xff;
			*out++ = (run_length >> 8) & 0xff;
			*out++ = (run_length >> 16) & 0xff;
			*out++ = (run_length >> 24) & 0xff;
			run_length = 1;
			plane_size += 7;
		}

		in++;
		left--;
	}

	if (original_size >= 4 && plane_size < original_size - 4)
		memcpy(out, in, 4);
	plane_size += 4;
	return plane_size;
}

static void compress_planes(RfRdpNscContext *context)
{
	for (size_t i = 0; i < G_N_ELEMENTS(context->plane_byte_count); ++i) {
		const uint32_t original_size = context->org_byte_count[i];
		uint32_t plane_size = 0;

		if (original_size > 0)
			plane_size = nsc_rle_encode(
				context->planes[i],
				context->planes[4],
				original_size
			);
		if (plane_size > 0 && plane_size < original_size) {
			memcpy(context->planes[i], context->planes[4], plane_size);
		} else {
			plane_size = original_size;
		}
		context->plane_byte_count[i] = plane_size;
	}
}

RfRdpNscContext *rf_rdp_nsc_context_new(void)
{
	return g_new0(RfRdpNscContext, 1);
}

void rf_rdp_nsc_context_free(RfRdpNscContext *context)
{
	if (context == NULL)
		return;
	for (size_t i = 0; i < G_N_ELEMENTS(context->planes); ++i)
		g_free(context->planes[i]);
	g_free(context);
}

GByteArray *rf_rdp_nsc_encode_rgba(
	RfRdpNscContext *context,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t width,
	uint16_t height
)
{
	uint32_t temp_width = 0;
	uint32_t temp_height = 0;
	uint64_t total_plane_bytes = 0;

	if (context == NULL ||
	    !validate_input(rgba, rgba_length, rgba_stride, width, height))
		return NULL;
	if (!initialize_planes(context, width, height, &temp_width, &temp_height))
		return NULL;

	encode_argb_to_aycocg(
		context,
		rgba,
		rgba_stride,
		width,
		height,
		temp_width
	);
	subsample_chroma(context, temp_width, temp_height);
	compress_planes(context);

	for (size_t i = 0; i < G_N_ELEMENTS(context->plane_byte_count); ++i)
		total_plane_bytes += context->plane_byte_count[i];
	if (total_plane_bytes > G_MAXUINT - 20)
		return NULL;

	GByteArray *encoded = g_byte_array_sized_new(20 + total_plane_bytes);
	append_u32_le(encoded, context->plane_byte_count[0]);
	append_u32_le(encoded, context->plane_byte_count[1]);
	append_u32_le(encoded, context->plane_byte_count[2]);
	append_u32_le(encoded, context->plane_byte_count[3]);
	append_u8(encoded, NSC_COLOR_LOSS_LEVEL);
	append_u8(encoded, NSC_CHROMA_SUBSAMPLING_LEVEL);
	append_u16_le(encoded, 0);
	for (size_t i = 0; i < G_N_ELEMENTS(context->plane_byte_count); ++i)
		g_byte_array_append(
			encoded,
			context->planes[i],
			context->plane_byte_count[i]
		);
	return encoded;
}
