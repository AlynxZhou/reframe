#ifndef __RF_RDP_MCS_H__
#define __RF_RDP_MCS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_MCS_BASE_CHANNEL_ID 1001u
#define RF_RDP_MCS_GLOBAL_CHANNEL_ID 1003u
#define RF_RDP_MCS_FIRST_DYNAMIC_CHANNEL_ID 1004u
#define RF_RDP_MCS_MAX_CHANNELS 31u

enum rf_rdp_mcs_domain_pdu_type {
	RF_RDP_MCS_PDU_ERECT_DOMAIN_REQUEST = 1,
	RF_RDP_MCS_PDU_ATTACH_USER_REQUEST = 10,
	RF_RDP_MCS_PDU_CHANNEL_JOIN_REQUEST = 14,
	RF_RDP_MCS_PDU_SEND_DATA_REQUEST = 25,
	RF_RDP_MCS_PDU_SEND_DATA_INDICATION = 26,
};

struct rf_rdp_mcs_domain_pdu {
	size_t payload_offset;
	size_t payload_length;
	uint16_t tpkt_length;
	uint16_t user_id;
	uint16_t channel_id;
	uint8_t type;
	uint8_t options;
};

struct rf_rdp_mcs_client_info {
	uint16_t desktop_width;
	uint16_t desktop_height;
	uint16_t channel_count;
	uint16_t drdynvc_channel_id;
};

bool rf_rdp_mcs_parse_connect_initial(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_mcs_client_info *info
);
bool rf_rdp_mcs_parse_domain_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_mcs_domain_pdu *pdu
);
size_t rf_rdp_mcs_write_connect_response(
	uint8_t *data,
	size_t capacity,
	uint32_t selected_protocol
);
size_t rf_rdp_mcs_write_connect_response_with_channels(
	uint8_t *data,
	size_t capacity,
	uint32_t selected_protocol,
	uint16_t channel_count
);
size_t rf_rdp_mcs_write_attach_user_confirm(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id
);
size_t rf_rdp_mcs_write_channel_join_confirm(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint16_t channel_id
);
size_t rf_rdp_mcs_write_send_data_indication(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint16_t channel_id,
	const uint8_t *payload,
	size_t payload_length
);

#endif
