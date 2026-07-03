#ifndef __RF_RDP_RFX_H__
#define __RF_RDP_RFX_H__

#include <stddef.h>
#include <stdint.h>

#include <glib.h>

typedef struct rf_rdp_rfx_context RfRdpRfxContext;

RfRdpRfxContext *rf_rdp_rfx_context_new(void);
void rf_rdp_rfx_context_free(RfRdpRfxContext *context);
void rf_rdp_rfx_context_reset(RfRdpRfxContext *context);
void rf_rdp_rfx_context_set_thread_count(
	RfRdpRfxContext *context,
	unsigned int thread_count
);
unsigned int rf_rdp_rfx_context_get_thread_count(const RfRdpRfxContext *context);
void rf_rdp_rfx_context_set_quality_level(
	RfRdpRfxContext *context,
	unsigned int quality_level,
	unsigned int max_quality_level
);
unsigned int rf_rdp_rfx_context_get_quality_level(
	const RfRdpRfxContext *context
);

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
);
GByteArray *rf_rdp_rfx_encode_progressive_rgba(
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
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RfRdpRfxContext, rf_rdp_rfx_context_free)

#endif
