#include <string.h>

#include "rf-rdp-mcs.h"
#include "rf-rdp-proto.h"

#define TPKT_VERSION 0x03
#define X224_DATA_LENGTH 0x02
#define X224_DATA_TPDU 0xf0
#define X224_EOT 0x80

#define MCS_TYPE_CONNECT_RESPONSE 102
#define MCS_PDU_ATTACH_USER_CONFIRM 11
#define MCS_PDU_CHANNEL_JOIN_CONFIRM 15

#define SC_CORE 0x0c01
#define SC_SECURITY 0x0c02
#define SC_NET 0x0c03

#define CS_CORE 0xc001
#define CS_NET 0xc003

#define RDP_VERSION_10_6 0x00080004u

struct writer {
	uint8_t *data;
	size_t capacity;
	size_t length;
	bool failed;
};

static bool writer_has(const struct writer *writer, size_t length)
{
	return !writer->failed && writer->length <= writer->capacity &&
	       writer->capacity - writer->length >= length;
}

static void write_u8(struct writer *writer, uint8_t value)
{
	if (!writer_has(writer, 1)) {
		writer->failed = true;
		return;
	}
	writer->data[writer->length++] = value;
}

static void write_u16_be(struct writer *writer, uint16_t value)
{
	write_u8(writer, value >> 8);
	write_u8(writer, value & 0xff);
}

static void write_u16_le(struct writer *writer, uint16_t value)
{
	write_u8(writer, value & 0xff);
	write_u8(writer, value >> 8);
}

static void write_u32_le(struct writer *writer, uint32_t value)
{
	write_u8(writer, value & 0xff);
	write_u8(writer, (value >> 8) & 0xff);
	write_u8(writer, (value >> 16) & 0xff);
	write_u8(writer, (value >> 24) & 0xff);
}

static void write_bytes(
	struct writer *writer,
	const uint8_t *data,
	size_t length
)
{
	if (!writer_has(writer, length)) {
		writer->failed = true;
		return;
	}
	if (length > 0)
		memcpy(writer->data + writer->length, data, length);
	writer->length += length;
}

static uint16_t read_u16_be(const uint8_t *data)
{
	return ((uint16_t)data[0] << 8) | data[1];
}

static uint16_t read_u16_le(const uint8_t *data)
{
	return ((uint16_t)data[1] << 8) | data[0];
}

static uint32_t read_u32_le(const uint8_t *data)
{
	return ((uint32_t)data[3] << 24) | ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[1] << 8) | data[0];
}

static bool read_per_length(
	const uint8_t *data,
	size_t length,
	size_t *offset,
	size_t *value
)
{
	if (*offset >= length)
		return false;

	const uint8_t first = data[(*offset)++];
	if (first & 0x80) {
		if (*offset >= length)
			return false;
		*value = ((size_t)(first & 0x7f) << 8) | data[(*offset)++];
	} else {
		*value = first;
	}
	return true;
}

static void write_tpkt_x224_data(struct writer *writer, uint16_t length)
{
	write_u8(writer, TPKT_VERSION);
	write_u8(writer, 0);
	write_u16_be(writer, length);
	write_u8(writer, X224_DATA_LENGTH);
	write_u8(writer, X224_DATA_TPDU);
	write_u8(writer, X224_EOT);
}

static void patch_tpkt_length(uint8_t *data, uint16_t length)
{
	data[2] = length >> 8;
	data[3] = length & 0xff;
}

static void ber_write_length(struct writer *writer, size_t length)
{
	if (length > 0xffff) {
		writer->failed = true;
		return;
	}
	if (length > 0xff) {
		write_u8(writer, 0x82);
		write_u16_be(writer, length);
	} else if (length > 0x7f) {
		write_u8(writer, 0x81);
		write_u8(writer, length);
	} else {
		write_u8(writer, length);
	}
}

static void ber_write_application_tag(
	struct writer *writer,
	uint8_t tag,
	size_t length
)
{
	if (tag > 30) {
		write_u8(writer, 0x7f);
		write_u8(writer, tag);
		ber_write_length(writer, length);
		return;
	}

	write_u8(writer, 0x60 | (tag & 0x1f));
	ber_write_length(writer, length);
}

