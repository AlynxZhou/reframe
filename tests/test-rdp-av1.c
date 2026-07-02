#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "rf-rdp-av1.h"

#ifdef RF_HAVE_RDP_AVC
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
#endif

#ifdef RF_HAVE_RDP_AVC
static enum AVPixelFormat decode_av1_packet(
	const uint8_t *av1,
	size_t av1_length,
	uint16_t width,
	uint16_t height
)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AV1);
	AVCodecContext *context = NULL;
	AVPacket *packet = NULL;
	AVFrame *frame = NULL;
	enum AVPixelFormat pix_fmt = AV_PIX_FMT_NONE;

	assert(codec != NULL);
	context = avcodec_alloc_context3(codec);
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	assert(context != NULL);
	assert(packet != NULL);
	assert(frame != NULL);
	assert(av1_length <= INT_MAX);
	assert(av_new_packet(packet, (int)av1_length) == 0);
	memcpy(packet->data, av1, av1_length);
	assert(avcodec_open2(context, codec, NULL) == 0);
	assert(avcodec_send_packet(context, packet) == 0);

	const int rc = avcodec_receive_frame(context, frame);
	if (rc == 0 && frame->width >= width && frame->height >= height)
		pix_fmt = frame->format;

	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&context);
	return pix_fmt;
}

static bool is_av1_i420_format(enum AVPixelFormat pix_fmt)
{
	return pix_fmt == AV_PIX_FMT_YUV420P ||
	       pix_fmt == AV_PIX_FMT_NV12 ||
	       pix_fmt == AV_PIX_FMT_CUDA;
}

static bool is_av1_i444_format(enum AVPixelFormat pix_fmt)
{
	return pix_fmt == AV_PIX_FMT_YUV444P ||
	       pix_fmt == AV_PIX_FMT_GBRP ||
	       pix_fmt == AV_PIX_FMT_CUDA;
}
#endif

static void test_av1_auto_candidates_prefer_hardware_before_software(void)
{
	const char *const *candidates = rf_rdp_av1_encoder_auto_candidates();
	const char *expected[] = {
		"av1_nvenc",
		"av1_vaapi",
		"av1_qsv",
		"av1_amf",
		"libaom-av1",
		NULL
	};

	for (size_t i = 0; expected[i] != NULL; ++i)
		assert(candidates[i] != NULL && strcmp(candidates[i], expected[i]) == 0);
	assert(candidates[5] == NULL);
}

static void test_av1_encoder_name_hardware_detection(void)
{
	assert(rf_rdp_av1_encoder_name_is_hardware("av1_nvenc"));
	assert(rf_rdp_av1_encoder_name_is_hardware("av1_vaapi"));
	assert(rf_rdp_av1_encoder_name_is_hardware("av1_qsv"));
	assert(rf_rdp_av1_encoder_name_is_hardware("av1_amf"));
	assert(!rf_rdp_av1_encoder_name_is_hardware("libaom-av1"));
	assert(!rf_rdp_av1_encoder_name_is_hardware(NULL));
}

static void test_av1_hardware_encoder_rejects_software_i444(void)
{
	RfRdpAv1Encoder *encoder =
		rf_rdp_av1_hardware_encoder_new_with_rate_and_mode(
			320,
			240,
			30,
			2000000,
			28,
			60,
			RF_RDP_AV1_MODE_I444,
			"libaom-av1"
		);

	assert(encoder == NULL);
}

static RfRdpAv1Encoder *new_test_encoder(
	uint16_t width,
	uint16_t height,
	unsigned int fps
)
{
	const char *forced_encoder = getenv("RF_RDP_AV1_TEST_ENCODER");
	RfRdpAv1Encoder *encoder = NULL;

	if (forced_encoder != NULL && forced_encoder[0] != '\0') {
		encoder = rf_rdp_av1_encoder_new(width, height, fps, forced_encoder);
		assert(encoder != NULL);
		assert(strcmp(rf_rdp_av1_encoder_name(encoder), forced_encoder) == 0);
		return encoder;
	}

	encoder = rf_rdp_av1_encoder_new(width, height, fps, "av1_nvenc");
	if (encoder == NULL)
		encoder = rf_rdp_av1_encoder_new(width, height, fps, NULL);
	assert(encoder != NULL);
	assert(rf_rdp_av1_encoder_name(encoder) != NULL);
	return encoder;
}

