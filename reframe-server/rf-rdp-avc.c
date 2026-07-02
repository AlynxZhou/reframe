#include "rf-rdp-avc.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RF_HAVE_RDP_AVC
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#endif

static const char *const avc_auto_candidates[] = {
	"h264_nvenc",
	"h264_vaapi",
	"h264_qsv",
	"h264_amf",
	"h264_v4l2m2m",
	"libx264",
	"h264",
	NULL
};

struct rf_rdp_avc_encoder {
#ifdef RF_HAVE_RDP_AVC
	AVCodecContext *codec;
	AVFrame *frame;
	AVFrame *hw_frame;
	AVFrame *avc444_yuv444_frame;
	AVPacket *packet;
	AVBufferRef *hw_device_ctx;
	AVBufferRef *hw_frames_ctx;
	struct SwsContext *sws;
	struct SwsContext *avc444_sws;
	enum AVPixelFormat sw_pix_fmt;
	bool hardware_frames;
#endif
	char *name;
	uint16_t width;
	uint16_t height;
	unsigned int fps;
	int64_t bit_rate;
	uint8_t qp;
	unsigned int gop_size;
	int64_t pts;
	uint64_t avc444_prepare_count;
};

static int64_t default_bit_rate(uint16_t width, uint16_t height, unsigned int fps)
{
	int64_t bits = (int64_t)width * height * fps / 10;
	if (bits < 2000000)
		return 2000000;
	if (bits > 16000000)
		return 16000000;
	return bits;
}

