#include "rf-rdp-rfx.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define RF_RFX_TILE_SIZE 64u
#define RF_RFX_COEFF_COUNT (RF_RFX_TILE_SIZE * RF_RFX_TILE_SIZE)
#define RF_RFX_COMPONENT_CAPACITY 4096u
#define RF_RFX_QUANT_COUNT 10u

#define RF_RFX_WBT_SYNC 0xccc0u
#define RF_RFX_WBT_CODEC_VERSIONS 0xccc1u
#define RF_RFX_WBT_CHANNELS 0xccc2u
#define RF_RFX_WBT_CONTEXT 0xccc3u
#define RF_RFX_WBT_FRAME_BEGIN 0xccc4u
#define RF_RFX_WBT_FRAME_END 0xccc5u
#define RF_RFX_WBT_REGION 0xccc6u
#define RF_RFX_WBT_EXTENSION 0xccc7u
#define RF_RFX_CBT_REGION 0xcac1u
#define RF_RFX_CBT_TILESET 0xcac2u
#define RF_RFX_CBT_TILE 0xcac3u
#define RF_RFX_WF_MAGIC 0xcaccaccau
#define RF_RFX_WF_VERSION_1_0 0x0100u
#define RF_RFX_CT_TILE_64X64 0x0040u
#define RF_RFX_COL_CONV_ICT 0x1u
#define RF_RFX_XFORM_DWT_53_A 0x1u
#define RF_RFX_ENTROPY_RLGR1 0x01u
#define RF_RFX_SCALAR_QUANTIZATION 0x1u

#define RF_RFX_KP_MAX 80u
#define RF_RFX_LSGR 3u
#define RF_RFX_UP_GR 4
#define RF_RFX_DN_GR 6
#define RF_RFX_UQ_GR 3
#define RF_RFX_DQ_GR 3
#define RF_RFX_PARALLEL_MIN_TILES 8u
#define RF_RFX_MAX_THREADS 16u

struct rf_rdp_rfx_context {
	GThreadPool *thread_pool;
	unsigned int thread_count;
	unsigned int quality_level;
	bool send_headers;
	uint16_t width;
	uint16_t height;
	uint32_t frame_idx;
	uint32_t quants[RF_RFX_QUANT_COUNT];
};

struct rfx_bit_writer {
	uint8_t *data;
	size_t capacity;
	size_t byte_pos;
	uint8_t bits_left;
	bool failed;
};

struct rfx_tile_job_group {
	GMutex mutex;
	GCond cond;
	unsigned int pending;
};

struct rfx_tile_job {
	struct rfx_tile_job_group *group;
	const uint8_t *rgba;
	size_t rgba_stride;
	uint16_t frame_width;
	uint16_t frame_height;
	uint16_t source_x;
	uint16_t source_y;
	uint16_t tile_x_idx;
	uint16_t tile_y_idx;
	const uint32_t *quants;
	GByteArray *encoded;
};

static const uint32_t RFX_DEFAULT_QUANTS[RF_RFX_QUANT_COUNT] = {
	6, 6, 6, 6, 7, 7, 8, 8, 8, 9
};

static const uint32_t RFX_QUALITY_MAX_DELTA[RF_RFX_QUANT_COUNT] = {
	1, 2, 2, 3, 3, 4, 4, 5, 5, 6
};

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

static void patch_u32_le(GByteArray *array, size_t offset, uint32_t value)
{
	array->data[offset] = value & 0xff;
	array->data[offset + 1] = (value >> 8) & 0xff;
	array->data[offset + 2] = (value >> 16) & 0xff;
	array->data[offset + 3] = (value >> 24) & 0xff;
}

static size_t begin_block(GByteArray *array, uint16_t block_type)
{
	const size_t offset = array->len;

	append_u16_le(array, block_type);
	append_u32_le(array, 0);
	return offset;
}

static bool end_block(GByteArray *array, size_t offset)
{
	const size_t length = array->len - offset;

	if (length > UINT32_MAX)
		return false;
	patch_u32_le(array, offset + 2, (uint32_t)length);
	return true;
}