static void test_av1_encoder_outputs_av1(void)
{
	const uint16_t width = 320;
	const uint16_t height = 240;
	uint8_t *rgba = calloc((size_t)width * height, 4);
	uint8_t *av1 = NULL;
	size_t av1_length = 0;

	assert(rgba != NULL);
	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			const size_t offset = ((size_t)y * width + x) * 4;
			rgba[offset] = (uint8_t)(x * 2u);
			rgba[offset + 1] = (uint8_t)(y * 3u);
			rgba[offset + 2] = (uint8_t)(0x80u + x + y);
			rgba[offset + 3] = 0xff;
		}
	}

	RfRdpAv1Encoder *encoder = new_test_encoder(width, height, 30);

	assert(rf_rdp_av1_encoder_encode_rgba(
		encoder,
		rgba,
		(size_t)width * height * 4,
		(size_t)width * 4,
		true,
		&av1,
		&av1_length
	));
	assert(av1 != NULL);
	assert(av1_length > 4);
#ifdef RF_HAVE_RDP_AVC
	assert(is_av1_i420_format(
		decode_av1_packet(av1, av1_length, width, height)
	));
#endif

	free(av1);
	free(rgba);
	rf_rdp_av1_encoder_free(encoder);
}

static void test_av1_encoder_outputs_i444_when_requested(void)
{
	const uint16_t width = 320;
	const uint16_t height = 240;
	uint8_t *rgba = calloc((size_t)width * height, 4);
	uint8_t *av1 = NULL;
	size_t av1_length = 0;
	const char *forced_encoder = getenv("RF_RDP_AV1_TEST_ENCODER");
	const char *preferred_encoder =
		forced_encoder != NULL && forced_encoder[0] != '\0' ?
			forced_encoder :
			"av1_nvenc";

	assert(rgba != NULL);
	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			const size_t offset = ((size_t)y * width + x) * 4;
			rgba[offset] = (uint8_t)(x * 2u);
			rgba[offset + 1] = (uint8_t)(y * 5u);
			rgba[offset + 2] = (uint8_t)(0xffu - x - y);
			rgba[offset + 3] = 0xff;
		}
	}

	RfRdpAv1Encoder *encoder = rf_rdp_av1_encoder_new_with_rate_and_mode(
		width,
		height,
		30,
		2000000,
		28,
		60,
		RF_RDP_AV1_MODE_I444,
		preferred_encoder
	);
	assert(encoder != NULL);
	assert(rf_rdp_av1_encoder_mode(encoder) == RF_RDP_AV1_MODE_I444);

	assert(rf_rdp_av1_encoder_encode_rgba(
		encoder,
		rgba,
		(size_t)width * height * 4,
		(size_t)width * 4,
		true,
		&av1,
		&av1_length
	));
	assert(av1 != NULL);
	assert(av1_length > 4);
#ifdef RF_HAVE_RDP_AVC
	assert(is_av1_i444_format(
		decode_av1_packet(av1, av1_length, width, height)
	));
#endif

	free(av1);
	free(rgba);
	rf_rdp_av1_encoder_free(encoder);
}

int main(void)
{
	test_av1_auto_candidates_prefer_hardware_before_software();
	test_av1_encoder_name_hardware_detection();
	test_av1_hardware_encoder_rejects_software_i444();
	test_av1_encoder_outputs_av1();
	test_av1_encoder_outputs_i444_when_requested();
	return 0;
}