#ifdef RF_HAVE_RDP_AVC
static void trace_av_error(const char *step, int rc)
{
	if (getenv("RF_RDP_AVC_TRACE") == NULL)
		return;

	char message[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	av_strerror(rc, message, sizeof(message));
	fprintf(stderr, "RDP AVC: %s failed: %s (%d)\n", step, message, rc);
}

static void set_encoder_options(RfRdpAvcEncoder *encoder, const char *name)
{
	AVCodecContext *codec = encoder != NULL ? encoder->codec : NULL;
	if (codec == NULL || codec->priv_data == NULL || name == NULL)
		return;

	char qp[8] = { 0 };
	snprintf(qp, sizeof(qp), "%u", encoder->qp);

	if (strcmp(name, "h264_nvenc") == 0) {
		av_opt_set(codec->priv_data, "preset", "p4", 0);
		av_opt_set(codec->priv_data, "tune", "ull", 0);
		av_opt_set(codec->priv_data, "rc", "cbr", 0);
		av_opt_set(codec->priv_data, "delay", "0", 0);
		av_opt_set(codec->priv_data, "zerolatency", "1", 0);
		av_opt_set(codec->priv_data, "forced-idr", "1", 0);
		av_opt_set(codec->priv_data, "repeat-headers", "1", 0);
		av_opt_set(codec->priv_data, "qp", qp, 0);
	} else if (strcmp(name, "h264_vaapi") == 0) {
		av_opt_set(codec->priv_data, "async_depth", "1", 0);
		av_opt_set(codec->priv_data, "rc_mode", "CBR", 0);
		av_opt_set(codec->priv_data, "profile", "high", 0);
		av_opt_set(codec->priv_data, "quality", "4", 0);
		av_opt_set(codec->priv_data, "aud", "1", 0);
		av_opt_set(codec->priv_data, "qp", qp, 0);
	} else if (strcmp(name, "libx264") == 0) {
		av_opt_set(codec->priv_data, "preset", "ultrafast", 0);
		av_opt_set(codec->priv_data, "tune", "zerolatency", 0);
		av_opt_set(codec->priv_data, "repeat-headers", "1", 0);
		av_opt_set(codec->priv_data, "qp", qp, 0);
	}
}

static void close_encoder_state(RfRdpAvcEncoder *encoder)
{
	if (encoder == NULL)
		return;

	sws_freeContext(encoder->sws);
	sws_freeContext(encoder->avc444_sws);
	encoder->sws = NULL;
	encoder->avc444_sws = NULL;
	av_packet_free(&encoder->packet);
	av_frame_free(&encoder->avc444_yuv444_frame);
	av_frame_free(&encoder->hw_frame);
	av_frame_free(&encoder->frame);
	avcodec_free_context(&encoder->codec);
	av_buffer_unref(&encoder->hw_frames_ctx);
	av_buffer_unref(&encoder->hw_device_ctx);
	free(encoder->name);
	encoder->name = NULL;
	encoder->sw_pix_fmt = AV_PIX_FMT_NONE;
	encoder->hardware_frames = false;
}

static bool open_vaapi_device(AVBufferRef **device_ctx)
{
	const char *env_device = getenv("RF_RDP_VAAPI_DEVICE");
	static const char *const devices[] = {
		"/dev/dri/renderD128",
		"/dev/dri/renderD129",
		NULL
	};

	if (env_device != NULL && env_device[0] != '\0' &&
	    av_hwdevice_ctx_create(
		    device_ctx,
		    AV_HWDEVICE_TYPE_VAAPI,
		    env_device,
		    NULL,
		    0
	    ) >= 0)
		return true;

	for (const char *const *device = devices; *device != NULL; ++device) {
		if (av_hwdevice_ctx_create(
			    device_ctx,
			    AV_HWDEVICE_TYPE_VAAPI,
			    *device,
			    NULL,
			    0
		    ) >= 0)
			return true;
	}

	return av_hwdevice_ctx_create(
		       device_ctx,
		       AV_HWDEVICE_TYPE_VAAPI,
		       NULL,
		       NULL,
		       0
	       ) >= 0;
}

static bool setup_vaapi_frames(RfRdpAvcEncoder *encoder)
{
	if (!open_vaapi_device(&encoder->hw_device_ctx))
		return false;

	encoder->hw_frames_ctx = av_hwframe_ctx_alloc(encoder->hw_device_ctx);
	if (encoder->hw_frames_ctx == NULL)
		return false;

	AVHWFramesContext *frames =
		(AVHWFramesContext *)encoder->hw_frames_ctx->data;
	frames->format = AV_PIX_FMT_VAAPI;
	frames->sw_format = AV_PIX_FMT_NV12;
	frames->width = encoder->width;
	frames->height = encoder->height;
	frames->initial_pool_size = 4;
	if (av_hwframe_ctx_init(encoder->hw_frames_ctx) < 0)
		return false;

	encoder->codec->hw_frames_ctx = av_buffer_ref(encoder->hw_frames_ctx);
	return encoder->codec->hw_frames_ctx != NULL;
}

static bool configure_encoder_context(RfRdpAvcEncoder *encoder, const char *name)
{
	encoder->codec->width = encoder->width;
	encoder->codec->height = encoder->height;
	encoder->codec->time_base = (AVRational){ 1, (int)encoder->fps };
	encoder->codec->framerate = (AVRational){ (int)encoder->fps, 1 };
	encoder->codec->gop_size = (int)encoder->gop_size;
	encoder->codec->max_b_frames = 0;
	encoder->codec->bit_rate = encoder->bit_rate;
	encoder->codec->rc_min_rate = encoder->bit_rate;
	encoder->codec->rc_max_rate = encoder->bit_rate;
	encoder->codec->rc_buffer_size =
		(int)(encoder->bit_rate > INT_MAX ? INT_MAX : encoder->bit_rate);
	encoder->codec->qmin = encoder->qp;
	encoder->codec->qmax =
		(int)(encoder->qp > 55 ? 63 : (unsigned int)encoder->qp + 8u);
	encoder->codec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	encoder->sw_pix_fmt = AV_PIX_FMT_NV12;

	if (strcmp(name, "h264_vaapi") == 0) {
		encoder->hardware_frames = true;
		encoder->codec->pix_fmt = AV_PIX_FMT_VAAPI;
		if (!setup_vaapi_frames(encoder))
			return false;
	} else {
		encoder->hardware_frames = false;
		encoder->codec->pix_fmt = encoder->sw_pix_fmt;
	}

	set_encoder_options(encoder, name);
	return true;
}

static bool open_encoder(RfRdpAvcEncoder *encoder, const char *name)
{
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	close_encoder_state(encoder);
	if (codec == NULL)
		return false;

	encoder->codec = avcodec_alloc_context3(codec);
	if (encoder->codec == NULL)
		return false;

	if (!configure_encoder_context(encoder, name))
		goto fail;

	int rc = avcodec_open2(encoder->codec, codec, NULL);
	if (rc < 0) {
		trace_av_error("avcodec_open2", rc);
		goto fail;
	}

	encoder->frame = av_frame_alloc();
	encoder->packet = av_packet_alloc();
	if (encoder->frame == NULL || encoder->packet == NULL)
		goto fail;
	if (encoder->hardware_frames) {
		encoder->hw_frame = av_frame_alloc();
		if (encoder->hw_frame == NULL)
			goto fail;
	}

	encoder->frame->format = encoder->sw_pix_fmt;
	encoder->frame->width = encoder->width;
	encoder->frame->height = encoder->height;
	rc = av_frame_get_buffer(encoder->frame, 32);
	if (rc < 0) {
		trace_av_error("av_frame_get_buffer", rc);
		goto fail;
	}

	encoder->sws = sws_getContext(
		encoder->width,
		encoder->height,
		AV_PIX_FMT_RGBA,
		encoder->width,
		encoder->height,
		encoder->sw_pix_fmt,
		SWS_FAST_BILINEAR,
		NULL,
		NULL,
		NULL
	);
	if (encoder->sws == NULL)
		goto fail;

	encoder->avc444_yuv444_frame = av_frame_alloc();
	if (encoder->avc444_yuv444_frame == NULL)
		goto fail;
	encoder->avc444_yuv444_frame->format = AV_PIX_FMT_YUV444P;
	encoder->avc444_yuv444_frame->width = encoder->width;
	encoder->avc444_yuv444_frame->height = encoder->height;
	rc = av_frame_get_buffer(encoder->avc444_yuv444_frame, 32);
	if (rc < 0) {
		trace_av_error("av_frame_get_buffer(avc444)", rc);
		goto fail;
	}

	encoder->avc444_sws = sws_getContext(
		encoder->width,
		encoder->height,
		AV_PIX_FMT_RGBA,
		encoder->width,
		encoder->height,
		AV_PIX_FMT_YUV444P,
		SWS_FAST_BILINEAR,
		NULL,
		NULL,
		NULL
	);
	if (encoder->avc444_sws == NULL)
		goto fail;

	encoder->name = strdup(name);
	if (encoder->name == NULL)
		goto fail;
	return true;

fail:
	close_encoder_state(encoder);
	return false;
}

static bool get_rgba_source_geometry(
	const RfRdpAvcEncoder *encoder,
	size_t rgba_length,
	size_t rgba_stride,
	size_t *copy_row_bytes,
	size_t *source_rows
)
{
	const size_t source_width =
		rgba_stride / 4 < encoder->width ? rgba_stride / 4 : encoder->width;

	if (rgba_stride == 0 || rgba_stride % 4 != 0 || source_width == 0)
		return false;

	*copy_row_bytes = source_width * 4;
	if (rgba_length < *copy_row_bytes)
		return false;

	*source_rows = 1 + (rgba_length - *copy_row_bytes) / rgba_stride;
	if (*source_rows > encoder->height)
		*source_rows = encoder->height;
	return *source_rows > 0;
}

static uint8_t *pad_rgba_source(
	const RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_stride,
	size_t copy_row_bytes,
	size_t source_rows
)
{
	const size_t row_bytes = (size_t)encoder->width * 4;
	if (encoder->height > SIZE_MAX / row_bytes)
		return NULL;

	uint8_t *padded = malloc((size_t)encoder->height * row_bytes);
	if (padded == NULL)
		return NULL;

	for (uint16_t y = 0; y < encoder->height; ++y) {
		const size_t src_y = y < source_rows ? y : source_rows - 1;
		const uint8_t *src = rgba + src_y * rgba_stride;
		uint8_t *dst = padded + (size_t)y * row_bytes;

		memcpy(dst, src, copy_row_bytes);
		if (copy_row_bytes < row_bytes) {
			const uint8_t *last_pixel = dst + copy_row_bytes - 4;
			for (size_t x = copy_row_bytes; x < row_bytes; x += 4)
				memcpy(dst + x, last_pixel, 4);
		}
	}

	return padded;
}

static uint8_t rgb_to_y(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint8_t)((54 * r + 183 * g + 18 * b) >> 8);
}

