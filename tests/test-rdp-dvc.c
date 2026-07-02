#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-dvc.h"

static void test_write_capability_request(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t expected[] = {
		0x50, 0x00,
		0x03, 0x00,
		0x00, 0x00,
		0x00, 0x00,
		0x00, 0x00,
		0x00, 0x00
	};

	const size_t length = rf_rdp_dvc_write_capability_request(
		out, sizeof(out), RF_RDP_DVC_VERSION_3
	);
	assert(length == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

static void test_write_rdpgfx_create_request(void)
{
	uint8_t out[128] = { 0 };
	const char *name = RF_RDP_DVC_RDPGFX_CHANNEL_NAME;
	const size_t name_length = strlen(name) + 1;

	const size_t length = rf_rdp_dvc_write_create_request(
		out, sizeof(out), 1, name
	);
	assert(length == 2 + name_length);
	assert(out[0] == 0x10);
	assert(out[1] == 0x01);
	assert(memcmp(out + 2, name, name_length) == 0);
}

static void test_write_create_request_uses_variable_channel_id(void)
{
	uint8_t out[128] = { 0 };
	const char name[] = "example";
	const size_t length = rf_rdp_dvc_write_create_request(
		out, sizeof(out), 0x1234, name
	);

	assert(length == 3 + sizeof(name));
	assert(out[0] == 0x11);
	assert(out[1] == 0x34);
	assert(out[2] == 0x12);
	assert(memcmp(out + 3, name, sizeof(name)) == 0);
}

static void test_parse_create_response(void)
{
	const uint8_t ok[] = {
		0x10, 0x01,
		0x00, 0x00, 0x00, 0x00
	};
	const uint8_t failed[] = {
		0x11, 0x34, 0x12,
		0x7e, 0x00, 0x00, 0xc0
	};
	struct rf_rdp_dvc_create_response response = { 0 };

	assert(rf_rdp_dvc_parse_create_response(ok, sizeof(ok), &response));
	assert(response.channel_id == 1);
	assert(response.status == 0);

	assert(rf_rdp_dvc_parse_create_response(
		failed, sizeof(failed), &response
	));
	assert(response.channel_id == 0x1234);
	assert(response.status == 0xc000007e);
}

static void test_parse_capability_response(void)
{
	const uint8_t pdu[] = {
		0x5c, 0x00,
		0x03, 0x00
	};
	uint16_t version = 0;

	assert(rf_rdp_dvc_parse_capability_response(
		pdu, sizeof(pdu), &version
	));
	assert(version == RF_RDP_DVC_VERSION_3);
}

static void test_parse_data_pdu(void)
{
	const uint8_t pdu[] = {
		0x31, 0x34, 0x12,
		0xaa, 0xbb, 0xcc
	};
	struct rf_rdp_dvc_data_pdu data = { 0 };

	assert(rf_rdp_dvc_parse_data_pdu(pdu, sizeof(pdu), &data));
	assert(data.channel_id == 0x1234);
	assert(data.payload_offset == 3);
	assert(data.payload_length == 3);
}

static void test_static_channel_wrapper(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t payload[] = { 0x50, 0x00, 0x03, 0x00 };
	struct rf_rdp_dvc_channel_pdu channel = { 0 };

	const size_t length = rf_rdp_dvc_write_channel_pdu(
		out, sizeof(out), payload, sizeof(payload)
	);
	assert(length == 8 + sizeof(payload));
	assert(out[0] == sizeof(payload));
	assert(out[1] == 0x00);
	assert(out[2] == 0x00);
	assert(out[3] == 0x00);
	assert(out[4] == RF_RDP_DVC_CHANNEL_FLAG_FIRST + RF_RDP_DVC_CHANNEL_FLAG_LAST);
	assert(out[5] == 0x00);
	assert(out[6] == 0x00);
	assert(out[7] == 0x00);
	assert(memcmp(out + 8, payload, sizeof(payload)) == 0);

	assert(rf_rdp_dvc_parse_channel_pdu(out, length, &channel));
	assert(channel.payload_offset == 8);
	assert(channel.payload_length == sizeof(payload));
	assert(channel.total_length == sizeof(payload));
	assert(channel.flags == (
		RF_RDP_DVC_CHANNEL_FLAG_FIRST |
		RF_RDP_DVC_CHANNEL_FLAG_LAST
	));
}

static void test_write_data_pdu(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t payload[] = { 0xaa, 0xbb, 0xcc };
	const uint8_t expected[] = {
		0x31, 0x34, 0x12,
		0xaa, 0xbb, 0xcc
	};

	const size_t length = rf_rdp_dvc_write_data(
		out, sizeof(out), 0x1234, payload, sizeof(payload)
	);
	assert(length == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

static void test_write_data_first_pdu(void)
{
	uint8_t out[32] = { 0 };
	const uint8_t payload[] = { 0xaa, 0xbb };
	const uint8_t expected[] = {
		0x29, 0x34, 0x12,
		0x00, 0x20, 0x01, 0x00,
		0xaa, 0xbb
	};

	const size_t length = rf_rdp_dvc_write_data_first(
		out,
		sizeof(out),
		0x1234,
		0x00012000,
		payload,
		sizeof(payload)
	);
	assert(length == sizeof(expected));
	assert(memcmp(out, expected, sizeof(expected)) == 0);
}

int main(void)
{
	test_write_capability_request();
	test_write_rdpgfx_create_request();
	test_write_create_request_uses_variable_channel_id();
	test_parse_create_response();
	test_parse_capability_response();
	test_parse_data_pdu();
	test_static_channel_wrapper();
	test_write_data_pdu();
	test_write_data_first_pdu();
	return 0;
}