static int16_t cast_i16(int value)
{
	return (int16_t)value;
}

static uint32_t update_param(uint32_t *param, int delta)
{
	if (delta < 0) {
		const uint32_t decrement = (uint32_t)-delta;
		*param = decrement > *param ? 0 : *param - decrement;
	} else {
		*param += (uint32_t)delta;
	}
	if (*param > RF_RFX_KP_MAX)
		*param = RF_RFX_KP_MAX;
	return *param >> RF_RFX_LSGR;
}

static void bit_writer_put_bits(
	struct rfx_bit_writer *writer,
	uint32_t value,
	uint32_t bits
)
{
	if (writer->failed)
		return;

	while (bits > 0) {
		if (writer->byte_pos >= writer->capacity) {
			writer->failed = true;
			return;
		}

		uint32_t count = bits;
		if (count > writer->bits_left)
			count = writer->bits_left;
		const uint32_t shift = bits - count;
		const uint8_t mask = (uint8_t)((1u << count) - 1u);
		writer->data[writer->byte_pos] |=
			(uint8_t)(((value >> shift) & mask) <<
				  (writer->bits_left - count));
		writer->bits_left -= count;
		bits -= count;
		if (writer->bits_left == 0) {
			writer->bits_left = 8;
			writer->byte_pos++;
		}
	}
}

static void bit_writer_put_repeated_bit(
	struct rfx_bit_writer *writer,
	uint32_t count,
	uint8_t bit
)
{
	for (uint32_t i = 0; i < count; ++i)
		bit_writer_put_bits(writer, bit ? 1u : 0u, 1);
}

static void bit_writer_flush(struct rfx_bit_writer *writer)
{
	if (writer->bits_left != 8)
		bit_writer_put_bits(writer, 0, 8 - writer->bits_left);
}

static size_t bit_writer_length(const struct rfx_bit_writer *writer)
{
	if (writer->failed)
		return 0;
	return writer->bits_left == 8 ? writer->byte_pos : writer->byte_pos + 1;
}

static uint32_t mag_sign(int value)
{
	return value >= 0 ? (uint32_t)(2 * value) : (uint32_t)(-2 * value - 1);
}

static void rlgr_code_gr(
	struct rfx_bit_writer *writer,
	uint32_t *krp,
	uint32_t value
)
{
	const uint32_t kr = *krp >> RF_RFX_LSGR;
	const uint32_t vk = value >> kr;

	bit_writer_put_repeated_bit(writer, vk, 1);
	bit_writer_put_bits(writer, 0, 1);
	if (kr != 0)
		bit_writer_put_bits(writer, value & ((1u << kr) - 1u), kr);

	if (vk == 0)
		update_param(krp, -2);
	else if (vk > 1)
		update_param(krp, (int)vk);
}

static bool rlgr1_encode(
	const int16_t *data,
	size_t data_size,
	uint8_t *buffer,
	size_t buffer_size,
	uint16_t *encoded_size
)
{
	struct rfx_bit_writer writer = {
		buffer, buffer_size, 0, 8, false
	};
	uint32_t kp = 1u << RF_RFX_LSGR;
	uint32_t krp = 1u << RF_RFX_LSGR;
	uint32_t k = 1;
	size_t index = 0;

	while (index < data_size) {
		if (k != 0) {
			uint32_t zero_count = 0;
			int input = data[index++];

			while (input == 0 && index < data_size) {
				zero_count++;
				input = data[index++];
			}

			uint32_t run_max = 1u << k;
			while (zero_count >= run_max) {
				bit_writer_put_bits(&writer, 0, 1);
				zero_count -= run_max;
				k = update_param(&kp, RF_RFX_UP_GR);
				run_max = 1u << k;
			}

			bit_writer_put_bits(&writer, 1, 1);
			bit_writer_put_bits(&writer, zero_count, k);
			bit_writer_put_bits(&writer, input < 0 ? 1u : 0u, 1);
			const uint32_t magnitude =
				input < 0 ? (uint32_t)-input : (uint32_t)input;
			rlgr_code_gr(&writer, &krp, magnitude != 0 ? magnitude - 1 : 0);
			k = update_param(&kp, -RF_RFX_DN_GR);
		} else {
			const uint32_t two_ms = mag_sign(data[index++]);

			rlgr_code_gr(&writer, &krp, two_ms);
			k = update_param(
				&kp,
				two_ms != 0 ? -RF_RFX_DQ_GR : RF_RFX_UQ_GR
			);
		}
	}

	bit_writer_flush(&writer);
	const size_t length = bit_writer_length(&writer);
	if (length == 0 || length > UINT16_MAX)
		return false;
	*encoded_size = (uint16_t)length;
	return true;
}