static void ber_write_integer(struct writer *writer, uint32_t value)
{
	write_u8(writer, 0x02);
	if (value < 0x80) {
		write_u8(writer, 1);
		write_u8(writer, value);
	} else if (value < 0x8000) {
		write_u8(writer, 2);
		write_u16_be(writer, value);
	} else if (value < 0x800000) {
		write_u8(writer, 3);
		write_u8(writer, value >> 16);
		write_u16_be(writer, value);
	} else {
		write_u8(writer, 4);
		write_u16_be(writer, value >> 16);
		write_u16_be(writer, value);
	}
}

static void ber_write_enumerated(struct writer *writer, uint8_t value)
{
	write_u8(writer, 0x0a);
	write_u8(writer, 1);
	write_u8(writer, value);
}

static void ber_write_octet_string(
	struct writer *writer,
	const uint8_t *data,
	size_t length
)
{
	write_u8(writer, 0x04);
	ber_write_length(writer, length);
	write_bytes(writer, data, length);
}

static bool mcs_write_domain_parameters(struct writer *writer)
{
	uint8_t body[64] = { 0 };
	struct writer params = { body, sizeof(body), 0, false };

	ber_write_integer(&params, 34);
	ber_write_integer(&params, 3);
	ber_write_integer(&params, 0);
	ber_write_integer(&params, 1);
	ber_write_integer(&params, 0);
	ber_write_integer(&params, 1);
	ber_write_integer(&params, 65535);
	ber_write_integer(&params, 2);
	if (params.failed)
		return false;

	write_u8(writer, 0x30);
	ber_write_length(writer, params.length);
	write_bytes(writer, body, params.length);
	return !writer->failed;
}

static void per_write_length(struct writer *writer, uint16_t length)
{
	if (length > 0x7f) {
		write_u16_be(writer, length | 0x8000);
		return;
	}
	write_u8(writer, length);
}

static void per_write_integer(struct writer *writer, uint32_t value)
{
	if (value <= 0xff) {
		per_write_length(writer, 1);
		write_u8(writer, value);
	} else if (value <= 0xffff) {
		per_write_length(writer, 2);
		write_u16_be(writer, value);
	} else {
		per_write_length(writer, 4);
		write_u16_be(writer, value >> 16);
		write_u16_be(writer, value);
	}
}

static void per_write_integer16(
	struct writer *writer,
	uint16_t value,
	uint16_t minimum
)
{
	if (value < minimum) {
		writer->failed = true;
		return;
	}
	write_u16_be(writer, value - minimum);
}

static void per_write_octet_string(
	struct writer *writer,
	const uint8_t *data,
	uint16_t length,
	uint16_t minimum
)
{
	if (length < minimum) {
		writer->failed = true;
		return;
	}
	per_write_length(writer, length - minimum);
	write_bytes(writer, data, length);
}

static void gcc_write_user_data_header(
	struct writer *writer,
	uint16_t type,
	uint16_t length
)
{
	write_u16_le(writer, type);
	write_u16_le(writer, length);
}

static bool gcc_write_server_data_blocks(
	struct writer *writer,
	uint32_t selected_protocol,
	uint16_t channel_count
)
{
	const uint16_t sc_net_length =
		8 + channel_count * 2 + (channel_count % 2 == 1 ? 2 : 0);

	if (channel_count > RF_RDP_MCS_MAX_CHANNELS)
		return false;

	gcc_write_user_data_header(writer, SC_CORE, 16);
	write_u32_le(writer, RDP_VERSION_10_6);
	write_u32_le(writer, selected_protocol);
	write_u32_le(writer, 0);

	gcc_write_user_data_header(writer, SC_NET, sc_net_length);
	write_u16_le(writer, RF_RDP_MCS_GLOBAL_CHANNEL_ID);
	write_u16_le(writer, channel_count);
	for (uint16_t i = 0; i < channel_count; ++i)
		write_u16_le(writer, RF_RDP_MCS_FIRST_DYNAMIC_CHANNEL_ID + i);
	if (channel_count % 2 == 1)
		write_u16_le(writer, 0);

	gcc_write_user_data_header(writer, SC_SECURITY, 12);
	write_u32_le(writer, 0);
	write_u32_le(writer, 0);
	return !writer->failed;
}

