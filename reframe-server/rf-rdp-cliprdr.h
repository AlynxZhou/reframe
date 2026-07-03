#ifndef __RF_RDP_CLIPRDR_H__
#define __RF_RDP_CLIPRDR_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_CLIPRDR_CHANNEL_NAME "cliprdr"
#define RF_RDP_CLIPRDR_HEADER_SIZE 8u

#define RF_RDP_CLIPRDR_CB_MONITOR_READY 0x0001u
#define RF_RDP_CLIPRDR_CB_FORMAT_LIST 0x0002u
#define RF_RDP_CLIPRDR_CB_FORMAT_LIST_RESPONSE 0x0003u
#define RF_RDP_CLIPRDR_CB_FORMAT_DATA_REQUEST 0x0004u
#define RF_RDP_CLIPRDR_CB_FORMAT_DATA_RESPONSE 0x0005u
#define RF_RDP_CLIPRDR_CB_CLIP_CAPS 0x0007u

#define RF_RDP_CLIPRDR_CB_RESPONSE_OK 0x0001u
#define RF_RDP_CLIPRDR_CB_RESPONSE_FAIL 0x0002u

#define RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL 0x0001u
#define RF_RDP_CLIPRDR_CB_CAPSTYPE_GENERAL_LEN 12u
#define RF_RDP_CLIPRDR_CAPS_VERSION_1 1u
#define RF_RDP_CLIPRDR_CAPS_VERSION_2 2u

#define RF_RDP_CLIPRDR_CF_UNICODETEXT 13u

struct rf_rdp_cliprdr_pdu {
	size_t data_offset;
	uint32_t data_length;
	uint16_t msg_type;
	uint16_t msg_flags;
};

struct rf_rdp_cliprdr_format_list {
	bool unicode_text;
};

bool rf_rdp_cliprdr_parse_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_pdu *pdu
);
bool rf_rdp_cliprdr_parse_format_list(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_cliprdr_format_list *formats
);
bool rf_rdp_cliprdr_parse_format_data_request(
	const uint8_t *data,
	size_t length,
	uint32_t *format_id
);
char *rf_rdp_cliprdr_parse_format_data_response_text(
	const uint8_t *data,
	size_t length,
	uint16_t msg_flags
);
size_t rf_rdp_cliprdr_write_caps(uint8_t *data, size_t capacity);
size_t rf_rdp_cliprdr_write_monitor_ready(uint8_t *data, size_t capacity);
size_t rf_rdp_cliprdr_write_format_list(uint8_t *data, size_t capacity);
size_t rf_rdp_cliprdr_write_format_list_response(
	uint8_t *data,
	size_t capacity,
	bool ok
);
size_t rf_rdp_cliprdr_write_format_data_request(
	uint8_t *data,
	size_t capacity,
	uint32_t format_id
);
size_t rf_rdp_cliprdr_write_format_data_response_text(
	uint8_t *data,
	size_t capacity,
	const char *text
);
size_t rf_rdp_cliprdr_write_format_data_response_fail(
	uint8_t *data,
	size_t capacity
);

#endif
