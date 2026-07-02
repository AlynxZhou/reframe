#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-proto.h"

static void test_parse_connection_request(void)
{
	const uint8_t pdu[] = {
		0x03, 0x00, 0x00, 0x13,
		0x0e, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x08, 0x00,
		0x03, 0x00, 0x00, 0x00
	};
	struct rf_rdp_connection_request request = { 0 };
	uint16_t length = 0;

	assert(rf_rdp_read_tpkt_header(pdu, sizeof(pdu), &length));
	assert(length == sizeof(pdu));
	assert(rf_rdp_parse_connection_request(pdu, sizeof(pdu), &request));
	assert(request.has_negotiation);
	assert(request.requested_protocols == (RF_RDP_PROTOCOL_SSL | RF_RDP_PROTOCOL_HYBRID));
}

static void test_read_tpkt_header_only(void)
{
	const uint8_t header[] = { 0x03, 0x00, 0x00, 0x13 };
	struct rf_rdp_connection_request request = { 0 };
	uint16_t length = 0;

	assert(rf_rdp_read_tpkt_header(header, sizeof(header), &length));
	assert(length == 19);
	assert(!rf_rdp_parse_connection_request(header, sizeof(header), &request));
}

static void test_parse_connection_request_with_cookie(void)
{
	const char cookie[] = "Cookie: mstshash=reframe\r\n";
	const size_t cookie_length = sizeof(cookie) - 1;
	const size_t total_length = 4 + 7 + cookie_length + 8;
	uint8_t pdu[64] = {
		0x03, 0x00, 0x00, 0x00,
		0x00, 0xe0, 0x00, 0x00, 0x00, 0x00, 0x00
	};
	struct rf_rdp_connection_request request = { 0 };

	assert(total_length <= sizeof(pdu));
	pdu[2] = total_length >> 8;
	pdu[3] = total_length & 0xff;
	pdu[4] = 6 + cookie_length + 8;
	memcpy(pdu + 11, cookie, cookie_length);
	uint8_t *neg = pdu + 11 + cookie_length;
	neg[0] = 0x01;
	neg[1] = 0x00;
	neg[2] = 0x08;
	neg[3] = 0x00;
	neg[4] = RF_RDP_PROTOCOL_SSL | RF_RDP_PROTOCOL_HYBRID_EX;

	assert(rf_rdp_parse_connection_request(pdu, total_length, &request));
	assert(request.has_negotiation);
	assert(request.requested_protocols == (RF_RDP_PROTOCOL_SSL | RF_RDP_PROTOCOL_HYBRID_EX));
}

static void test_write_negotiation_response(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t expected[] = {
		0x03, 0x00, 0x00, 0x13,
		0x0e, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x02, 0x01, 0x08, 0x00,
		0x01, 0x00, 0x00, 0x00
	};

	size_t length = rf_rdp_write_negotiation_response(
		out, sizeof(out), RF_RDP_PROTOCOL_SSL
	);
	assert(length == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

static void test_write_negotiation_failure(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t expected[] = {
		0x03, 0x00, 0x00, 0x13,
		0x0e, 0xd0, 0x00, 0x00, 0x00, 0x00, 0x00,
			0x03, 0x00, 0x08, 0x00,
			0x01, 0x00, 0x00, 0x00
	};

	size_t length = rf_rdp_write_negotiation_failure(
		out, sizeof(out), RF_RDP_NEG_FAILURE_SSL_REQUIRED
	);
	assert(length == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

int main(void)
{
	test_parse_connection_request();
	test_read_tpkt_header_only();
	test_parse_connection_request_with_cookie();
	test_write_negotiation_response();
	test_write_negotiation_failure();
	return 0;
}