static bool gcc_write_conference_create_response(
	struct writer *writer,
	const uint8_t *server_data,
	size_t server_data_length
)
{
	const uint8_t t124_02_98_oid[] = { 0x05, 0x00, 0x14, 0x7c, 0x00, 0x01 };
	const uint8_t h221_sc_key[] = { 'M', 'c', 'D', 'n' };

	if (server_data_length > 0xffff)
		return false;

	write_u8(writer, 0);
	write_bytes(writer, t124_02_98_oid, sizeof(t124_02_98_oid));
	per_write_length(writer, 0x2a);
	write_u8(writer, 0x14);
	per_write_integer16(writer, 0x79f3, RF_RDP_MCS_BASE_CHANNEL_ID);
	per_write_integer(writer, 1);
	write_u8(writer, 0);
	write_u8(writer, 1);
	write_u8(writer, 0xc0);
	per_write_octet_string(writer, h221_sc_key, sizeof(h221_sc_key), 4);
	per_write_octet_string(
		writer,
		server_data,
		server_data_length,
		0
	);
	return !writer->failed;
}

static bool mcs_write_connect_response_body(
	struct writer *writer,
	const uint8_t *gcc_data,
	size_t gcc_data_length
)
{
	ber_write_enumerated(writer, 0);
	ber_write_integer(writer, 0);
	if (!mcs_write_domain_parameters(writer))
		return false;
	ber_write_octet_string(writer, gcc_data, gcc_data_length);
	return !writer->failed;
}

bool rf_rdp_mcs_parse_connect_initial(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_mcs_client_info *info
)
{
	uint16_t tpkt_length = 0;

	if (data == NULL || info == NULL ||
	    !rf_rdp_read_tpkt_header(data, length, &tpkt_length))
		return false;
	if (tpkt_length > length)
		return false;

	memset(info, 0, sizeof(*info));

	for (size_t offset = 0; offset + 4 <= tpkt_length; ++offset) {
		const uint16_t type = read_u16_le(data + offset);
		const uint16_t block_length = read_u16_le(data + offset + 2);

		if (block_length < 4 || offset + block_length > tpkt_length)
			continue;
		if (type == CS_CORE && block_length >= 12) {
			info->desktop_width = read_u16_le(data + offset + 8);
			info->desktop_height = read_u16_le(data + offset + 10);
			offset += block_length - 1;
			} else if (type == CS_NET && block_length >= 8) {
				const uint32_t channel_count =
					read_u32_le(data + offset + 4);

				if (channel_count > RF_RDP_MCS_MAX_CHANNELS)
					return false;
				if (block_length >= 8 + channel_count * 12) {
						for (uint32_t i = 0; i < channel_count; ++i) {
							const uint8_t *name =
								data + offset + 8 + i * 12;

							if (memcmp(name, "cliprdr", 7) == 0 &&
							    name[7] == '\0') {
								info->cliprdr_channel_id =
									RF_RDP_MCS_FIRST_DYNAMIC_CHANNEL_ID + i;
							}
							if (memcmp(name, "drdynvc", 7) == 0 &&
							    name[7] == '\0') {
								info->drdynvc_channel_id =
								RF_RDP_MCS_FIRST_DYNAMIC_CHANNEL_ID + i;
						}
					}
				}
				info->channel_count = channel_count;
				offset += block_length - 1;
			}
	}

	return info->desktop_width > 0 && info->desktop_height > 0;
}

bool rf_rdp_mcs_parse_domain_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_mcs_domain_pdu *pdu
)
{
	uint16_t tpkt_length = 0;

	if (data == NULL || pdu == NULL ||
	    !rf_rdp_read_tpkt_header(data, length, &tpkt_length))
		return false;
	if (tpkt_length > length || tpkt_length < 8)
		return false;
	if (data[4] != X224_DATA_LENGTH || data[5] != X224_DATA_TPDU ||
	    data[6] != X224_EOT)
		return false;

	memset(pdu, 0, sizeof(*pdu));
	pdu->tpkt_length = tpkt_length;
	pdu->type = data[7] >> 2;
	pdu->options = data[7] & 0x03;

	if (pdu->type == RF_RDP_MCS_PDU_CHANNEL_JOIN_REQUEST) {
		if (tpkt_length < 12)
			return false;
		pdu->user_id = read_u16_be(data + 8) + RF_RDP_MCS_BASE_CHANNEL_ID;
		pdu->channel_id = read_u16_be(data + 10);
	} else if (pdu->type == RF_RDP_MCS_PDU_SEND_DATA_REQUEST ||
		   pdu->type == RF_RDP_MCS_PDU_SEND_DATA_INDICATION) {
		size_t payload_offset = 13;
		size_t payload_length = 0;

		if (tpkt_length < 14)
			return false;
		pdu->user_id = read_u16_be(data + 8) + RF_RDP_MCS_BASE_CHANNEL_ID;
		pdu->channel_id = read_u16_be(data + 10);
		if (!read_per_length(
			    data,
			    tpkt_length,
			    &payload_offset,
			    &payload_length
		    ))
			return false;
		if (payload_offset + payload_length > tpkt_length)
			return false;
		pdu->payload_offset = payload_offset;
		pdu->payload_length = payload_length;
	}

	return true;
}