static uint8_t rgb_to_u(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint8_t)(((-29 * r - 99 * g + 128 * b) >> 8) + 128);
}

static uint8_t rgb_to_v(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint8_t)(((128 * r - 116 * g - 12 * b) >> 8) + 128);
}

static void read_rgba_yuv(
	const uint8_t *rgba,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint8_t *out_y,
	uint8_t *out_u,
	uint8_t *out_v
)
{
	const uint8_t *pixel = rgba + (size_t)y * rgba_stride + (size_t)x * 4;
	const uint8_t r = pixel[0];
	const uint8_t g = pixel[1];
	const uint8_t b = pixel[2];

	*out_y = rgb_to_y(r, g, b);
	*out_u = rgb_to_u(r, g, b);
	*out_v = rgb_to_v(r, g, b);
}

static bool prepare_avc444_yuv444_frame(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_stride
)
{
	if (rgba_stride > INT_MAX ||
	    av_frame_make_writable(encoder->avc444_yuv444_frame) < 0)
		return false;

	const uint8_t *src_slices[1] = { rgba };
	const int src_strides[1] = { (int)rgba_stride };
	if (sws_scale(
		    encoder->avc444_sws,
		    src_slices,
		    src_strides,
		    0,
		    encoder->height,
		    encoder->avc444_yuv444_frame->data,
		    encoder->avc444_yuv444_frame->linesize
	    ) != encoder->height)
		return false;
	encoder->avc444_prepare_count++;
	return true;
}

