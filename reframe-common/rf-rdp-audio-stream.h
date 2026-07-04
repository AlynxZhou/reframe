#ifndef __RF_RDP_AUDIO_STREAM_H__
#define __RF_RDP_AUDIO_STREAM_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gio/gio.h>

#define RF_RDP_AUDIO_STREAM_MAX_PCM_BYTES (512u * 1024u)

struct rf_rdp_audio_pcm_header {
	uint32_t sample_rate;
	uint16_t channels;
	uint16_t frame_ms;
	uint64_t timestamp_us;
	uint32_t pcm_length;
};

bool rf_rdp_audio_stream_write_pcm(
	GOutputStream *stream,
	uint32_t sample_rate,
	uint16_t channels,
	uint16_t frame_ms,
	uint64_t timestamp_us,
	const uint8_t *pcm,
	size_t pcm_length,
	GError **error
);
bool rf_rdp_audio_stream_read_pcm(
	GInputStream *stream,
	struct rf_rdp_audio_pcm_header *header,
	GByteArray **pcm,
	GError **error
);
bool rf_rdp_audio_pcm_is_silent(
	const uint8_t *pcm,
	size_t pcm_length,
	int16_t threshold
);

#endif
