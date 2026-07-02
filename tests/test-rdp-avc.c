#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rf-rdp-avc.h"

#ifdef RF_HAVE_RDP_AVC
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#endif

#ifdef RF_HAVE_RDP_AVC
static bool decode_h264_packet(
	const uint8_t *h264,
	size_t h264_length,
	uint16_t width,
	uint16_t height
)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	AVCodecContext *context = NULL;
	AVPacket *packet = NULL;
	AVFrame *frame = NULL;
	bool decoded = false;

	assert(codec != NULL);
	context = avcodec_alloc_context3(codec);
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	assert(context != NULL);
	assert(packet != NULL);
	assert(frame != NULL);
	assert(h264_length <= INT_MAX);
	assert(av_new_packet(packet, (int)h264_length) == 0);
	memcpy(packet->data, h264, h264_length);
	assert(avcodec_open2(context, codec, NULL) == 0);
	assert(avcodec_send_packet(context, packet) == 0);

	const int rc = avcodec_receive_frame(context, frame);
	decoded = rc == 0 &&
		  frame->width >= width &&
		  frame->height >= height;

	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&context);
	return decoded;
}

static bool decode_h264_sequence(
	const uint8_t *first_h264,
	size_t first_h264_length,
	const uint8_t *second_h264,
	size_t second_h264_length,
	uint16_t width,
	uint16_t height
)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	AVCodecContext *context = NULL;
	AVPacket *packet = NULL;
	AVFrame *frame = NULL;
	bool decoded_first = false;
	bool decoded_second = false;

	assert(codec != NULL);
	context = avcodec_alloc_context3(codec);
	packet = av_packet_alloc();
	frame = av_frame_alloc();
	assert(context != NULL);
	assert(packet != NULL);
	assert(frame != NULL);
	assert(first_h264_length <= INT_MAX);
	assert(second_h264_length <= INT_MAX);
	assert(avcodec_open2(context, codec, NULL) == 0);

	assert(av_new_packet(packet, (int)first_h264_length) == 0);
	memcpy(packet->data, first_h264, first_h264_length);
	assert(avcodec_send_packet(context, packet) == 0);
	decoded_first = avcodec_receive_frame(context, frame) == 0 &&
			frame->width >= width && frame->height >= height;
	av_frame_unref(frame);
	av_packet_unref(packet);

	assert(av_new_packet(packet, (int)second_h264_length) == 0);
	memcpy(packet->data, second_h264, second_h264_length);
	assert(avcodec_send_packet(context, packet) == 0);
	decoded_second = avcodec_receive_frame(context, frame) == 0 &&
			 frame->width >= width && frame->height >= height;

	av_frame_free(&frame);
	av_packet_free(&packet);
	avcodec_free_context(&context);
	return decoded_first && decoded_second;
}
#endif

static void test_avc_auto_candidates_prefer_hardware_before_software(void)
{
	const char *const *candidates = rf_rdp_avc_encoder_auto_candidates();
	const char *expected[] = {
		"h264_nvenc",
		"h264_vaapi",
		"h264_qsv",
		"h264_amf",
		"h264_v4l2m2m",
		"libx264",
		"h264",
		NULL
	};

	for (size_t i = 0; expected[i] != NULL; ++i)
		assert(candidates[i] != NULL && strcmp(candidates[i], expected[i]) == 0);
	assert(candidates[7] == NULL);
}

static void test_avc_encoder_name_hardware_detection(void)
{
	assert(rf_rdp_avc_encoder_name_is_hardware("h264_nvenc"));
	assert(rf_rdp_avc_encoder_name_is_hardware("h264_vaapi"));
	assert(rf_rdp_avc_encoder_name_is_hardware("h264_qsv"));
	assert(rf_rdp_avc_encoder_name_is_hardware("h264_amf"));
	assert(rf_rdp_avc_encoder_name_is_hardware("h264_v4l2m2m"));
	assert(!rf_rdp_avc_encoder_name_is_hardware("libx264"));
	assert(!rf_rdp_avc_encoder_name_is_hardware("h264"));
	assert(!rf_rdp_avc_encoder_name_is_hardware(NULL));
}

static void test_avc_hardware_encoder_rejects_software_encoder(void)
{
	RfRdpAvcEncoder *encoder = rf_rdp_avc_hardware_encoder_new_with_rate(
		320,
		240,
		30,
		2000000,
		28,
		60,
		"libx264"
	);

	assert(encoder == NULL);
}