size_t rf_rdp_mcs_write_connect_response(
	uint8_t *data,
	size_t capacity,
	uint32_t selected_protocol
)
{
	return rf_rdp_mcs_write_connect_response_with_channels(
		data,
		capacity,
		selected_protocol,
		0
	);
}

size_t rf_rdp_mcs_write_connect_response_with_channels(
	uint8_t *data,
	size_t capacity,
	uint32_t selected_protocol,
	uint16_t channel_count
)
{
	uint8_t server_data[128] = { 0 };
	uint8_t gcc_data[256] = { 0 };
	uint8_t mcs_body[512] = { 0 };
	struct writer server = { server_data, sizeof(server_data), 0, false };
	struct writer gcc = { gcc_data, sizeof(gcc_data), 0, false };
	struct writer body = { mcs_body, sizeof(mcs_body), 0, false };
	struct writer out = { data, capacity, 0, false };

	if (data == NULL)
		return 0;
	if (!gcc_write_server_data_blocks(
		    &server,
		    selected_protocol,
		    channel_count
	    ))
		return 0;
	if (!gcc_write_conference_create_response(
		    &gcc,
		    server_data,
		    server.length
	    ))
		return 0;
	if (!mcs_write_connect_response_body(&body, gcc_data, gcc.length))
		return 0;

	write_tpkt_x224_data(&out, 0);
	ber_write_application_tag(&out, MCS_TYPE_CONNECT_RESPONSE, body.length);
	write_bytes(&out, mcs_body, body.length);
	if (out.failed || out.length > 0xffff)
		return 0;
	patch_tpkt_length(data, out.length);
	return out.length;
}

size_t rf_rdp_mcs_write_attach_user_confirm(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id
)
{
	const uint16_t length = 11;
	struct writer writer = { data, capacity, 0, false };

	if (data == NULL || user_id < RF_RDP_MCS_BASE_CHANNEL_ID)
		return 0;

	write_tpkt_x224_data(&writer, length);
	write_u8(&writer, (MCS_PDU_ATTACH_USER_CONFIRM << 2) | 2);
	write_u8(&writer, 0);
	write_u16_be(&writer, user_id - RF_RDP_MCS_BASE_CHANNEL_ID);
	if (writer.failed)
		return 0;
	return writer.length;
}

size_t rf_rdp_mcs_write_send_data_indication(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint16_t channel_id,
	const uint8_t *payload,
	size_t payload_length
)
{
	struct writer writer = { data, capacity, 0, false };
	const size_t length = 15 + payload_length;

	if (data == NULL || user_id < RF_RDP_MCS_BASE_CHANNEL_ID ||
	    payload_length > 0x7fff || length > 0xffff ||
	    (payload == NULL && payload_length > 0))
		return 0;

	write_tpkt_x224_data(&writer, length);
	write_u8(&writer, RF_RDP_MCS_PDU_SEND_DATA_INDICATION << 2);
	write_u16_be(&writer, user_id - RF_RDP_MCS_BASE_CHANNEL_ID);
	write_u16_be(&writer, channel_id);
	write_u8(&writer, 0x70);
	write_u16_be(&writer, payload_length | 0x8000);
	write_bytes(&writer, payload, payload_length);
	if (writer.failed)
		return 0;
	return writer.length;
}

size_t rf_rdp_mcs_write_channel_join_confirm(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint16_t channel_id
)
{
	const uint16_t length = 15;
	struct writer writer = { data, capacity, 0, false };

	if (data == NULL || user_id < RF_RDP_MCS_BASE_CHANNEL_ID)
		return 0;

	write_tpkt_x224_data(&writer, length);
	write_u8(&writer, (MCS_PDU_CHANNEL_JOIN_CONFIRM << 2) | 2);
	write_u8(&writer, 0);
	write_u16_be(&writer, user_id - RF_RDP_MCS_BASE_CHANNEL_ID);
	write_u16_be(&writer, channel_id);
	write_u16_be(&writer, channel_id);
	if (writer.failed)
		return 0;
	return writer.length;
}