static bool encode_current_frame(
	RfRdpAvcEncoder *encoder,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
)
{
	AVFrame *encode_frame = encoder->frame;

	*h264_data = NULL;
	*h264_data_length = 0;

	encoder->frame->pts = encoder->pts++;
	encoder->frame->pict_type =
		force_keyframe ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;

	if (encoder->hardware_frames) {
		av_frame_unref(encoder->hw_frame);
		int rc = av_hwframe_get_buffer(
			    encoder->hw_frames_ctx,
			    encoder->hw_frame,
			    0
		    );
		if (rc < 0) {
			trace_av_error("av_hwframe_get_buffer", rc);
			return false;
		}
		rc = av_hwframe_transfer_data(
			    encoder->hw_frame,
			    encoder->frame,
			    0
		    );
		if (rc < 0) {
			trace_av_error("av_hwframe_transfer_data", rc);
			return false;
		}
		encoder->hw_frame->pts = encoder->frame->pts;
		encoder->hw_frame->pict_type = encoder->frame->pict_type;
		encode_frame = encoder->hw_frame;
	}

	int rc = avcodec_send_frame(encoder->codec, encode_frame);
	if (rc < 0) {
		trace_av_error("avcodec_send_frame", rc);
		return false;
	}

	rc = avcodec_receive_packet(encoder->codec, encoder->packet);
	if (rc < 0) {
		trace_av_error("avcodec_receive_packet", rc);
		return false;
	}

	*h264_data = malloc(encoder->packet->size);
	if (*h264_data == NULL) {
		av_packet_unref(encoder->packet);
		return false;
	}
	memcpy(*h264_data, encoder->packet->data, encoder->packet->size);
	*h264_data_length = encoder->packet->size;
	av_packet_unref(encoder->packet);
	return *h264_data_length > 0;
}

static bool fill_avc444_luma_nv12(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_stride
)
{
	if (rgba_stride > INT_MAX)
		return false;

	const uint8_t *src_slices[1] = { rgba };
	const int src_strides[1] = { (int)rgba_stride };

	return sws_scale(
		       encoder->sws,
		       src_slices,
		       src_strides,
		       0,
		       encoder->height,
		       encoder->frame->data,
		       encoder->frame->linesize
	       ) == encoder->height;
}