static RfRdpAvcEncoder *new_test_encoder(
	uint16_t width,
	uint16_t height,
	unsigned int fps
)
{
	const char *forced_encoder = getenv("RF_RDP_AVC_TEST_ENCODER");
	RfRdpAvcEncoder *encoder = NULL;

	if (forced_encoder != NULL && forced_encoder[0] != '\0') {
		encoder = rf_rdp_avc_encoder_new(width, height, fps, forced_encoder);
		assert(encoder != NULL);
		assert(strcmp(rf_rdp_avc_encoder_name(encoder), forced_encoder) == 0);
		return encoder;
	}

	encoder = rf_rdp_avc_encoder_new(width, height, fps, "h264_nvenc");
	if (encoder == NULL)
		encoder = rf_rdp_avc_encoder_new(width, height, fps, NULL);
	assert(encoder != NULL);
	assert(rf_rdp_avc_encoder_name(encoder) != NULL);
	return encoder;
}

static void test_avc_encoder_outputs_h264(void)
{
	const uint16_t width = 1260;
	const uint16_t height = 656;
	uint8_t *rgba = calloc((size_t)width * height, 4);
	uint8_t *h264 = NULL;
	size_t h264_length = 0;

	assert(rgba != NULL);
	for (size_t i = 0; i < (size_t)width * height * 4; i += 4) {
		rgba[i] = 0x20;
		rgba[i + 1] = 0x80;
		rgba[i + 2] = 0xc0;
		rgba[i + 3] = 0xff;
	}

	RfRdpAvcEncoder *encoder = new_test_encoder(width, height, 15);

	assert(rf_rdp_avc_encoder_encode_rgba(
			encoder,
			rgba,
			(size_t)width * height * 4,
			(size_t)width * 4,
			true,
			&h264,
		&h264_length
	));
	assert(h264 != NULL);
	assert(h264_length > 4);
	assert((h264[0] == 0 && h264[1] == 0 && h264[2] == 1) ||
	       (h264[0] == 0 && h264[1] == 0 && h264[2] == 0 && h264[3] == 1));
	const char *dump_path = getenv("RF_RDP_AVC_DUMP");
	if (dump_path != NULL && dump_path[0] != '\0') {
		FILE *dump = fopen(dump_path, "wb");
		assert(dump != NULL);
		assert(fwrite(h264, 1, h264_length, dump) == h264_length);
		assert(fclose(dump) == 0);
	}
#ifdef RF_HAVE_RDP_AVC
	assert(decode_h264_packet(h264, h264_length, width, height));
#endif

	free(h264);
	free(rgba);
	rf_rdp_avc_encoder_free(encoder);
}

static void test_avc_encoder_pads_short_rgba_source(void)
{
	const uint16_t visible_width = 319;
	const uint16_t visible_height = 201;
	const uint16_t coded_width = 320;
	const uint16_t coded_height = 208;
	uint8_t *rgba = calloc((size_t)visible_width * visible_height, 4);
	uint8_t *h264 = NULL;
	size_t h264_length = 0;

	assert(rgba != NULL);
	for (size_t i = 0; i < (size_t)visible_width * visible_height * 4; i += 4) {
		rgba[i] = (uint8_t)(i / 4);
		rgba[i + 1] = 0x70;
		rgba[i + 2] = 0xd0;
		rgba[i + 3] = 0xff;
	}

	RfRdpAvcEncoder *encoder = new_test_encoder(coded_width, coded_height, 15);

	assert(rf_rdp_avc_encoder_encode_rgba(
		encoder,
		rgba,
		(size_t)visible_width * visible_height * 4,
		(size_t)visible_width * 4,
		true,
		&h264,
		&h264_length
	));
	assert(h264 != NULL);
	assert(h264_length > 4);
#ifdef RF_HAVE_RDP_AVC
	assert(decode_h264_packet(h264, h264_length, coded_width, coded_height));
#endif

	free(h264);
	free(rgba);
	rf_rdp_avc_encoder_free(encoder);
}

static void test_avc_encoder_accepts_rate_control_settings(void)
{
	RfRdpAvcEncoder *encoder = rf_rdp_avc_encoder_new_with_rate(
		320,
		240,
		60,
		4000000,
		34,
		120,
		getenv("RF_RDP_AVC_TEST_ENCODER")
	);

	assert(encoder != NULL);
	assert(rf_rdp_avc_encoder_bit_rate(encoder) == 4000000);
	assert(rf_rdp_avc_encoder_qp(encoder) == 34);
	assert(rf_rdp_avc_encoder_gop_size(encoder) == 120);
	rf_rdp_avc_encoder_free(encoder);
}

