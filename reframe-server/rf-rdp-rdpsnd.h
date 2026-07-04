#ifndef __RF_RDP_RDPSND_H__
#define __RF_RDP_RDPSND_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_RDPSND_CHANNEL_NAME "rdpsnd"

#define RF_RDP_RDPSND_SNDC_CLOSE 0x01u
#define RF_RDP_RDPSND_SNDC_WAVE 0x02u
#define RF_RDP_RDPSND_SNDC_SETVOLUME 0x03u
#define RF_RDP_RDPSND_SNDC_WAVECONFIRM 0x05u
#define RF_RDP_RDPSND_SNDC_TRAINING 0x06u
#define RF_RDP_RDPSND_SNDC_FORMATS 0x07u
#define RF_RDP_RDPSND_SNDC_QUALITYMODE 0x0cu
#define RF_RDP_RDPSND_SNDC_WAVE2 0x0du

#define RF_RDP_RDPSND_WAVE_FORMAT_PCM 0x0001u
#define RF_RDP_RDPSND_CHANNEL_VERSION_WIN_7 6u
#define RF_RDP_RDPSND_CHANNEL_VERSION_WIN_MAX 8u
#define RF_RDP_RDPSND_QUALITY_MODE_DYNAMIC 2u
#define RF_RDP_RDPSND_MAX_FORMATS 16u
#define RF_RDP_RDPSND_WAVE_INFO_LENGTH 16u

struct rf_rdp_rdpsnd_audio_format {
	uint16_t tag;
	uint16_t channels;
	uint32_t samples_per_sec;
	uint32_t avg_bytes_per_sec;
	uint16_t block_align;
	uint16_t bits_per_sample;
};

struct rf_rdp_rdpsnd_client_formats {
	struct rf_rdp_rdpsnd_audio_format formats[RF_RDP_RDPSND_MAX_FORMATS];
	size_t format_count;
	uint8_t last_block_confirmed;
	uint16_t version;
};

size_t rf_rdp_rdpsnd_write_server_formats(
	uint8_t *data,
	size_t capacity,
	const struct rf_rdp_rdpsnd_audio_format *formats,
	size_t format_count,
	uint8_t last_block_confirmed
);
bool rf_rdp_rdpsnd_parse_client_formats(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_rdpsnd_client_formats *formats
);
int rf_rdp_rdpsnd_choose_pcm_format(
	const struct rf_rdp_rdpsnd_client_formats *formats,
	uint32_t preferred_rate,
	uint16_t preferred_channels
);
size_t rf_rdp_rdpsnd_write_quality_mode(uint8_t *data, size_t capacity);
size_t rf_rdp_rdpsnd_write_training(
	uint8_t *data,
	size_t capacity,
	uint16_t timestamp,
	uint16_t pack_size,
	const uint8_t *data_payload,
	size_t data_payload_length
);
bool rf_rdp_rdpsnd_parse_training_confirm(
	const uint8_t *data,
	size_t length,
	uint16_t *timestamp,
	uint16_t *pack_size
);
bool rf_rdp_rdpsnd_parse_wave_confirm(
	const uint8_t *data,
	size_t length,
	uint16_t *timestamp,
	uint8_t *confirmed_block_no
);
size_t rf_rdp_rdpsnd_write_wave_info(
	uint8_t *data,
	size_t capacity,
	uint8_t block_no,
	uint16_t format_no,
	uint16_t timestamp,
	const uint8_t *pcm,
	size_t pcm_length
);
size_t rf_rdp_rdpsnd_write_wave_data(
	uint8_t *data,
	size_t capacity,
	const uint8_t *pcm,
	size_t pcm_length
);
size_t rf_rdp_rdpsnd_write_wave(
	uint8_t *data,
	size_t capacity,
	uint8_t block_no,
	uint16_t format_no,
	uint16_t timestamp,
	const uint8_t *pcm,
	size_t pcm_length
);
size_t rf_rdp_rdpsnd_write_close(uint8_t *data, size_t capacity);

#endif
