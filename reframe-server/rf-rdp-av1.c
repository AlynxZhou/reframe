#include "rf-rdp-av1.h"

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

static const char *const av1_auto_candidates[] = {
	"av1_nvenc",
	"av1_vaapi",
	"av1_qsv",
	"av1_amf",
	"libaom-av1",
	NULL
};

struct rf_rdp_av1_encoder {
#ifdef RF_HAVE_RDP_AVC
	AVCodecContext *codec;
	AVFrame *frame;
	AVFrame *hw_frame;
	AVPacket *packet;
	AVBufferRef *hw_device_ctx;
	AVBufferRef *hw_frames_ctx;
	struct SwsContext *sws;
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
	enum rf_rdp_av1_mode mode;
	int64_t pts;
};

static int64_t default_bit_rate(uint16_t width, uint16_t height, unsigned int fps)
{
	int64_t bits = (int64_t)width * height * fps / 12;
	if (bits < 1500000)
		return 1500000;
	if (bits > 20000000)
		return 20000000;
	return bits;
}

#ifdef RF_HAVE_RDP_AVC
static void trace_av_error(const char *step, int rc)
{
	if (getenv("RF_RDP_AV1_TRACE") == NULL)
		return;

	char message[AV_ERROR_MAX_STRING_SIZE] = { 0 };
	av_strerror(rc, message, sizeof(message));
	fprintf(stderr, "RDP AV1: %s failed: %s (%d)\n", step, message, rc);
}

static void set_encoder_options(RfRdpAv1Encoder *encoder, const char *name)
{
	AVCodecContext *codec = encoder != NULL ? encoder->codec : NULL;
	if (codec == NULL || codec->priv_data == NULL || name == NULL)
		return;

	char qp[8] = { 0 };
	snprintf(qp, sizeof(qp), "%u", encoder->qp);

	if (strcmp(name, "av1_nvenc") == 0) {
		av_opt_set(codec->priv_data, "preset", "p4", 0);
		av_opt_set(codec->priv_data, "tune", "ull", 0);
		av_opt_set(codec->priv_data, "rc", "cbr", 0);
		av_opt_set(codec->priv_data, "delay", "0", 0);
		av_opt_set(codec->priv_data, "zerolatency", "1", 0);
		av_opt_set(codec->priv_data, "forced-idr", "1", 0);
		av_opt_set(codec->priv_data, "qp", qp, 0);
	} else if (strcmp(name, "av1_vaapi") == 0) {
		av_opt_set(codec->priv_data, "async_depth", "1", 0);
		av_opt_set(codec->priv_data, "rc_mode", "CBR", 0);
		av_opt_set(codec->priv_data, "quality", "4", 0);
		av_opt_set(codec->priv_data, "qp", qp, 0);
	} else if (strcmp(name, "libaom-av1") == 0) {
		av_opt_set(codec->priv_data, "usage", "realtime", 0);
		av_opt_set(codec->priv_data, "cpu-used", "8", 0);
		av_opt_set(codec->priv_data, "lag-in-frames", "0", 0);
		av_opt_set(codec->priv_data, "row-mt", "1", 0);
		av_opt_set(codec->priv_data, "error-resilient", "1", 0);
	}
}

static void close_encoder_state(RfRdpAv1Encoder *encoder)
{
	if (encoder == NULL)
		return;

	sws_freeContext(encoder->sws);
	encoder->sws = NULL;
	av_packet_free(&encoder->packet);
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

static bool setup_vaapi_frames(RfRdpAv1Encoder *encoder)
{
	if (encoder->mode != RF_RDP_AV1_MODE_I420)
		return false;
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

static bool configure_encoder_context(RfRdpAv1Encoder *encoder, const char *name)
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
	encoder->sw_pix_fmt = encoder->mode == RF_RDP_AV1_MODE_I444 ?
		AV_PIX_FMT_YUV444P :
		AV_PIX_FMT_NV12;

	if (strcmp(name, "av1_vaapi") == 0) {
		encoder->hardware_frames = true;
		encoder->codec->pix_fmt = AV_PIX_FMT_VAAPI;
		if (!setup_vaapi_frames(encoder))
			return false;
	} else {
		encoder->hardware_frames = false;
		if (strcmp(name, "libaom-av1") == 0 &&
		    encoder->mode == RF_RDP_AV1_MODE_I420)
			encoder->sw_pix_fmt = AV_PIX_FMT_YUV420P;
		encoder->codec->pix_fmt = encoder->sw_pix_fmt;
	}

	set_encoder_options(encoder, name);
	return true;
}

static bool open_encoder(RfRdpAv1Encoder *encoder, const char *name)
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

	encoder->name = strdup(name);
	if (encoder->name == NULL)
		goto fail;
	return true;

fail:
	close_encoder_state(encoder);
	return false;
}

