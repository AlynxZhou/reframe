#ifndef __RF_RDP_NSC_H__
#define __RF_RDP_NSC_H__

#include <stddef.h>
#include <stdint.h>

#include <glib.h>

typedef struct rf_rdp_nsc_context RfRdpNscContext;

RfRdpNscContext *rf_rdp_nsc_context_new(void);
void rf_rdp_nsc_context_free(RfRdpNscContext *context);
GByteArray *rf_rdp_nsc_encode_rgba(
	RfRdpNscContext *context,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t width,
	uint16_t height
);

G_DEFINE_AUTOPTR_CLEANUP_FUNC(RfRdpNscContext, rf_rdp_nsc_context_free)

#endif