static void dwt_2d_encode_block(int16_t *buffer, int16_t *dwt, uint32_t subband_width)
{
	const uint32_t total_width = subband_width << 1;

	for (uint32_t x = 0; x < total_width; ++x) {
		for (uint32_t n = 0; n < subband_width; ++n) {
			const uint32_t y = n << 1;
			int16_t *low = dwt + (size_t)n * total_width + x;
			int16_t *high = low + (size_t)subband_width * total_width;
			int16_t *src = buffer + (size_t)y * total_width + x;
			const int16_t next =
				src[n < subband_width - 1 ? 2 * total_width : 0];

			*high = cast_i16((src[total_width] - ((src[0] + next) >> 1)) >> 1);
			*low = cast_i16(src[0] + (n == 0 ? *high : (*(high - total_width) + *high) >> 1));
		}
	}

	int16_t *ll = buffer + 3u * subband_width * subband_width;
	int16_t *hl = buffer;
	int16_t *l_src = dwt;
	int16_t *lh = buffer + (size_t)subband_width * subband_width;
	int16_t *hh = buffer + 2u * subband_width * subband_width;
	int16_t *h_src = dwt + 2u * subband_width * subband_width;

	for (uint32_t y = 0; y < subband_width; ++y) {
		for (uint32_t n = 0; n < subband_width; ++n) {
			const uint32_t x = n << 1;
			const uint32_t next = n < subband_width - 1 ? x + 2 : x;

			hl[n] = cast_i16((l_src[x + 1] - ((l_src[x] + l_src[next]) >> 1)) >> 1);
			ll[n] = cast_i16(l_src[x] + (n == 0 ? hl[n] : (hl[n - 1] + hl[n]) >> 1));
			hh[n] = cast_i16((h_src[x + 1] - ((h_src[x] + h_src[next]) >> 1)) >> 1);
			lh[n] = cast_i16(h_src[x] + (n == 0 ? hh[n] : (hh[n - 1] + hh[n]) >> 1));
		}

		ll += subband_width;
		hl += subband_width;
		l_src += total_width;
		lh += subband_width;
		hh += subband_width;
		h_src += total_width;
	}
}

static void dwt_2d_encode(int16_t *buffer, int16_t *dwt)
{
	dwt_2d_encode_block(buffer, dwt, 32);
	dwt_2d_encode_block(buffer + 3072, dwt, 16);
	dwt_2d_encode_block(buffer + 3840, dwt, 8);
}

static void quantize_block(int16_t *buffer, size_t count, uint32_t factor)
{
	if (factor == 0)
		return;

	const int16_t half = (int16_t)(1u << (factor - 1));
	for (size_t i = 0; i < count; ++i)
		buffer[i] = cast_i16((buffer[i] + half) >> factor);
}

static void quantize(int16_t *buffer, const uint32_t quants[RF_RFX_QUANT_COUNT])
{
	quantize_block(buffer, 1024, quants[8] - 6);
	quantize_block(buffer + 1024, 1024, quants[7] - 6);
	quantize_block(buffer + 2048, 1024, quants[9] - 6);
	quantize_block(buffer + 3072, 256, quants[5] - 6);
	quantize_block(buffer + 3328, 256, quants[4] - 6);
	quantize_block(buffer + 3584, 256, quants[6] - 6);
	quantize_block(buffer + 3840, 64, quants[2] - 6);
	quantize_block(buffer + 3904, 64, quants[1] - 6);
	quantize_block(buffer + 3968, 64, quants[3] - 6);
	quantize_block(buffer + 4032, 64, quants[0] - 6);
	quantize_block(buffer, RF_RFX_COEFF_COUNT, 5);
}

