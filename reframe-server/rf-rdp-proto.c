#include <string.h>

#include "rf-rdp-proto.h"

#define TPKT_VERSION 0x03
#define X224_CONNECTION_REQUEST 0xe0
#define X224_CONNECTION_CONFIRM 0xd0
#define TYPE_RDP_NEG_REQ 0x01
#define TYPE_RDP_NEG_RSP 0x02
#define TYPE_RDP_NEG_FAILURE 0x03
#define EXTENDED_CLIENT_DATA_SUPPORTED 0x01

static bool has_prefix(const uint8_t *data, size_t length, const char *prefix)
{
	const size_t prefix_length = strlen(prefix);

	return length >= prefix_length &&
	       memcmp(data, prefix, prefix_length) == 0;
}

static bool skip_token_or_cookie(
	const uint8_t *data,
	size_t length,
	size_t *offset
)
{
	*offset = 0;
	if (!has_prefix(data, length, "Cookie: mstshash=") &&
	    !has_prefix(data, length, "Cookie: msts=") &&
	    !has_prefix(data, length, "tsv:") &&
	    !has_prefix(data, length, "mth://"))
		return true;

	for (size_t i = 0; i + 1 < length; ++i) {
		if (data[i] == '\r' && data[i + 1] == '\n') {
			*offset = i + 2;
			return true;
		}
	}

	return false;
}

static uint16_t read_u16_be(const uint8_t *data)
{
	return ((uint16_t)data[0] << 8) | data[1];
}

static uint16_t read_u16_le(const uint8_t *data)
{
	return data[0] | ((uint16_t)data[1] << 8);
}

static uint32_t read_u32_le(const uint8_t *data)
{
	return data[0] | ((uint32_t)data[1] << 8) |
	       ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void write_u16_be(uint8_t *data, uint16_t value)
{
	data[0] = value >> 8;
	data[1] = value & 0xff;
}

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

bool rf_rdp_read_tpkt_header(
	const uint8_t *data,
	size_t length,
	uint16_t *tpkt_length
)
{
	if (data == NULL || tpkt_length == NULL || length < 4)
		return false;
	if (data[0] != TPKT_VERSION || data[1] != 0)
		return false;

	*tpkt_length = read_u16_be(data + 2);
	if (*tpkt_length < 7)
		return false;

	return true;
}

bool rf_rdp_parse_connection_request(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_connection_request *request
)
{
	uint16_t tpkt_length = 0;
	if (request == NULL || !rf_rdp_read_tpkt_header(data, length, &tpkt_length))
		return false;
	if (tpkt_length > length)
		return false;
	memset(request, 0, sizeof(*request));

	const uint8_t *tpdu = data + 4;
	const size_t tpdu_length = tpkt_length - 4;
	if (tpdu_length < 7)
		return false;
	const uint8_t li = tpdu[0];
	if ((size_t)li + 1 != tpdu_length)
		return false;
	if (tpdu[1] != X224_CONNECTION_REQUEST)
		return false;

	const size_t fixed_length = 7;
	if (tpdu_length <= fixed_length)
		return true;
	const uint8_t *variable = tpdu + fixed_length;
	const size_t variable_length = tpdu_length - fixed_length;
	size_t neg_offset = 0;
	if (!skip_token_or_cookie(variable, variable_length, &neg_offset))
		return false;
	if (neg_offset == variable_length)
		return true;
	if (neg_offset > variable_length || variable_length - neg_offset < 8)
		return false;

	const uint8_t *neg = variable + neg_offset;
	if (neg[0] != TYPE_RDP_NEG_REQ)
		return false;
	if (read_u16_le(neg + 2) != 8)
		return false;

	request->has_negotiation = true;
	request->requested_protocols = read_u32_le(neg + 4);
	return true;
}

static size_t write_connection_confirm(
	uint8_t *data,
	size_t capacity,
	uint8_t neg_type,
	uint8_t flags,
	uint32_t value
)
{
	const size_t length = 19;
	if (data == NULL || capacity < length)
		return 0;

	data[0] = TPKT_VERSION;
	data[1] = 0;
	write_u16_be(data + 2, length);
	data[4] = 0x0e;
	data[5] = X224_CONNECTION_CONFIRM;
	data[6] = 0;
	data[7] = 0;
	data[8] = 0;
	data[9] = 0;
	data[10] = 0;
	data[11] = neg_type;
	data[12] = flags;
	write_u16_le(data + 13, 8);
	write_u32_le(data + 15, value);
	return length;
}

size_t rf_rdp_write_negotiation_response(
	uint8_t *data,
	size_t capacity,
	uint32_t selected_protocol
)
{
	return write_connection_confirm(
		data,
		capacity,
		TYPE_RDP_NEG_RSP,
		EXTENDED_CLIENT_DATA_SUPPORTED,
		selected_protocol
	);
}

size_t rf_rdp_write_negotiation_failure(
	uint8_t *data,
	size_t capacity,
	uint32_t failure_code
)
{
	return write_connection_confirm(
		data,
		capacity,
		TYPE_RDP_NEG_FAILURE,
		0,
		failure_code
	);
}