static void test_avc444_signatures_detect_luma_and_chroma_changes(void)
{
	const uint8_t black[] = { 0x00, 0x00, 0x00, 0xff };
	const uint8_t gray[] = { 0x80, 0x80, 0x80, 0xff };
	const uint8_t red[] = { 0xff, 0x00, 0x00, 0xff };
	const uint8_t green_same_luma[] = { 0x00, 0x4b, 0x00, 0xff };
	uint64_t black_luma = 0;
	uint64_t black_chroma = 0;
	uint64_t gray_luma = 0;
	uint64_t gray_chroma = 0;
	uint64_t red_luma = 0;
	uint64_t red_chroma = 0;
	uint64_t green_luma = 0;
	uint64_t green_chroma = 0;

	assert(rf_rdp_avc_compute_avc444_signatures(
		black,
		sizeof(black),
		4,
		1,
		1,
		&black_luma,
		&black_chroma
	));
	assert(rf_rdp_avc_compute_avc444_signatures(
		gray,
		sizeof(gray),
		4,
		1,
		1,
		&gray_luma,
		&gray_chroma
	));
	assert(rf_rdp_avc_compute_avc444_signatures(
		red,
		sizeof(red),
		4,
		1,
		1,
		&red_luma,
		&red_chroma
	));
	assert(rf_rdp_avc_compute_avc444_signatures(
		green_same_luma,
		sizeof(green_same_luma),
		4,
		1,
		1,
		&green_luma,
		&green_chroma
	));

	assert(black_luma != gray_luma);
	assert(black_chroma == gray_chroma);
	assert(red_luma == green_luma);
	assert(red_chroma != green_chroma);
}

static void test_avc444_delta_detects_changed_planes_inside_damage(void)
{
	const uint8_t black[] = { 0x00, 0x00, 0x00, 0xff };
	const uint8_t gray[] = { 0x80, 0x80, 0x80, 0xff };
	const uint8_t red[] = { 0xff, 0x00, 0x00, 0xff };
	const uint8_t green_same_luma[] = { 0x00, 0x4b, 0x00, 0xff };
	bool luma_changed = true;
	bool chroma_changed = true;

	assert(rf_rdp_avc_compare_avc444_rect(
		gray,
		sizeof(gray),
		black,
		sizeof(black),
		4,
		0,
		0,
		1,
		1,
		&luma_changed,
		&chroma_changed
	));
	assert(luma_changed);
	assert(!chroma_changed);

	assert(rf_rdp_avc_compare_avc444_rect(
		green_same_luma,
		sizeof(green_same_luma),
		red,
		sizeof(red),
		4,
		0,
		0,
		1,
		1,
		&luma_changed,
		&chroma_changed
	));
	assert(!luma_changed);
	assert(chroma_changed);
}

static void test_avc444_delta_ignores_changes_outside_damage(void)
{
	uint8_t previous[] = {
		0x00, 0x00, 0x00, 0xff,
		0x00, 0x00, 0x00, 0xff,
		0x00, 0x00, 0x00, 0xff,
		0x00, 0x00, 0x00, 0xff
	};
	uint8_t current[] = {
		0xff, 0xff, 0xff, 0xff,
		0x00, 0x00, 0x00, 0xff,
		0x00, 0x00, 0x00, 0xff,
		0x00, 0x00, 0x00, 0xff
	};
	bool luma_changed = true;
	bool chroma_changed = true;

	assert(rf_rdp_avc_compare_avc444_rect(
		current,
		sizeof(current),
		previous,
		sizeof(previous),
		2 * 4,
		1,
		1,
		1,
		1,
		&luma_changed,
		&chroma_changed
	));
	assert(!luma_changed);
	assert(!chroma_changed);

	assert(!rf_rdp_avc_compare_avc444_rect(
		current,
		sizeof(current),
		previous,
		sizeof(previous),
		2 * 4,
		2,
		0,
		1,
		1,
		&luma_changed,
		&chroma_changed
	));
	assert(!luma_changed);
	assert(!chroma_changed);
}

static void test_avc444_encoder_outputs_luma_and_chroma_h264(void)
{
	const uint16_t width = 320;
	const uint16_t height = 240;
	uint8_t *rgba = calloc((size_t)width * height, 4);
	uint8_t *luma = NULL;
	uint8_t *chroma = NULL;
	size_t luma_length = 0;
	size_t chroma_length = 0;
	uint8_t lc = 0xff;

	assert(rgba != NULL);
	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			const size_t offset = ((size_t)y * width + x) * 4;
			rgba[offset] = (uint8_t)(x & 0xff);
			rgba[offset + 1] = (uint8_t)(y & 0xff);
			rgba[offset + 2] = (uint8_t)((x + y) & 0xff);
			rgba[offset + 3] = 0xff;
		}
	}

	RfRdpAvcEncoder *encoder = new_test_encoder(width, height, 30);

	assert(rf_rdp_avc_encoder_encode_avc444_rgba(
		encoder,
		rgba,
		(size_t)width * height * 4,
		(size_t)width * 4,
		true,
		&lc,
		&luma,
		&luma_length,
		&chroma,
		&chroma_length
	));
	assert(lc == 0);
	assert(luma != NULL);
	assert(chroma != NULL);
	assert(luma_length > 4);
	assert(chroma_length > 4);