static void differential_encode(int16_t *buffer, size_t count)
{
	int16_t previous = buffer[0];

	for (size_t i = 0; i < count - 1; ++i) {
		int16_t *dst = buffer + i + 1;
		const int16_t next = *dst;

		*dst = cast_i16(next - previous);
		previous = next;
	}
}

static int16_t clamp_ycbcr(int32_t value)
{
	if (value < -4096)
		return -4096;
	if (value > 4095)
		return 4095;
	return (int16_t)value;
}

static void convert_rgb_to_ycbcr(int16_t *r, int16_t *g, int16_t *b)
{
	for (size_t i = 0; i < RF_RFX_COEFF_COUNT; ++i) {
		const int32_t rv = r[i];
		const int32_t gv = g[i];
		const int32_t bv = b[i];
		const int32_t y = (rv * 9798 + gv * 19235 + bv * 3735) >> 10;
		const int32_t cb = (rv * -5535 + gv * -10868 + bv * 16403) >> 10;
		const int32_t cr = (rv * 16377 + gv * -13714 + bv * -2663) >> 10;

		r[i] = clamp_ycbcr(y - 4096);
		g[i] = clamp_ycbcr(cb);
		b[i] = clamp_ycbcr(cr);
	}
}

static bool encode_component(
	int16_t *component,
	uint8_t *out,
	uint16_t *out_length,
	const uint32_t quants[RF_RFX_QUANT_COUNT]
)
{
	int16_t dwt[RF_RFX_COEFF_COUNT] = { 0 };

	dwt_2d_encode(component, dwt);
	quantize(component, quants);
	differential_encode(component + 4032, 64);
	memset(out, 0, RF_RFX_COMPONENT_CAPACITY);
	return rlgr1_encode(
		component,
		RF_RFX_COEFF_COUNT,
		out,
		RF_RFX_COMPONENT_CAPACITY,
		out_length
	);
}

static bool validate_input(
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	if (rgba == NULL || frame_width == 0 || frame_height == 0 ||
	    width == 0 || height == 0)
		return false;
	if ((uint32_t)x + width > frame_width ||
	    (uint32_t)y + height > frame_height)
		return false;

	const size_t row_bytes = (size_t)frame_width * 4;
	if (rgba_stride < row_bytes)
		return false;
	if ((size_t)frame_height > SIZE_MAX / rgba_stride)
		return false;
	if (((size_t)frame_height - 1) * rgba_stride + row_bytes > rgba_length)
		return false;
	return true;
}

static void extract_tile(
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t tile_x,
	uint16_t tile_y,
	int16_t *r,
	int16_t *g,
	int16_t *b
)
{
	const uint16_t max_x = frame_width - 1;
	const uint16_t max_y = frame_height - 1;

	for (uint16_t y = 0; y < RF_RFX_TILE_SIZE; ++y) {
		uint16_t src_y = tile_y + y;
		if (src_y > max_y)
			src_y = max_y;

		for (uint16_t x = 0; x < RF_RFX_TILE_SIZE; ++x) {
			uint16_t src_x = tile_x + x;
			if (src_x > max_x)
				src_x = max_x;

			const uint8_t *pixel =
				rgba + (size_t)src_y * rgba_stride + (size_t)src_x * 4;
			const size_t index = (size_t)y * RF_RFX_TILE_SIZE + x;
			r[index] = pixel[0];
			g[index] = pixel[1];
			b[index] = pixel[2];
		}
	}
}

