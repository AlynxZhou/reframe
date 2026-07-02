#include "rf-rdp-dvc.h"

#include <string.h>

static void write_u16_le(uint8_t *data, uint16_t value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = (value >> 24) & 0xff;
}

static uint16_t read_u16_le(const uint8_t *data)
{
	return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
	return (uint32_t)data[0] |
	       ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) |
	       ((uint32_t)data[3] << 24);
}

static uint8_t variable_uint_size_code(uint32_t value)
{
	if (value <= 0xff)
		return 0;
	if (value <= 0xffff)
		return 1;
	return 2;
}

static size_t variable_uint_size(uint8_t size_code)
{
	if (size_code == 0)
		return 1;
	if (size_code == 1)
		return 2;
	return 4;
}

static void write_variable_uint(uint8_t *data, uint8_t size_code, uint32_t value)
{
	if (size_code == 0) {
		data[0] = value;
	} else if (size_code == 1) {
		write_u16_le(data, value);
	} else {
		write_u32_le(data, value);
	}
}

static uint32_t read_variable_uint(const uint8_t *data, uint8_t size_code)
{
	if (size_code == 0)
		return data[0];
	if (size_code == 1)
		return read_u16_le(data);
	return read_u32_le(data);
}

size_t rf_rdp_dvc_write_capability_request(
	uint8_t *data,
	size_t capacity,
	uint16_t version
)
{
	if (data == NULL || capacity < 12)
		return 0;

	data[0] = RF_RDP_DVC_CAPABILITY_REQUEST_PDU << 4;
	data[1] = 0;
	write_u16_le(data + 2, version);
	memset(data + 4, 0, 8);
	return 12;
}

size_t rf_rdp_dvc_write_channel_pdu(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (data == NULL || (payload == NULL && payload_length > 0) ||
	    payload_length > RF_RDP_DVC_CHANNEL_CHUNK_LENGTH ||
	    capacity < 8 + payload_length)
		return 0;

	write_u32_le(data, payload_length);
	write_u32_le(
		data + 4,
		RF_RDP_DVC_CHANNEL_FLAG_FIRST | RF_RDP_DVC_CHANNEL_FLAG_LAST
	);
	if (payload_length > 0)
		memcpy(data + 8, payload, payload_length);
	return 8 + payload_length;
}

bool rf_rdp_dvc_parse_channel_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_dvc_channel_pdu *pdu
)
{
	if (data == NULL || pdu == NULL || length < 8)
		return false;

	const uint32_t total_length = read_u32_le(data);
	const uint32_t flags = read_u32_le(data + 4);
	const size_t payload_length = length - 8;

	if (total_length < payload_length ||
	    payload_length > RF_RDP_DVC_CHANNEL_CHUNK_LENGTH)
		return false;
	if ((flags & RF_RDP_DVC_CHANNEL_FLAG_FIRST) != 0 &&
	    total_length != payload_length &&
	    (flags & RF_RDP_DVC_CHANNEL_FLAG_LAST) != 0)
		return false;

	pdu->payload_offset = 8;
	pdu->payload_length = payload_length;
	pdu->total_length = total_length;
	pdu->flags = flags;
	return true;
}

size_t rf_rdp_dvc_write_create_request(
	uint8_t *data,
	size_t capacity,
	uint32_t channel_id,
	const char *channel_name
)
{
	if (data == NULL || channel_name == NULL)
		return 0;

	const uint8_t size_code = variable_uint_size_code(channel_id);
	const size_t id_size = variable_uint_size(size_code);
	const size_t name_length = strlen(channel_name) + 1;
	const size_t length = 1 + id_size + name_length;

	if (capacity < length)
		return 0;

	data[0] = (RF_RDP_DVC_CREATE_REQUEST_PDU << 4) | size_code;
	write_variable_uint(data + 1, size_code, channel_id);
	memcpy(data + 1 + id_size, channel_name, name_length);
	return length;
}

bool rf_rdp_dvc_parse_create_response(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_dvc_create_response *response
)
{
	if (data == NULL || response == NULL || length < 1)
		return false;

	const uint8_t command = data[0] >> 4;
	const uint8_t size_code = data[0] & 0x03;
	const size_t id_size = variable_uint_size(size_code);

	if (command != RF_RDP_DVC_CREATE_REQUEST_PDU ||
	    length < 1 + id_size + 4)
		return false;

	response->channel_id = read_variable_uint(data + 1, size_code);
	response->status = read_u32_le(data + 1 + id_size);
	return true;
}

bool rf_rdp_dvc_parse_capability_response(
	const uint8_t *data,
	size_t length,
	uint16_t *version
)
{
	if (data == NULL || version == NULL || length < 4)
		return false;
	if ((data[0] >> 4) != RF_RDP_DVC_CAPABILITY_REQUEST_PDU)
		return false;

	*version = read_u16_le(data + 2);
	return true;
}

bool rf_rdp_dvc_parse_data_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_dvc_data_pdu *pdu
)
{
	if (data == NULL || pdu == NULL || length < 1)
		return false;

	const uint8_t command = data[0] >> 4;
	const uint8_t size_code = data[0] & 0x03;
	const size_t id_size = variable_uint_size(size_code);

	if (command != RF_RDP_DVC_DATA_PDU || length < 1 + id_size)
		return false;

	pdu->channel_id = read_variable_uint(data + 1, size_code);
	pdu->payload_offset = 1 + id_size;
	pdu->payload_length = length - pdu->payload_offset;
	return true;
}

size_t rf_rdp_dvc_write_data(
	uint8_t *data,
	size_t capacity,
	uint32_t channel_id,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (data == NULL || (payload == NULL && payload_length > 0))
		return 0;

	const uint8_t size_code = variable_uint_size_code(channel_id);
	const size_t id_size = variable_uint_size(size_code);
	const size_t length = 1 + id_size + payload_length;

	if (capacity < length)
		return 0;

	data[0] = (RF_RDP_DVC_DATA_PDU << 4) | size_code;
	write_variable_uint(data + 1, size_code, channel_id);
	if (payload_length > 0)
		memcpy(data + 1 + id_size, payload, payload_length);
	return length;
}

size_t rf_rdp_dvc_write_data_first(
	uint8_t *data,
	size_t capacity,
	uint32_t channel_id,
	uint32_t total_length,
	const uint8_t *payload,
	size_t payload_length
)
{
	if (data == NULL || total_length == 0 ||
	    (payload == NULL && payload_length > 0))
		return 0;

	const uint8_t id_size_code = variable_uint_size_code(channel_id);
	const uint8_t length_size_code = variable_uint_size_code(total_length);
	const size_t id_size = variable_uint_size(id_size_code);
	const size_t total_size = variable_uint_size(length_size_code);
	const size_t length = 1 + id_size + total_size + payload_length;

	if (capacity < length)
		return 0;

	data[0] = (RF_RDP_DVC_DATA_FIRST_PDU << 4) |
		  id_size_code |
		  (length_size_code << 2);
	write_variable_uint(data + 1, id_size_code, channel_id);
	write_variable_uint(data + 1 + id_size, length_size_code, total_length);
	if (payload_length > 0)
		memcpy(data + 1 + id_size + total_size, payload, payload_length);
	return length;
}
