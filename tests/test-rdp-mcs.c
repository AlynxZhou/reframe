#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-mcs.h"
#include "rf-rdp-proto.h"

static bool contains_bytes(
	const uint8_t *haystack,
	size_t haystack_length,
	const uint8_t *needle,
	size_t needle_length
)
{
	if (needle_length == 0)
		return true;
	if (haystack_length < needle_length)
		return false;

	for (size_t i = 0; i <= haystack_length - needle_length; ++i) {
		if (memcmp(haystack + i, needle, needle_length) == 0)
			return true;
	}
	return false;
}

static void test_write_connect_response(void)
{
	uint8_t out[1024] = { 0 };
	uint16_t tpkt_length = 0;
	const uint8_t sc_core[] = {
		0x01, 0x0c, 0x10, 0x00,
		0x04, 0x00, 0x08, 0x00,
		0x01, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	const uint8_t sc_net[] = {
		0x03, 0x0c, 0x08, 0x00,
		0xeb, 0x03, 0x00, 0x00
	};
	const uint8_t sc_security[] = {
		0x02, 0x0c, 0x0c, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};

	const size_t length = rf_rdp_mcs_write_connect_response(
		out, sizeof(out), RF_RDP_PROTOCOL_SSL
	);
	assert(length > 0);
	assert(rf_rdp_read_tpkt_header(out, length, &tpkt_length));
	assert(tpkt_length == length);
	assert(out[4] == 0x02);
	assert(out[5] == 0xf0);
	assert(out[6] == 0x80);
	assert(out[7] == 0x7f);
	assert(out[8] == 0x66);
	assert(contains_bytes(out, length, sc_core, sizeof(sc_core)));
	assert(contains_bytes(out, length, sc_net, sizeof(sc_net)));
	assert(contains_bytes(out, length, sc_security, sizeof(sc_security)));
}

static void test_parse_attach_user_request(void)
{
	const uint8_t pdu[] = {
		0x03, 0x00, 0x00, 0x08,
		0x02, 0xf0, 0x80,
		0x28
	};
	struct rf_rdp_mcs_domain_pdu parsed = { 0 };
	uint8_t out[16] = { 0 };
	const uint8_t expected[] = {
		0x03, 0x00, 0x00, 0x0b,
		0x02, 0xf0, 0x80,
		0x2e, 0x00, 0x00, 0x00
	};

	assert(rf_rdp_mcs_parse_domain_pdu(pdu, sizeof(pdu), &parsed));
	assert(parsed.type == RF_RDP_MCS_PDU_ATTACH_USER_REQUEST);
	assert(parsed.options == 0);
	assert(rf_rdp_mcs_write_attach_user_confirm(
		out, sizeof(out), RF_RDP_MCS_BASE_CHANNEL_ID
	) == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

static void test_parse_channel_join_request(void)
{
	const uint8_t pdu[] = {
		0x03, 0x00, 0x00, 0x0c,
		0x02, 0xf0, 0x80,
		0x38, 0x00, 0x00, 0x03, 0xeb
	};
	struct rf_rdp_mcs_domain_pdu parsed = { 0 };
	uint8_t out[16] = { 0 };
	const uint8_t expected[] = {
		0x03, 0x00, 0x00, 0x0f,
		0x02, 0xf0, 0x80,
		0x3e, 0x00, 0x00, 0x00, 0x03, 0xeb, 0x03, 0xeb
	};

	assert(rf_rdp_mcs_parse_domain_pdu(pdu, sizeof(pdu), &parsed));
	assert(parsed.type == RF_RDP_MCS_PDU_CHANNEL_JOIN_REQUEST);
	assert(parsed.options == 0);
	assert(parsed.user_id == RF_RDP_MCS_BASE_CHANNEL_ID);
	assert(parsed.channel_id == RF_RDP_MCS_GLOBAL_CHANNEL_ID);
	assert(rf_rdp_mcs_write_channel_join_confirm(
		out, sizeof(out), parsed.user_id, parsed.channel_id
	) == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

static void test_parse_client_connect_initial_info(void)
{
	const uint8_t pdu[] = {
		0x03, 0x00, 0x00, 0x50,
		0x02, 0xf0, 0x80,
		0x7f, 0x65, 0x82, 0x00,
		0x44,
		0x34, 0x12, 0x20, 0x00,
		0x01, 0xc0, 0x0c, 0x00,
		0x04, 0x00, 0x08, 0x00,
		0x00, 0x04, 0x00, 0x03,
		0x03, 0xc0, 0x2c, 0x00,
		0x03, 0x00, 0x00, 0x00,
		'c', 'l', 'i', 'p', 'r', 'd', 'r', '\0',
		0x80, 0x00, 0x00, 0xc0,
		'r', 'd', 'p', 'd', 'r', '\0', '\0', '\0',
		0x80, 0x80, 0x00, 0xc0,
		'd', 'r', 'd', 'y', 'n', 'v', 'c', '\0',
		0x80, 0x80, 0x00, 0xc0,
		0x02, 0xc0, 0x0c, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00
	};
	struct rf_rdp_mcs_client_info info = { 0 };

	assert(rf_rdp_mcs_parse_connect_initial(pdu, sizeof(pdu), &info));
	assert(info.desktop_width == 1024);
	assert(info.desktop_height == 768);
	assert(info.channel_count == 3);
	assert(info.cliprdr_channel_id == RF_RDP_MCS_FIRST_DYNAMIC_CHANNEL_ID);
	assert(info.drdynvc_channel_id == RF_RDP_MCS_FIRST_DYNAMIC_CHANNEL_ID + 2);
}

static void test_write_connect_response_with_channels(void)
{
	uint8_t out[1024] = { 0 };
	const uint8_t sc_net[] = {
		0x03, 0x0c, 0x10, 0x00,
		0xeb, 0x03, 0x03, 0x00,
		0xec, 0x03, 0xed, 0x03,
		0xee, 0x03, 0x00, 0x00
	};

	const size_t length = rf_rdp_mcs_write_connect_response_with_channels(
		out, sizeof(out), RF_RDP_PROTOCOL_SSL, 3
	);
	assert(length > 0);
	assert(contains_bytes(out, length, sc_net, sizeof(sc_net)));
}

int main(void)
{
	test_write_connect_response();
	test_parse_attach_user_request();
	test_parse_channel_join_request();
	test_parse_client_connect_initial_info();
	test_write_connect_response_with_channels();
	return 0;
}
