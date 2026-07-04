#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "rf-rdp-rdpsnd.h"

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

static struct rf_rdp_rdpsnd_audio_format pcm_format(
	uint32_t sample_rate,
	uint16_t channels
)
{
	return (struct rf_rdp_rdpsnd_audio_format){
		.tag = RF_RDP_RDPSND_WAVE_FORMAT_PCM,
		.channels = channels,
		.samples_per_sec = sample_rate,
		.avg_bytes_per_sec = sample_rate * channels * 2,
		.block_align = channels * 2,
		.bits_per_sample = 16
	};
}

static void test_server_formats_pcm_48k_stereo(void)
{
	uint8_t out[256] = { 0 };
	const struct rf_rdp_rdpsnd_audio_format format = pcm_format(48000, 2);
	const size_t length = rf_rdp_rdpsnd_write_server_formats(
		out,
		sizeof(out),
		&format,
		1,
		7
	);

	assert(length == 42);
	assert(out[0] == RF_RDP_RDPSND_SNDC_FORMATS);
	assert(out[1] == 0);
	assert(read_u16_le(out + 2) == 38);
	assert(read_u32_le(out + 4) == 0);
	assert(read_u32_le(out + 8) == 0);
	assert(read_u32_le(out + 12) == 0);
	assert(read_u16_le(out + 16) == 0);
	assert(read_u16_le(out + 18) == 1);
	assert(out[20] == 7);
	assert(read_u16_le(out + 21) == RF_RDP_RDPSND_CHANNEL_VERSION_WIN_MAX);
	assert(out[23] == 0);
	assert(read_u16_le(out + 24) == RF_RDP_RDPSND_WAVE_FORMAT_PCM);
	assert(read_u16_le(out + 26) == 2);
	assert(read_u32_le(out + 28) == 48000);
	assert(read_u32_le(out + 32) == 192000);
	assert(read_u16_le(out + 36) == 4);
	assert(read_u16_le(out + 38) == 16);
	assert(read_u16_le(out + 40) == 0);
}

static void test_parse_client_formats(void)
{
	uint8_t out[256] = { 0 };
	const struct rf_rdp_rdpsnd_audio_format formats[] = {
		pcm_format(48000, 2),
		pcm_format(44100, 2)
	};
	struct rf_rdp_rdpsnd_client_formats parsed = { 0 };
	const size_t length = rf_rdp_rdpsnd_write_server_formats(
		out,
		sizeof(out),
		formats,
		2,
		3
	);

	assert(rf_rdp_rdpsnd_parse_client_formats(out, length, &parsed));
	assert(parsed.format_count == 2);
	assert(parsed.last_block_confirmed == 3);
	assert(parsed.version == RF_RDP_RDPSND_CHANNEL_VERSION_WIN_MAX);
	assert(parsed.formats[0].samples_per_sec == 48000);
	assert(parsed.formats[1].samples_per_sec == 44100);
}

static void test_choose_client_pcm_format(void)
{
	uint8_t out[256] = { 0 };
	struct rf_rdp_rdpsnd_client_formats formats = { 0 };
	const struct rf_rdp_rdpsnd_audio_format server_format = pcm_format(48000, 2);
	const size_t length = rf_rdp_rdpsnd_write_server_formats(
		out,
		sizeof(out),
		&server_format,
		1,
		0
	);

	assert(rf_rdp_rdpsnd_parse_client_formats(out, length, &formats));
	assert(formats.format_count == 1);
	assert(rf_rdp_rdpsnd_choose_pcm_format(&formats, 48000, 2) == 0);
	assert(rf_rdp_rdpsnd_choose_pcm_format(&formats, 44100, 2) == 0);
	assert(rf_rdp_rdpsnd_choose_pcm_format(&formats, 48000, 1) == 0);
}

static void test_choose_falls_back_to_44100(void)
{
	const struct rf_rdp_rdpsnd_client_formats formats = {
		.formats = {
			{
				.tag = RF_RDP_RDPSND_WAVE_FORMAT_PCM,
				.channels = 2,
				.samples_per_sec = 44100,
				.avg_bytes_per_sec = 176400,
				.block_align = 4,
				.bits_per_sample = 16
			}
		},
		.format_count = 1
	};

	assert(rf_rdp_rdpsnd_choose_pcm_format(&formats, 48000, 2) == 0);
}

