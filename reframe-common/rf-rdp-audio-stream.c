#include "rf-rdp-audio-stream.h"

#include <string.h>

#include "rf-common.h"

#define RF_RDP_AUDIO_STREAM_HEADER_LENGTH 25u
#define RF_RDP_AUDIO_STREAM_METADATA_LENGTH 20u

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
	data[3] = value >> 24;
}

static void write_u64_le(uint8_t *data, uint64_t value)
{
	for (size_t i = 0; i < 8; ++i)
		data[i] = (uint8_t)(value >> (i * 8));
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

static uint64_t read_u64_le(const uint8_t *data)
{
	uint64_t value = 0;

	for (size_t i = 0; i < 8; ++i)
		value |= (uint64_t)data[i] << (i * 8);
	return value;
}

static bool pcm_header_valid(const struct rf_rdp_audio_pcm_header *header)
{
	if (header == NULL)
		return false;
	if (header->sample_rate != 44100 && header->sample_rate != 48000)
		return false;
	if (header->channels != 1 && header->channels != 2)
		return false;
	if (header->frame_ms != 10 && header->frame_ms != 20 &&
	    header->frame_ms != 40)
		return false;
	return header->pcm_length > 0 &&
	       header->pcm_length <= RF_RDP_AUDIO_STREAM_MAX_PCM_BYTES;
}

static bool read_all(
	GInputStream *stream,
	void *buffer,
	size_t length,
	GError **error
)
{
	gsize read = 0;

	return g_input_stream_read_all(
		stream,
		buffer,
		length,
		&read,
		NULL,
		error
	) && read == length;
}

static bool write_all(
	GOutputStream *stream,
	const void *buffer,
	size_t length,
	GError **error
)
{
	gsize written = 0;

	return g_output_stream_write_all(
		stream,
		buffer,
		length,
		&written,
		NULL,
		error
	) && written == length;
}

bool rf_rdp_audio_stream_write_pcm(
	GOutputStream *stream,
	uint32_t sample_rate,
	uint16_t channels,
	uint16_t frame_ms,
	uint64_t timestamp_us,
	const uint8_t *pcm,
	size_t pcm_length,
	GError **error
)
{
	uint8_t header[RF_RDP_AUDIO_STREAM_HEADER_LENGTH] = { 0 };
	struct rf_rdp_audio_pcm_header pcm_header = { 0 };
	uint32_t payload_length = 0;

	g_return_val_if_fail(G_IS_OUTPUT_STREAM(stream), false);

	if (pcm == NULL || pcm_length > UINT32_MAX) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_ARGUMENT,
			"invalid RDP audio PCM frame"
		);
		return false;
	}
	pcm_header = (struct rf_rdp_audio_pcm_header){
		.sample_rate = sample_rate,
		.channels = channels,
		.frame_ms = frame_ms,
		.timestamp_us = timestamp_us,
		.pcm_length = (uint32_t)pcm_length
	};
	if (!pcm_header_valid(&pcm_header)) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_ARGUMENT,
			"invalid RDP audio PCM frame"
		);
		return false;
	}
	payload_length =
		RF_RDP_AUDIO_STREAM_METADATA_LENGTH + pcm_header.pcm_length;

	header[0] = RF_MSG_TYPE_RDP_AUDIO_PCM;
	write_u32_le(header + 1, payload_length);
	write_u32_le(header + 5, sample_rate);
	write_u16_le(header + 9, channels);
	write_u16_le(header + 11, frame_ms);
	write_u64_le(header + 13, timestamp_us);
	write_u32_le(header + 21, pcm_header.pcm_length);

	return write_all(stream, header, sizeof(header), error) &&
	       write_all(stream, pcm, pcm_header.pcm_length, error);
}

bool rf_rdp_audio_stream_read_pcm(
	GInputStream *stream,
	struct rf_rdp_audio_pcm_header *header,
	GByteArray **pcm,
	GError **error
)
{
	uint8_t wire_header[RF_RDP_AUDIO_STREAM_HEADER_LENGTH] = { 0 };
	uint32_t payload_length = 0;
	GByteArray *buffer = NULL;

	g_return_val_if_fail(G_IS_INPUT_STREAM(stream), false);

	if (header == NULL || pcm == NULL) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_ARGUMENT,
			"missing RDP audio output buffer"
		);
		return false;
	}

	*pcm = NULL;
	if (!read_all(stream, wire_header, sizeof(wire_header), error))
		return false;
	if (wire_header[0] != RF_MSG_TYPE_RDP_AUDIO_PCM) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_DATA,
			"unexpected RDP audio message type"
		);
		return false;
	}

	payload_length = read_u32_le(wire_header + 1);
	header->sample_rate = read_u32_le(wire_header + 5);
	header->channels = read_u16_le(wire_header + 9);
	header->frame_ms = read_u16_le(wire_header + 11);
	header->timestamp_us = read_u64_le(wire_header + 13);
	header->pcm_length = read_u32_le(wire_header + 21);

	if (payload_length !=
		    RF_RDP_AUDIO_STREAM_METADATA_LENGTH + header->pcm_length ||
	    !pcm_header_valid(header)) {
		g_set_error(
			error,
			G_IO_ERROR,
			G_IO_ERROR_INVALID_DATA,
			"invalid RDP audio PCM payload"
		);
		return false;
	}

	buffer = g_byte_array_sized_new(header->pcm_length);
	g_byte_array_set_size(buffer, header->pcm_length);
	if (!read_all(stream, buffer->data, buffer->len, error)) {
		g_byte_array_unref(buffer);
		return false;
	}

	*pcm = buffer;
	return true;
}

bool rf_rdp_audio_pcm_is_silent(
	const uint8_t *pcm,
	size_t pcm_length,
	int16_t threshold
)
{
	const int16_t absolute_threshold = threshold < 0 ? -threshold : threshold;

	if (pcm == NULL || pcm_length == 0 || (pcm_length % 2) != 0)
		return false;

	for (size_t i = 0; i < pcm_length; i += 2) {
		const int16_t sample =
			(int16_t)((uint16_t)pcm[i] | ((uint16_t)pcm[i + 1] << 8));
		const int32_t value = sample < 0 ? -(int32_t)sample : sample;

		if (value > absolute_threshold)
			return false;
	}
	return true;
}