static GByteArray *encode_tile(
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t source_x,
	uint16_t source_y,
	uint16_t tile_x_idx,
	uint16_t tile_y_idx,
	const uint32_t quants[RF_RFX_QUANT_COUNT]
)
{
	int16_t y[RF_RFX_COEFF_COUNT] = { 0 };
	int16_t cb[RF_RFX_COEFF_COUNT] = { 0 };
	int16_t cr[RF_RFX_COEFF_COUNT] = { 0 };
	uint8_t y_data[RF_RFX_COMPONENT_CAPACITY] = { 0 };
	uint8_t cb_data[RF_RFX_COMPONENT_CAPACITY] = { 0 };
	uint8_t cr_data[RF_RFX_COMPONENT_CAPACITY] = { 0 };
	uint16_t y_length = 0;
	uint16_t cb_length = 0;
	uint16_t cr_length = 0;

	extract_tile(
		rgba,
		rgba_stride,
		frame_width,
		frame_height,
		source_x,
		source_y,
		y,
		cb,
		cr
	);
	convert_rgb_to_ycbcr(y, cb, cr);
	if (!encode_component(y, y_data, &y_length, quants) ||
	    !encode_component(cb, cb_data, &cb_length, quants) ||
	    !encode_component(cr, cr_data, &cr_length, quants))
		return NULL;

	const size_t block_len =
		19u + (size_t)y_length + cb_length + cr_length;
	if (block_len > UINT32_MAX)
		return NULL;
	GByteArray *array = g_byte_array_sized_new(block_len);
	append_u16_le(array, RF_RFX_CBT_TILE);
	append_u32_le(array, (uint32_t)block_len);
	append_u8(array, 0);
	append_u8(array, 0);
	append_u8(array, 0);
	append_u16_le(array, tile_x_idx);
	append_u16_le(array, tile_y_idx);
	append_u16_le(array, y_length);
	append_u16_le(array, cb_length);
	append_u16_le(array, cr_length);
	g_byte_array_append(array, y_data, y_length);
	g_byte_array_append(array, cb_data, cb_length);
	g_byte_array_append(array, cr_data, cr_length);
	return array;
}

static void encode_tile_job_sync(struct rfx_tile_job *job)
{
	job->encoded = encode_tile(
		job->rgba,
		job->rgba_stride,
		job->frame_width,
		job->frame_height,
		job->source_x,
		job->source_y,
		job->tile_x_idx,
		job->tile_y_idx,
		job->quants
	);
}

static void rfx_tile_job_complete(struct rfx_tile_job_group *group)
{
	g_mutex_lock(&group->mutex);
	group->pending--;
	if (group->pending == 0)
		g_cond_signal(&group->cond);
	g_mutex_unlock(&group->mutex);
}

static void encode_tile_job(gpointer data, gpointer user_data)
{
	(void)user_data;

	struct rfx_tile_job *job = data;
	encode_tile_job_sync(job);
	rfx_tile_job_complete(job->group);
}

static void append_sync(GByteArray *array)
{
	append_u16_le(array, RF_RFX_WBT_SYNC);
	append_u32_le(array, 12);
	append_u32_le(array, RF_RFX_WF_MAGIC);
	append_u16_le(array, RF_RFX_WF_VERSION_1_0);
}

static uint16_t context_properties(void)
{
	return (RF_RFX_COL_CONV_ICT << 3) |
	       (RF_RFX_XFORM_DWT_53_A << 5) |
	       (RF_RFX_ENTROPY_RLGR1 << 9) |
	       (RF_RFX_SCALAR_QUANTIZATION << 13);
}

static uint16_t tileset_properties(void)
{
	return 1u |
	       (RF_RFX_COL_CONV_ICT << 4) |
	       (RF_RFX_XFORM_DWT_53_A << 6) |
	       (RF_RFX_ENTROPY_RLGR1 << 10) |
	       (RF_RFX_SCALAR_QUANTIZATION << 14);
}

static void append_context(GByteArray *array)
{
	append_u16_le(array, RF_RFX_WBT_CONTEXT);
	append_u32_le(array, 13);
	append_u8(array, 1);
	append_u8(array, 0xff);
	append_u8(array, 0);
	append_u16_le(array, RF_RFX_CT_TILE_64X64);
	append_u16_le(array, context_properties());
}