static bool fill_avc444_chroma_nv12_from_yuv444(RfRdpAvcEncoder *encoder)
{
	AVFrame *frame = encoder->frame;
	AVFrame *yuv = encoder->avc444_yuv444_frame;

	if (yuv->linesize[1] < encoder->width ||
	    yuv->linesize[2] < encoder->width)
		return false;

	for (uint16_t y = 0; y < encoder->height; y += 2) {
		const uint8_t *u_even = yuv->data[1] + (size_t)y * yuv->linesize[1];
		const uint8_t *u_odd = yuv->data[1] +
			(size_t)(y + 1) * yuv->linesize[1];
		const uint8_t *v_even = yuv->data[2] + (size_t)y * yuv->linesize[2];
		const uint8_t *v_odd = yuv->data[2] +
			(size_t)(y + 1) * yuv->linesize[2];
		const size_t i = y / 2;
		const size_t n = (i & (size_t)~7) + i;
		uint8_t *b4 = frame->data[0] + n * frame->linesize[0];
		uint8_t *b5 = frame->data[0] + (n + 8) * frame->linesize[0];
		uint8_t *uv = frame->data[1] +
			(size_t)(y / 2) * frame->linesize[1];

		for (uint16_t x = 0; x < encoder->width; x += 2) {
			b4[x] = u_odd[x];
			b4[x + 1] = u_odd[x + 1];
			b5[x] = v_odd[x];
			b5[x + 1] = v_odd[x + 1];
			uv[x] = u_even[x + 1];
			uv[x + 1] = v_even[x + 1];
		}
	}
	return true;
}
#endif

void rf_rdp_avc_encoder_free(RfRdpAvcEncoder *encoder)
{
	if (encoder == NULL)
		return;

#ifdef RF_HAVE_RDP_AVC
	close_encoder_state(encoder);
#endif
	free(encoder);
}

static RfRdpAvcEncoder *avc_encoder_new_with_rate_internal(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder,
	bool hardware_only
)
{
	if (width == 0 || height == 0 || fps == 0 || qp > 63)
		return NULL;

	RfRdpAvcEncoder *encoder = calloc(1, sizeof(*encoder));
	if (encoder == NULL)
		return NULL;

	encoder->width = width;
	encoder->height = height;
	encoder->fps = fps;
	encoder->bit_rate = bit_rate > 0 ?
		bit_rate :
		default_bit_rate(encoder->width, encoder->height, encoder->fps);
	encoder->qp = qp;
	encoder->gop_size = gop_size > 0 ? gop_size : fps;
	if (encoder->gop_size > INT_MAX)
		encoder->gop_size = INT_MAX;

#ifdef RF_HAVE_RDP_AVC
	const bool prefer_auto = preferred_encoder == NULL ||
		preferred_encoder[0] == '\0' ||
		strcmp(preferred_encoder, "auto") == 0;
	if (hardware_only && !prefer_auto &&
	    !rf_rdp_avc_encoder_name_is_hardware(preferred_encoder))
		goto fail;
	if (!prefer_auto &&
	    open_encoder(encoder, preferred_encoder))
		return encoder;

	for (const char *const *candidate = avc_auto_candidates;
	     *candidate != NULL;
	     ++candidate) {
		if (hardware_only &&
		    !rf_rdp_avc_encoder_name_is_hardware(*candidate))
			continue;
		if (!prefer_auto &&
		    strcmp(preferred_encoder, *candidate) == 0)
			continue;
		if (open_encoder(encoder, *candidate))
			return encoder;
	}
#else
	(void)preferred_encoder;
	(void)hardware_only;
#endif

fail:
	rf_rdp_avc_encoder_free(encoder);
	return NULL;
}

RfRdpAvcEncoder *rf_rdp_avc_encoder_new_with_rate(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder
)
{
	return avc_encoder_new_with_rate_internal(
		width,
		height,
		fps,
		bit_rate,
		qp,
		gop_size,
		preferred_encoder,
		false
	);
}

RfRdpAvcEncoder *rf_rdp_avc_hardware_encoder_new_with_rate(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder
)
{
	return avc_encoder_new_with_rate_internal(
		width,
		height,
		fps,
		bit_rate,
		qp,
		gop_size,
		preferred_encoder,
		true
	);
}

RfRdpAvcEncoder *rf_rdp_avc_encoder_new(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	const char *preferred_encoder
)
{
	return rf_rdp_avc_encoder_new_with_rate(
		width,
		height,
		fps,
		0,
		26,
		0,
		preferred_encoder
	);
}

const char *rf_rdp_avc_encoder_name(const RfRdpAvcEncoder *encoder)
{
	return encoder != NULL ? encoder->name : NULL;
}

int64_t rf_rdp_avc_encoder_bit_rate(const RfRdpAvcEncoder *encoder)
{
	return encoder != NULL ? encoder->bit_rate : 0;
}

uint8_t rf_rdp_avc_encoder_qp(const RfRdpAvcEncoder *encoder)
{
	return encoder != NULL ? encoder->qp : 0;
}

