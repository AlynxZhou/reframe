#ifndef __RF_RDP_AVC_H__
#define __RF_RDP_AVC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rf_rdp_avc_encoder RfRdpAvcEncoder;

RfRdpAvcEncoder *rf_rdp_avc_encoder_new(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	const char *preferred_encoder
);
RfRdpAvcEncoder *rf_rdp_avc_encoder_new_with_rate(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder
);
RfRdpAvcEncoder *rf_rdp_avc_hardware_encoder_new_with_rate(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder
);
void rf_rdp_avc_encoder_free(RfRdpAvcEncoder *encoder);
const char *rf_rdp_avc_encoder_name(const RfRdpAvcEncoder *encoder);
bool rf_rdp_avc_encoder_is_hardware(const RfRdpAvcEncoder *encoder);
bool rf_rdp_avc_encoder_name_is_hardware(const char *name);
const char *const *rf_rdp_avc_encoder_auto_candidates(void);
int64_t rf_rdp_avc_encoder_bit_rate(const RfRdpAvcEncoder *encoder);
uint8_t rf_rdp_avc_encoder_qp(const RfRdpAvcEncoder *encoder);
unsigned int rf_rdp_avc_encoder_gop_size(const RfRdpAvcEncoder *encoder);
uint64_t rf_rdp_avc_encoder_avc444_prepare_count(const RfRdpAvcEncoder *encoder);
bool rf_rdp_avc_compute_avc444_signatures(
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t width,
	uint16_t height,
	uint64_t *luma_signature,
	uint64_t *chroma_signature
);
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
);
bool rf_rdp_avc_encoder_encode_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
);
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
);
bool rf_rdp_avc_encoder_encode_avc444_v2_rgba(
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
);
bool rf_rdp_avc_encoder_encode_avc444_luma_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
);
bool rf_rdp_avc_encoder_encode_avc444_chroma_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
);
bool rf_rdp_avc_encoder_encode_avc444_v2_chroma_rgba(
	RfRdpAvcEncoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **h264_data,
	size_t *h264_data_length
);

#endif
