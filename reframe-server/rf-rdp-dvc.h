#ifndef __RF_RDP_DVC_H__
#define __RF_RDP_DVC_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_DVC_RDPGFX_CHANNEL_NAME "Microsoft::Windows::RDS::Graphics"

#define RF_RDP_DVC_VERSION_3 3u

#define RF_RDP_DVC_CREATE_REQUEST_PDU 0x01u
#define RF_RDP_DVC_DATA_FIRST_PDU 0x02u
#define RF_RDP_DVC_DATA_PDU 0x03u
#define RF_RDP_DVC_CLOSE_REQUEST_PDU 0x04u
#define RF_RDP_DVC_CAPABILITY_REQUEST_PDU 0x05u

#define RF_RDP_DVC_CHANNEL_FLAG_FIRST 0x00000001u
#define RF_RDP_DVC_CHANNEL_FLAG_LAST 0x00000002u
#define RF_RDP_DVC_CHANNEL_FLAG_SHOW_PROTOCOL 0x00000010u
#define RF_RDP_DVC_CHANNEL_CHUNK_LENGTH 1600u

struct rf_rdp_dvc_create_response {
	uint32_t channel_id;
	uint32_t status;
};

struct rf_rdp_dvc_channel_pdu {
	size_t payload_offset;
	size_t payload_length;
	uint32_t total_length;
	uint32_t flags;
};

struct rf_rdp_dvc_data_pdu {
	uint32_t channel_id;
	size_t payload_offset;
	size_t payload_length;
};

size_t rf_rdp_dvc_write_capability_request(
	uint8_t *data,
	size_t capacity,
	uint16_t version
);
size_t rf_rdp_dvc_write_channel_pdu(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
);
size_t rf_rdp_dvc_write_channel_pdu_with_flags(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length,
	uint32_t extra_flags
);
bool rf_rdp_dvc_parse_channel_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_dvc_channel_pdu *pdu
);
size_t rf_rdp_dvc_write_create_request(
	uint8_t *data,
	size_t capacity,
	uint32_t channel_id,
	const char *channel_name
);
bool rf_rdp_dvc_parse_create_response(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_dvc_create_response *response
);
bool rf_rdp_dvc_parse_capability_response(
	const uint8_t *data,
	size_t length,
	uint16_t *version
);
bool rf_rdp_dvc_parse_data_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_dvc_data_pdu *pdu
);
size_t rf_rdp_dvc_write_data(
	uint8_t *data,
	size_t capacity,
	uint32_t channel_id,
	const uint8_t *payload,
	size_t payload_length
);
size_t rf_rdp_dvc_write_data_first(
	uint8_t *data,
	size_t capacity,
	uint32_t channel_id,
	uint32_t total_length,
	const uint8_t *payload,
	size_t payload_length
);

#endif