unsigned int rf_rdp_avc_encoder_gop_size(const RfRdpAvcEncoder *encoder)
{
	return encoder != NULL ? encoder->gop_size : 0;
}

uint64_t rf_rdp_avc_encoder_avc444_prepare_count(const RfRdpAvcEncoder *encoder)
{
	return encoder != NULL ? encoder->avc444_prepare_count : 0;
}

#ifdef RF_HAVE_RDP_AVC
static uint64_t fnv1a_byte(uint64_t hash, uint8_t value)
{
	return (hash ^ value) * 1099511628211ull;
}
#endif

bool rf_rdp_avc_compute_avc444_signatures(
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t width,
	uint16_t height,
	uint64_t *luma_signature,
	uint64_t *chroma_signature
)
{
	if (luma_signature != NULL)
		*luma_signature = 0;
	if (chroma_signature != NULL)
		*chroma_signature = 0;
	if (rgba == NULL || luma_signature == NULL || chroma_signature == NULL ||
	    width == 0 || height == 0)
		return false;
	if ((size_t)width > SIZE_MAX / 4 || rgba_stride < (size_t)width * 4)
		return false;
	if ((size_t)height > SIZE_MAX / rgba_stride ||
	    rgba_length < ((size_t)height - 1) * rgba_stride + (size_t)width * 4)
		return false;

#ifndef RF_HAVE_RDP_AVC
	return false;
#else
	uint64_t luma = 1469598103934665603ull;
	uint64_t chroma = 1469598103934665603ull;

	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			uint8_t yy = 0;
			uint8_t u = 0;
			uint8_t v = 0;

			read_rgba_yuv(rgba, rgba_stride, x, y, &yy, &u, &v);
			luma = fnv1a_byte(luma, yy);
			chroma = fnv1a_byte(chroma, u);
			chroma = fnv1a_byte(chroma, v);
		}
	}

	*luma_signature = luma;
	*chroma_signature = chroma;
	return true;
#endif
}

bool rf_rdp_avc_compare_avc444_rect(
	const uint8_t *current_rgba,
	size_t current_rgba_length,
	const uint8_t *previous_rgba,
	size_t previous_rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	bool *luma_changed,
	bool *chroma_changed
)
{
	if (luma_changed != NULL)
		*luma_changed = false;
	if (chroma_changed != NULL)
		*chroma_changed = false;
	if (current_rgba == NULL || previous_rgba == NULL ||
	    luma_changed == NULL || chroma_changed == NULL ||
	    width == 0 || height == 0)
		return false;
	if ((uint32_t)x + width > UINT16_MAX ||
	    (uint32_t)y + height > UINT16_MAX)
		return false;
	if ((size_t)(x + width) > SIZE_MAX / 4)
		return false;

	const size_t last_row = (size_t)y + height - 1;
	const size_t row_bytes = (size_t)(x + width) * 4;
	if (rgba_stride < row_bytes || last_row > SIZE_MAX / rgba_stride)
		return false;
	const size_t needed = last_row * rgba_stride + row_bytes;
	if (needed > current_rgba_length || needed > previous_rgba_length)
		return false;

#ifndef RF_HAVE_RDP_AVC
	return false;
#else
	for (uint16_t yy = 0; yy < height; ++yy) {
		for (uint16_t xx = 0; xx < width; ++xx) {
			uint8_t current_y = 0;
			uint8_t current_u = 0;
			uint8_t current_v = 0;
			uint8_t previous_y = 0;
			uint8_t previous_u = 0;
			uint8_t previous_v = 0;
			const uint16_t px = x + xx;
			const uint16_t py = y + yy;

			read_rgba_yuv(
				current_rgba,
				rgba_stride,
				px,
				py,
				&current_y,
				&current_u,
				&current_v
			);
			read_rgba_yuv(
				previous_rgba,
				rgba_stride,
				px,
				py,
				&previous_y,
				&previous_u,
				&previous_v
			);
			if (current_y != previous_y)
				*luma_changed = true;
			if (current_u != previous_u || current_v != previous_v)
				*chroma_changed = true;
			if (*luma_changed && *chroma_changed)
				return true;
		}
	}
	return true;
#endif
}