#ifdef RF_HAVE_RDP_AVC
	assert(decode_h264_packet(luma, luma_length, width, height));
	assert(decode_h264_sequence(
		luma,
		luma_length,
		chroma,
		chroma_length,
		width,
		height
	));
#endif

	free(chroma);
	free(luma);
	free(rgba);
	rf_rdp_avc_encoder_free(encoder);
}

static void test_avc444_combined_encode_prepares_source_once(void)
{
#ifdef RF_HAVE_RDP_AVC
	const uint16_t width = 320;
	const uint16_t height = 240;
	uint8_t *rgba = calloc((size_t)width * height, 4);
	uint8_t *luma = NULL;
	uint8_t *chroma = NULL;
	size_t luma_length = 0;
	size_t chroma_length = 0;
	uint8_t lc = 0xff;

	assert(rgba != NULL);
	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			const size_t offset = ((size_t)y * width + x) * 4;
			rgba[offset] = (uint8_t)(x * 3u);
			rgba[offset + 1] = (uint8_t)(y * 5u);
			rgba[offset + 2] = (uint8_t)(x + y * 7u);
			rgba[offset + 3] = 0xff;
		}
	}

	RfRdpAvcEncoder *encoder = new_test_encoder(width, height, 30);
	const uint64_t before = rf_rdp_avc_encoder_avc444_prepare_count(encoder);

	assert(rf_rdp_avc_encoder_encode_avc444_rgba(
		encoder,
		rgba,
		(size_t)width * height * 4,
		(size_t)width * 4,
		true,
		&lc,
		&luma,
		&luma_length,
		&chroma,
		&chroma_length
	));
	assert(lc == 0);
	assert(luma != NULL);
	assert(chroma != NULL);
	assert(luma_length > 4);
	assert(chroma_length > 4);
	assert(rf_rdp_avc_encoder_avc444_prepare_count(encoder) == before + 1);

	free(chroma);
	free(luma);
	free(rgba);
	rf_rdp_avc_encoder_free(encoder);
#endif
}

static void test_avc444_chroma_encode_prepares_source_once(void)
{
#ifdef RF_HAVE_RDP_AVC
	const uint16_t width = 320;
	const uint16_t height = 240;
	uint8_t *rgba = calloc((size_t)width * height, 4);
	uint8_t *chroma = NULL;
	size_t chroma_length = 0;

	assert(rgba != NULL);
	for (uint16_t y = 0; y < height; ++y) {
		for (uint16_t x = 0; x < width; ++x) {
			const size_t offset = ((size_t)y * width + x) * 4;
			rgba[offset] = (uint8_t)(0x40u + x);
			rgba[offset + 1] = (uint8_t)(0x20u + y);
			rgba[offset + 2] = (uint8_t)(x * 5u + y * 3u);
			rgba[offset + 3] = 0xff;
		}
	}

	RfRdpAvcEncoder *encoder = new_test_encoder(width, height, 30);
	const uint64_t before = rf_rdp_avc_encoder_avc444_prepare_count(encoder);

	assert(rf_rdp_avc_encoder_encode_avc444_chroma_rgba(
		encoder,
		rgba,
		(size_t)width * height * 4,
		(size_t)width * 4,
		true,
		&chroma,
		&chroma_length
	));
	assert(chroma != NULL);
	assert(chroma_length > 4);
	assert(rf_rdp_avc_encoder_avc444_prepare_count(encoder) == before + 1);
	assert(decode_h264_packet(chroma, chroma_length, width, height));

	free(chroma);
	free(rgba);
	rf_rdp_avc_encoder_free(encoder);
#endif
}

int main(void)
{
	test_avc_auto_candidates_prefer_hardware_before_software();
	test_avc_encoder_name_hardware_detection();
	test_avc_hardware_encoder_rejects_software_encoder();
	test_avc_encoder_outputs_h264();
	test_avc_encoder_pads_short_rgba_source();
	test_avc_encoder_accepts_rate_control_settings();
	test_avc444_signatures_detect_luma_and_chroma_changes();
	test_avc444_delta_detects_changed_planes_inside_damage();
	test_avc444_delta_ignores_changes_outside_damage();
	test_avc444_encoder_outputs_luma_and_chroma_h264();
	test_avc444_combined_encode_prepares_source_once();
	test_avc444_chroma_encode_prepares_source_once();
	return 0;
}