static void test_quality_mode_packet(void)
{
	uint8_t out[16] = { 0 };
	const size_t length = rf_rdp_rdpsnd_write_quality_mode(out, sizeof(out));

	assert(length == 8);
	assert(out[0] == RF_RDP_RDPSND_SNDC_QUALITYMODE);
	assert(read_u16_le(out + 2) == 4);
	assert(read_u16_le(out + 4) == RF_RDP_RDPSND_QUALITY_MODE_DYNAMIC);
	assert(read_u16_le(out + 6) == 0);
}

static void test_training_packet_and_parse_confirm(void)
{
	uint8_t out[16] = { 0 };
	uint16_t timestamp = 0;
	uint16_t pack_size = 0;
	const uint8_t payload[] = { 1, 2, 3, 4 };
	const size_t length = rf_rdp_rdpsnd_write_training(
		out,
		sizeof(out),
		123,
		sizeof(payload),
		payload,
		sizeof(payload)
	);

	assert(length == 12);
	assert(out[0] == RF_RDP_RDPSND_SNDC_TRAINING);
	assert(read_u16_le(out + 2) == 8);
	assert(read_u16_le(out + 4) == 123);
	assert(read_u16_le(out + 6) == sizeof(payload));
	assert(memcmp(out + 8, payload, sizeof(payload)) == 0);
	assert(rf_rdp_rdpsnd_parse_training_confirm(
		out,
		length,
		&timestamp,
		&pack_size
	));
	assert(timestamp == 123);
	assert(pack_size == sizeof(payload));
}

static void test_wave_info_and_data_layout(void)
{
	uint8_t info[32] = { 0 };
	uint8_t rest[32] = { 0 };
	const uint8_t pcm[12] = {
		1, 0, 2, 0,
		3, 0, 4, 0,
		5, 0, 6, 0
	};
	const size_t info_length = rf_rdp_rdpsnd_write_wave_info(
		info,
		sizeof(info),
		7,
		0,
		1234,
		pcm,
		sizeof(pcm)
	);
	const size_t rest_length = rf_rdp_rdpsnd_write_wave_data(
		rest,
		sizeof(rest),
		pcm,
		sizeof(pcm)
	);

	assert(info_length == RF_RDP_RDPSND_WAVE_INFO_LENGTH);
	assert(info[0] == RF_RDP_RDPSND_SNDC_WAVE);
	assert(read_u16_le(info + 2) == 12);
	assert(read_u16_le(info + 4) == 1234);
	assert(read_u16_le(info + 6) == 0);
	assert(info[8] == 7);
	assert(memcmp(info + 12, pcm, 4) == 0);
	assert(rest_length == sizeof(pcm) - 4);
	assert(memcmp(rest, pcm + 4, sizeof(pcm) - 4) == 0);
}

static void test_wave_packet_layout(void)
{
	uint8_t out[256] = { 0 };
	const uint8_t pcm[8] = { 1, 0, 2, 0, 3, 0, 4, 0 };
	const size_t length = rf_rdp_rdpsnd_write_wave(
		out,
		sizeof(out),
		7,
		0,
		1234,
		pcm,
		sizeof(pcm)
	);

	assert(length == RF_RDP_RDPSND_WAVE_INFO_LENGTH + sizeof(pcm) - 4);
	assert(out[0] == RF_RDP_RDPSND_SNDC_WAVE);
	assert(read_u16_le(out + 2) == 12);
	assert(read_u16_le(out + 4) == 1234);
	assert(read_u16_le(out + 6) == 0);
	assert(out[8] == 7);
	assert(memcmp(out + 12, pcm, 4) == 0);
	assert(memcmp(out + 16, pcm + 4, sizeof(pcm) - 4) == 0);
}

static void test_parse_wave_confirm(void)
{
	const uint8_t pdu[8] = {
		RF_RDP_RDPSND_SNDC_WAVECONFIRM, 0, 4, 0,
		0x34, 0x12, 9, 0
	};
	uint16_t timestamp = 0;
	uint8_t block_no = 0;

	assert(rf_rdp_rdpsnd_parse_wave_confirm(
		pdu,
		sizeof(pdu),
		&timestamp,
		&block_no
	));
	assert(timestamp == 0x1234);
	assert(block_no == 9);
}

int main(void)
{
	test_server_formats_pcm_48k_stereo();
	test_parse_client_formats();
	test_choose_client_pcm_format();
	test_choose_falls_back_to_44100();
	test_quality_mode_packet();
	test_training_packet_and_parse_confirm();
	test_wave_info_and_data_layout();
	test_wave_packet_layout();
	test_parse_wave_confirm();
	return 0;
}