bool rf_rdp_avc_encoder_name_is_hardware(const char *name)
{
	return name != NULL &&
	       (strcmp(name, "h264_nvenc") == 0 ||
		strcmp(name, "h264_vaapi") == 0 ||
		strcmp(name, "h264_qsv") == 0 ||
		strcmp(name, "h264_amf") == 0 ||
		strcmp(name, "h264_v4l2m2m") == 0);
}

bool rf_rdp_avc_encoder_is_hardware(const RfRdpAvcEncoder *encoder)
{
	return encoder != NULL &&
	       rf_rdp_avc_encoder_name_is_hardware(encoder->name);
}

const char *const *rf_rdp_avc_encoder_auto_candidates(void)
{
	return avc_auto_candidates;
}

bool rf_rdp_avc_encoder_encode_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
)
{
	if (h264_data != NULL)
		*h264_data = NULL;
	if (h264_data_length != NULL)
		*h264_data_length = 0;
	if (encoder == NULL || rgba == NULL || h264_data == NULL ||
	    h264_data_length == NULL)
		return false;

#ifndef RF_HAVE_RDP_AVC
	(void)rgba_length;
	(void)rgba_stride;
	(void)force_keyframe;
	return false;
#else
	const size_t row_bytes = (size_t)encoder->width * 4;
	size_t copy_row_bytes = 0;
	size_t source_rows = 0;
	uint8_t *padded_rgba = NULL;
	const uint8_t *src_rgba = rgba;
	size_t src_stride = rgba_stride;
	bool ok = false;

	if (row_bytes == 0 || !get_rgba_source_geometry(
				    encoder,
				    rgba_length,
				    rgba_stride,
				    &copy_row_bytes,
				    &source_rows
			    ))
		return false;

	if (copy_row_bytes < row_bytes || source_rows < encoder->height) {
		padded_rgba = pad_rgba_source(
			encoder,
			rgba,
			rgba_stride,
			copy_row_bytes,
			source_rows
		);
		if (padded_rgba == NULL)
			return false;
		src_rgba = padded_rgba;
		src_stride = row_bytes;
	}
	if (src_stride > INT_MAX) {
		free(padded_rgba);
		return false;
	}
	if (av_frame_make_writable(encoder->frame) < 0)
		goto out;

	const uint8_t *src_slices[1] = { src_rgba };
	const int src_strides[1] = { (int)src_stride };
	if (sws_scale(
		    encoder->sws,
		    src_slices,
		    src_strides,
		    0,
		    encoder->height,
		    encoder->frame->data,
		    encoder->frame->linesize
	    ) != encoder->height)
		goto out;

	ok = encode_current_frame(
		encoder,
		force_keyframe,
		h264_data,
		h264_data_length
	);

out:
	free(padded_rgba);
	return ok;
	#endif
}

static bool encode_avc444_plane_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	bool chroma,
	uint8_t **h264_data,
	size_t *h264_data_length
)
{
	if (h264_data != NULL)
		*h264_data = NULL;
	if (h264_data_length != NULL)
		*h264_data_length = 0;
	if (encoder == NULL || rgba == NULL || h264_data == NULL ||
	    h264_data_length == NULL)
		return false;

#ifndef RF_HAVE_RDP_AVC
	(void)rgba_length;
	(void)rgba_stride;
	(void)force_keyframe;
	(void)chroma;
	return false;
#else
	const size_t row_bytes = (size_t)encoder->width * 4;
	size_t copy_row_bytes = 0;
	size_t source_rows = 0;
	uint8_t *padded_rgba = NULL;
	const uint8_t *src_rgba = rgba;
	size_t src_stride = rgba_stride;
	bool ok = false;

	if (encoder->width % 2 != 0 || encoder->height % 16 != 0 ||
	    row_bytes == 0 || !get_rgba_source_geometry(
				    encoder,
				    rgba_length,
				    rgba_stride,
				    &copy_row_bytes,
				    &source_rows
			    ))
		return false;

	if (copy_row_bytes < row_bytes || source_rows < encoder->height) {
		padded_rgba = pad_rgba_source(
			encoder,
			rgba,
			rgba_stride,
			copy_row_bytes,
			source_rows
		);
		if (padded_rgba == NULL)
			return false;
		src_rgba = padded_rgba;
		src_stride = row_bytes;
	}

	if (chroma && !prepare_avc444_yuv444_frame(encoder, src_rgba, src_stride))
		goto out;
	if (av_frame_make_writable(encoder->frame) < 0)
		goto out;
	if (chroma) {
		if (!fill_avc444_chroma_nv12_from_yuv444(encoder))
			goto out;
	} else if (!fill_avc444_luma_nv12(encoder, src_rgba, src_stride)) {
		goto out;
	}
	ok = encode_current_frame(
		encoder,
		force_keyframe,
		h264_data,
		h264_data_length
	);

out:
	free(padded_rgba);
	return ok;
#endif
}