static void append_codec_versions(GByteArray *array)
{
	append_u16_le(array, RF_RFX_WBT_CODEC_VERSIONS);
	append_u32_le(array, 10);
	append_u8(array, 1);
	append_u8(array, 1);
	append_u16_le(array, RF_RFX_WF_VERSION_1_0);
}

static void append_channels(GByteArray *array, uint16_t width, uint16_t height)
{
	append_u16_le(array, RF_RFX_WBT_CHANNELS);
	append_u32_le(array, 12);
	append_u8(array, 1);
	append_u8(array, 0);
	append_u16_le(array, width);
	append_u16_le(array, height);
}

static void append_frame_begin(GByteArray *array, uint32_t frame_idx)
{
	append_u16_le(array, RF_RFX_WBT_FRAME_BEGIN);
	append_u32_le(array, 14);
	append_u8(array, 1);
	append_u8(array, 0);
	append_u32_le(array, frame_idx);
	append_u16_le(array, 1);
}

static void append_region(
	GByteArray *array,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	append_u16_le(array, RF_RFX_WBT_REGION);
	append_u32_le(array, 23);
	append_u8(array, 1);
	append_u8(array, 0);
	append_u8(array, 1);
	append_u16_le(array, 1);
	append_u16_le(array, x);
	append_u16_le(array, y);
	append_u16_le(array, width);
	append_u16_le(array, height);
	append_u16_le(array, RF_RFX_CBT_REGION);
	append_u16_le(array, 1);
}

static void append_frame_end(GByteArray *array)
{
	append_u16_le(array, RF_RFX_WBT_FRAME_END);
	append_u32_le(array, 8);
	append_u8(array, 1);
	append_u8(array, 0);
}

static void free_tile_jobs(struct rfx_tile_job *jobs, uint16_t tile_count)
{
	for (uint16_t i = 0; i < tile_count; ++i)
		g_clear_pointer(&jobs[i].encoded, g_byte_array_unref);
	g_free(jobs);
}

static bool append_encoded_tiles(
	GByteArray *array,
	struct rfx_tile_job *jobs,
	uint16_t tile_count
)
{
	for (uint16_t i = 0; i < tile_count; ++i) {
		if (jobs[i].encoded == NULL)
			return false;
		g_byte_array_append(
			array,
			jobs[i].encoded->data,
			jobs[i].encoded->len
		);
	}
	return true;
}

static void prepare_tile_jobs(
	struct rfx_tile_job *jobs,
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t end_x,
	uint16_t end_y,
	const uint32_t quants[RF_RFX_QUANT_COUNT]
)
{
	uint16_t index = 0;

	for (uint16_t tile_y = 0; tile_y <= end_y; ++tile_y) {
		for (uint16_t tile_x = 0; tile_x <= end_x; ++tile_x) {
			struct rfx_tile_job *job = jobs + index++;

			job->rgba = rgba;
			job->rgba_stride = rgba_stride;
			job->frame_width = frame_width;
			job->frame_height = frame_height;
			job->source_x = (uint16_t)(
				(uint32_t)x + tile_x * RF_RFX_TILE_SIZE
			);
			job->source_y = (uint16_t)(
				(uint32_t)y + tile_y * RF_RFX_TILE_SIZE
			);
			job->tile_x_idx = tile_x;
			job->tile_y_idx = tile_y;
			job->quants = quants;
		}
	}
}

static bool encode_tiles_serial(
	GByteArray *array,
	struct rfx_tile_job *jobs,
	uint16_t tile_count
)
{
	for (uint16_t i = 0; i < tile_count; ++i)
		encode_tile_job_sync(jobs + i);
	return append_encoded_tiles(array, jobs, tile_count);
}

