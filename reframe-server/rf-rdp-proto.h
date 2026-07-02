#ifndef __RF_RDP_PROTO_H__
#define __RF_RDP_PROTO_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_PROTOCOL_RDP 0x00000000u
#define RF_RDP_PROTOCOL_SSL 0x00000001u
#define RF_RDP_PROTOCOL_HYBRID 0x00000002u
#define RF_RDP_PROTOCOL_HYBRID_EX 0x00000008u

#define RF_RDP_NEG_FAILURE_SSL_REQUIRED 0x00000001u
#define RF_RDP_NEG_FAILURE_HYBRID_REQUIRED 0x00000005u

struct rf_rdp_connection_request {
	bool has_negotiation;
	uint32_t requested_protocols;
};

bool rf_rdp_read_tpkt_header(
	const uint8_t *data,
	size_t length,
	uint16_t *tpkt_length
);
bool rf_rdp_parse_connection_request(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_connection_request *request
);
size_t rf_rdp_write_negotiation_response(
	uint8_t *data,
	size_t capacity,
	uint32_t selected_protocol
);
size_t rf_rdp_write_negotiation_failure(
	uint8_t *data,
	size_t capacity,
	uint32_t failure_code
);

#endif