bool rf_rdp_avc_encoder_encode_avc444_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t *lc,
	uint8_t **first_h264_data,
	size_t *first_h264_data_length,
	uint8_t **second_h264_data,
	size_t *second_h264_data_length
)
{
	if (lc != NULL)
		*lc = 0;
	if (first_h264_data != NULL)
		*first_h264_data = NULL;
	if (first_h264_data_length != NULL)
		*first_h264_data_length = 0;
	if (second_h264_data != NULL)
		*second_h264_data = NULL;
	if (second_h264_data_length != NULL)
		*second_h264_data_length = 0;
	if (encoder == NULL || rgba == NULL || lc == NULL ||
	    first_h264_data == NULL || first_h264_data_length == NULL ||
	    second_h264_data == NULL || second_h264_data_length == NULL)
		return false;

#ifndef RF_HAVE_RDP_AVC
	(void)rgba_length;
	(void)rgba_stride;
	(void)force_keyframe;
	return false;
#else
	const size_t row_bytes = (size_t)encoder->width * 4;
	size_t copy_row_bytes = 0;
	size_t source_rows = 0;
	uint8_t *padded_rgba = NULL;
	const uint8_t *src_rgba = rgba;
	size_t src_stride = rgba_stride;
	uint8_t *luma = NULL;
	uint8_t *chroma = NULL;
	size_t luma_length = 0;
	size_t chroma_length = 0;
	bool ok = false;

	if (encoder->width % 2 != 0 || encoder->height % 16 != 0 ||
	    row_bytes == 0 || !get_rgba_source_geometry(
				    encoder,
				    rgba_length,
				    rgba_stride,
				    &copy_row_bytes,
				    &source_rows
			    ))
		return false;

	if (copy_row_bytes < row_bytes || source_rows < encoder->height) {
		padded_rgba = pad_rgba_source(
			encoder,
			rgba,
			rgba_stride,
			copy_row_bytes,
			source_rows
		);
		if (padded_rgba == NULL)
			return false;
		src_rgba = padded_rgba;
		src_stride = row_bytes;
	}

	if (!prepare_avc444_yuv444_frame(encoder, src_rgba, src_stride))
		goto out;
	if (av_frame_make_writable(encoder->frame) < 0)
		goto out;
	if (!fill_avc444_luma_nv12(encoder, src_rgba, src_stride))
		goto out;
	if (!encode_current_frame(
		    encoder,
		    force_keyframe,
		    &luma,
		    &luma_length
	    ))
		goto out;

	if (av_frame_make_writable(encoder->frame) < 0)
		goto out;
	if (!fill_avc444_chroma_nv12_from_yuv444(encoder))
		goto out;
	if (!encode_current_frame(
		    encoder,
		    false,
		    &chroma,
		    &chroma_length
	    ))
		goto out;

	*lc = 0;
	*first_h264_data = luma;
	*first_h264_data_length = luma_length;
	*second_h264_data = chroma;
	*second_h264_data_length = chroma_length;
	luma = NULL;
	chroma = NULL;
	ok = true;

out:
	free(chroma);
	free(luma);
	free(padded_rgba);
	return ok;
#endif
}

bool rf_rdp_avc_encoder_encode_avc444_luma_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
)
{
	return encode_avc444_plane_rgba(
		encoder,
		rgba,
		rgba_length,
		rgba_stride,
		force_keyframe,
		false,
		h264_data,
		h264_data_length
	);
}

bool rf_rdp_avc_encoder_encode_avc444_chroma_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
)
{
	return encode_avc444_plane_rgba(
		encoder,
		rgba,
		rgba_length,
		rgba_stride,
		force_keyframe,
		true,
		h264_data,
		h264_data_length
	);
}