static bool encode_tiles_parallel(
	RfRdpRfxContext *context,
	GByteArray *array,
	struct rfx_tile_job *jobs,
	uint16_t tile_count
)
{
	struct rfx_tile_job_group group = { 0 };

	g_mutex_init(&group.mutex);
	g_cond_init(&group.cond);
	group.pending = tile_count;

	for (uint16_t i = 0; i < tile_count; ++i) {
		g_autoptr(GError) error = NULL;

		jobs[i].group = &group;
		if (!g_thread_pool_push(context->thread_pool, jobs + i, &error)) {
			encode_tile_job_sync(jobs + i);
			rfx_tile_job_complete(&group);
		}
	}

	g_mutex_lock(&group.mutex);
	while (group.pending > 0)
		g_cond_wait(&group.cond, &group.mutex);
	g_mutex_unlock(&group.mutex);

	g_cond_clear(&group.cond);
	g_mutex_clear(&group.mutex);
	return append_encoded_tiles(array, jobs, tile_count);
}

static bool append_tiles(
	RfRdpRfxContext *context,
	GByteArray *array,
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t end_x,
	uint16_t end_y,
	uint16_t tile_count,
	const uint32_t quants[RF_RFX_QUANT_COUNT]
)
{
	struct rfx_tile_job *jobs = g_new0(struct rfx_tile_job, tile_count);
	bool ok = false;

	prepare_tile_jobs(
		jobs,
		rgba,
		rgba_stride,
		frame_width,
		frame_height,
		x,
		y,
		end_x,
		end_y,
		quants
	);

	if (context->thread_pool != NULL &&
	    tile_count >= RF_RFX_PARALLEL_MIN_TILES)
		ok = encode_tiles_parallel(context, array, jobs, tile_count);
	else
		ok = encode_tiles_serial(array, jobs, tile_count);

	free_tile_jobs(jobs, tile_count);
	return ok;
}

static bool append_tileset(
	RfRdpRfxContext *context,
	GByteArray *array,
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	const uint16_t end_x = (width - 1) / RF_RFX_TILE_SIZE;
	const uint16_t end_y = (height - 1) / RF_RFX_TILE_SIZE;
	const uint32_t tile_count_32 =
		((uint32_t)end_x + 1) * ((uint32_t)end_y + 1);
	uint32_t quants[RF_RFX_QUANT_COUNT] = { 0 };
	if (tile_count_32 == 0 || tile_count_32 > UINT16_MAX)
		return false;
	const uint16_t tile_count = (uint16_t)tile_count_32;
	const size_t block = begin_block(array, RF_RFX_WBT_EXTENSION);

	memcpy(quants, context->quants, sizeof(quants));

	append_u8(array, 1);
	append_u8(array, 0);
	append_u16_le(array, RF_RFX_CBT_TILESET);
	append_u16_le(array, 0);
	append_u16_le(array, tileset_properties());
	append_u8(array, 1);
	append_u8(array, RF_RFX_TILE_SIZE);
	append_u16_le(array, tile_count);
	const size_t tiles_size_offset = array->len;
	append_u32_le(array, 0);
	for (size_t i = 0; i < G_N_ELEMENTS(quants); i += 2)
		append_u8(array, (uint8_t)(quants[i] | (quants[i + 1] << 4)));

	const size_t tiles_start = array->len;
	if (!append_tiles(
		    context,
		    array,
		    rgba,
		    rgba_stride,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    end_x,
		    end_y,
		    tile_count,
		    quants
	    ))
		return false;

	const size_t tiles_size = array->len - tiles_start;
	if (tiles_size > UINT32_MAX)
		return false;
	patch_u32_le(array, tiles_size_offset, (uint32_t)tiles_size);
	return end_block(array, block);
}

static unsigned int clamp_thread_count(unsigned int thread_count)
{
	if (thread_count == 0)
		return 1;
	if (thread_count > RF_RFX_MAX_THREADS)
		return RF_RFX_MAX_THREADS;
	return thread_count;
}

static unsigned int default_thread_count(void)
{
	const char *env = g_getenv("RF_RDP_RFX_THREADS");
	if (env != NULL && env[0] != '\0') {
		char *end = NULL;
		const guint64 value = g_ascii_strtoull(env, &end, 10);

		if (end != env)
			return clamp_thread_count((unsigned int)value);
	}

	return clamp_thread_count((unsigned int)g_get_num_processors());
}

static void set_default_quants(RfRdpRfxContext *context)
{
	memcpy(context->quants, RFX_DEFAULT_QUANTS, sizeof(context->quants));
}