static bool get_rgba_source_geometry(
	const RfRdpAv1Encoder *encoder,
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
	const RfRdpAv1Encoder *encoder,
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

static bool encode_current_frame(
	RfRdpAv1Encoder *encoder,
	bool force_keyframe,
	uint8_t **av1_data,
	size_t *av1_data_length
)
{
	AVFrame *encode_frame = encoder->frame;

	*av1_data = NULL;
	*av1_data_length = 0;

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

	*av1_data = malloc(encoder->packet->size);
	if (*av1_data == NULL) {
		av_packet_unref(encoder->packet);
		return false;
	}
	memcpy(*av1_data, encoder->packet->data, encoder->packet->size);
	*av1_data_length = encoder->packet->size;
	av_packet_unref(encoder->packet);
	return *av1_data_length > 0;
}
#endif

void rf_rdp_av1_encoder_free(RfRdpAv1Encoder *encoder)
{
	if (encoder == NULL)
		return;

#ifdef RF_HAVE_RDP_AVC
	close_encoder_state(encoder);
#endif
	free(encoder);
}

RfRdpAv1Encoder *rf_rdp_av1_encoder_new_with_rate_and_mode(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	enum rf_rdp_av1_mode mode,
	const char *preferred_encoder
)
{
	if (width == 0 || height == 0 || fps == 0 || qp > 63 ||
	    (mode != RF_RDP_AV1_MODE_I420 && mode != RF_RDP_AV1_MODE_I444))
		return NULL;

	RfRdpAv1Encoder *encoder = calloc(1, sizeof(*encoder));
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
	encoder->mode = mode;

#ifdef RF_HAVE_RDP_AVC
	const bool prefer_auto = preferred_encoder == NULL ||
		preferred_encoder[0] == '\0' ||
		strcmp(preferred_encoder, "auto") == 0;
	if (!prefer_auto &&
	    open_encoder(encoder, preferred_encoder))
		return encoder;

	for (const char *const *candidate = av1_auto_candidates;
	     *candidate != NULL;
	     ++candidate) {
		if (!prefer_auto &&
		    strcmp(preferred_encoder, *candidate) == 0)
			continue;
		if (open_encoder(encoder, *candidate))
			return encoder;
	}
#else
	(void)preferred_encoder;
#endif

	rf_rdp_av1_encoder_free(encoder);
	return NULL;
}

RfRdpAv1Encoder *rf_rdp_av1_encoder_new_with_rate(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder
)
{
	return rf_rdp_av1_encoder_new_with_rate_and_mode(
		width,
		height,
		fps,
		bit_rate,
		qp,
		gop_size,
		RF_RDP_AV1_MODE_I420,
		preferred_encoder
	);
}

RfRdpAv1Encoder *rf_rdp_av1_encoder_new(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	const char *preferred_encoder
)
{
	return rf_rdp_av1_encoder_new_with_rate(
		width,
		height,
		fps,
		0,
		30,
		0,
		preferred_encoder
	);
}

const char *rf_rdp_av1_encoder_name(const RfRdpAv1Encoder *encoder)
{
	return encoder != NULL ? encoder->name : NULL;
}

enum rf_rdp_av1_mode rf_rdp_av1_encoder_mode(const RfRdpAv1Encoder *encoder)
{
	return encoder != NULL ? encoder->mode : RF_RDP_AV1_MODE_I420;
}

int64_t rf_rdp_av1_encoder_bit_rate(const RfRdpAv1Encoder *encoder)
{
	return encoder != NULL ? encoder->bit_rate : 0;
}

uint8_t rf_rdp_av1_encoder_qp(const RfRdpAv1Encoder *encoder)
{
	return encoder != NULL ? encoder->qp : 0;
}

unsigned int rf_rdp_av1_encoder_gop_size(const RfRdpAv1Encoder *encoder)
{
	return encoder != NULL ? encoder->gop_size : 0;
}

bool rf_rdp_av1_encoder_name_is_hardware(const char *name)
{
	return name != NULL &&
	       (strcmp(name, "av1_nvenc") == 0 ||
		strcmp(name, "av1_vaapi") == 0 ||
		strcmp(name, "av1_qsv") == 0 ||
		strcmp(name, "av1_amf") == 0);
}

bool rf_rdp_av1_encoder_is_hardware(const RfRdpAv1Encoder *encoder)
{
	return encoder != NULL &&
	       rf_rdp_av1_encoder_name_is_hardware(encoder->name);
}

const char *const *rf_rdp_av1_encoder_auto_candidates(void)
{
	return av1_auto_candidates;
}

bool rf_rdp_av1_encoder_encode_rgba(
	RfRdpAv1Encoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **av1_data,
	size_t *av1_data_length
)
{
	if (av1_data != NULL)
		*av1_data = NULL;
	if (av1_data_length != NULL)
		*av1_data_length = 0;
	if (encoder == NULL || rgba == NULL || av1_data == NULL ||
	    av1_data_length == NULL)
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
		av1_data,
		av1_data_length
	);

out:
	free(padded_rgba);
	return ok;
#endif
}
