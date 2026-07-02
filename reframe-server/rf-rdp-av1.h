#ifndef __RF_RDP_AV1_H__
#define __RF_RDP_AV1_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct rf_rdp_av1_encoder RfRdpAv1Encoder;

RfRdpAv1Encoder *rf_rdp_av1_encoder_new(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	const char *preferred_encoder
);
RfRdpAv1Encoder *rf_rdp_av1_encoder_new_with_rate(
	uint16_t width,
	uint16_t height,
	unsigned int fps,
	int64_t bit_rate,
	uint8_t qp,
	unsigned int gop_size,
	const char *preferred_encoder
);
void rf_rdp_av1_encoder_free(RfRdpAv1Encoder *encoder);
const char *rf_rdp_av1_encoder_name(const RfRdpAv1Encoder *encoder);
bool rf_rdp_av1_encoder_is_hardware(const RfRdpAv1Encoder *encoder);
bool rf_rdp_av1_encoder_name_is_hardware(const char *name);
const char *const *rf_rdp_av1_encoder_auto_candidates(void);
int64_t rf_rdp_av1_encoder_bit_rate(const RfRdpAv1Encoder *encoder);
uint8_t rf_rdp_av1_encoder_qp(const RfRdpAv1Encoder *encoder);
unsigned int rf_rdp_av1_encoder_gop_size(const RfRdpAv1Encoder *encoder);
bool rf_rdp_av1_encoder_encode_rgba(
	RfRdpAv1Encoder *encoder,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	bool force_keyframe,
	uint8_t **av1_data,
	size_t *av1_data_length
);

#endif