void rf_rdp_rfx_context_set_quality_level(
	RfRdpRfxContext *context,
	unsigned int quality_level,
	unsigned int max_quality_level
)
{
	if (context == NULL)
		return;

	if (max_quality_level == 0 || quality_level == 0) {
		context->quality_level = 0;
		set_default_quants(context);
		return;
	}

	if (quality_level > max_quality_level)
		quality_level = max_quality_level;
	context->quality_level = quality_level;

	for (size_t i = 0; i < G_N_ELEMENTS(context->quants); ++i) {
		uint32_t delta = RFX_QUALITY_MAX_DELTA[i] * quality_level;

		delta = (delta + max_quality_level / 2) / max_quality_level;
		context->quants[i] = RFX_DEFAULT_QUANTS[i] + delta;
		if (context->quants[i] > 15)
			context->quants[i] = 15;
	}
}

unsigned int rf_rdp_rfx_context_get_quality_level(
	const RfRdpRfxContext *context
)
{
	return context != NULL ? context->quality_level : 0;
}

void rf_rdp_rfx_context_set_thread_count(
	RfRdpRfxContext *context,
	unsigned int thread_count
)
{
	g_autoptr(GError) error = NULL;
	GThreadPool *thread_pool = NULL;

	if (context == NULL)
		return;

	thread_count = clamp_thread_count(thread_count);
	if (thread_count > 1) {
		thread_pool = g_thread_pool_new(
			encode_tile_job,
			NULL,
			(int)thread_count,
			false,
			&error
		);
		if (thread_pool == NULL)
			thread_count = 1;
	}

	if (context->thread_pool != NULL)
		g_thread_pool_free(context->thread_pool, false, true);
	context->thread_pool = thread_pool;
	context->thread_count = thread_count;
}

unsigned int rf_rdp_rfx_context_get_thread_count(const RfRdpRfxContext *context)
{
	return context != NULL ? context->thread_count : 0;
}

RfRdpRfxContext *rf_rdp_rfx_context_new(void)
{
	RfRdpRfxContext *context = g_new0(RfRdpRfxContext, 1);

	context->send_headers = true;
	set_default_quants(context);
	rf_rdp_rfx_context_set_thread_count(context, default_thread_count());
	return context;
}

void rf_rdp_rfx_context_free(RfRdpRfxContext *context)
{
	if (context == NULL)
		return;
	if (context->thread_pool != NULL)
		g_thread_pool_free(context->thread_pool, false, true);
	g_free(context);
}

void rf_rdp_rfx_context_reset(RfRdpRfxContext *context)
{
	if (context == NULL)
		return;
	context->send_headers = true;
	context->width = 0;
	context->height = 0;
	context->frame_idx = 0;
}

GByteArray *rf_rdp_rfx_encode_rgba(
	RfRdpRfxContext *context,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t frame_width,
	uint16_t frame_height,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	if (context == NULL ||
	    !validate_input(
		    rgba,
		    rgba_length,
		    rgba_stride,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height
	    ))
		return NULL;

	const bool size_changed =
		context->width != frame_width || context->height != frame_height;
	const bool send_headers = context->send_headers || size_changed;
	const uint32_t frame_idx = size_changed ? 0 : context->frame_idx;

	GByteArray *array = g_byte_array_sized_new(
		(size_t)width * height * 2 + 256
	);
	if (send_headers) {
		append_sync(array);
		append_context(array);
		append_codec_versions(array);
		append_channels(array, frame_width, frame_height);
	}

	append_frame_begin(array, frame_idx);
	append_region(array, 0, 0, width, height);
	if (!append_tileset(
		    context,
		    array,
		    rgba,
		    rgba_stride,
		    frame_width,
		    frame_height,
		    x,
		    y,
		    width,
		    height
	    )) {
		g_byte_array_unref(array);
		return NULL;
	}
	append_frame_end(array);
	context->send_headers = false;
	context->width = frame_width;
	context->height = frame_height;
	context->frame_idx = frame_idx + 1;
	return array;
}
