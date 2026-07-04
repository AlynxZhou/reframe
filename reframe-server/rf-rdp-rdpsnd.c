#include "rf-rdp-rdpsnd.h"

#include <string.h>

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

static bool write_header(
	uint8_t *data,
	size_t capacity,
	uint8_t type,
	uint16_t body_size
)
{
	if (data == NULL || capacity < 4)
		return false;
	data[0] = type;
	data[1] = 0;
	write_u16_le(data + 2, body_size);
	return true;
}

static bool write_audio_format(
	uint8_t *data,
	size_t capacity,
	const struct rf_rdp_rdpsnd_audio_format *format
)
{
	if (data == NULL || capacity < 18 || format == NULL)
		return false;

	write_u16_le(data, format->tag);
	write_u16_le(data + 2, format->channels);
	write_u32_le(data + 4, format->samples_per_sec);
	write_u32_le(data + 8, format->avg_bytes_per_sec);
	write_u16_le(data + 12, format->block_align);
	write_u16_le(data + 14, format->bits_per_sample);
	write_u16_le(data + 16, 0);
	return true;
}

static bool read_audio_format(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_rdpsnd_audio_format *format,
	size_t *bytes_read
)
{
	uint16_t extra_size = 0;

	if (bytes_read != NULL)
		*bytes_read = 0;
	if (data == NULL || length < 18 || format == NULL)
		return false;

	extra_size = read_u16_le(data + 16);
	if (extra_size > length - 18)
		return false;

	format->tag = read_u16_le(data);
	format->channels = read_u16_le(data + 2);
	format->samples_per_sec = read_u32_le(data + 4);
	format->avg_bytes_per_sec = read_u32_le(data + 8);
	format->block_align = read_u16_le(data + 12);
	format->bits_per_sample = read_u16_le(data + 14);
	if (bytes_read != NULL)
		*bytes_read = 18 + extra_size;
	return true;
}

size_t rf_rdp_rdpsnd_write_server_formats(
	uint8_t *data,
	size_t capacity,
	const struct rf_rdp_rdpsnd_audio_format *formats,
	size_t format_count,
	uint8_t last_block_confirmed
)
{
	const size_t body_size = 20 + format_count * 18;
	const size_t total_size = 4 + body_size;
	size_t offset = 4;

	if (data == NULL || formats == NULL || format_count == 0 ||
	    format_count > RF_RDP_RDPSND_MAX_FORMATS ||
	    body_size > UINT16_MAX || capacity < total_size)
		return 0;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_RDPSND_SNDC_FORMATS,
		    (uint16_t)body_size
	    ))
		return 0;

	write_u32_le(data + offset, 0);
	offset += 4;
	write_u32_le(data + offset, 0);
	offset += 4;
	write_u32_le(data + offset, 0);
	offset += 4;
	write_u16_le(data + offset, 0);
	offset += 2;
	write_u16_le(data + offset, (uint16_t)format_count);
	offset += 2;
	data[offset++] = last_block_confirmed;
	write_u16_le(data + offset, RF_RDP_RDPSND_CHANNEL_VERSION_WIN_MAX);
	offset += 2;
	data[offset++] = 0;

	for (size_t i = 0; i < format_count; ++i) {
		if (!write_audio_format(data + offset, capacity - offset, &formats[i]))
			return 0;
		offset += 18;
	}
	return offset;
}

bool rf_rdp_rdpsnd_parse_client_formats(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_rdpsnd_client_formats *formats
)
{
	size_t offset = 0;
	uint16_t body_size = 0;
	uint16_t format_count = 0;

	if (data == NULL || formats == NULL || length < 28 ||
	    data[0] != RF_RDP_RDPSND_SNDC_FORMATS)
		return false;

	body_size = read_u16_le(data + 2);
	if (body_size > length - 4 || body_size < 20)
		return false;

	memset(formats, 0, sizeof(*formats));
	offset = 4;
	offset += 4;
	offset += 4;
	offset += 4;
	offset += 2;
	format_count = read_u16_le(data + offset);
	offset += 2;
	formats->last_block_confirmed = data[offset++];
	formats->version = read_u16_le(data + offset);
	offset += 2;
	offset += 1;

	if (format_count > RF_RDP_RDPSND_MAX_FORMATS)
		format_count = RF_RDP_RDPSND_MAX_FORMATS;

	for (uint16_t i = 0; i < format_count; ++i) {
		size_t bytes_read = 0;

		if (offset >= length ||
		    !read_audio_format(
			    data + offset,
			    length - offset,
			    &formats->formats[formats->format_count],
			    &bytes_read
		    ))
			return false;
		offset += bytes_read;
		formats->format_count++;
	}
	return true;
}

static bool pcm_format_matches(
	const struct rf_rdp_rdpsnd_audio_format *format,
	uint32_t sample_rate,
	uint16_t channels
)
{
	if (format == NULL)
		return false;
	return format->tag == RF_RDP_RDPSND_WAVE_FORMAT_PCM &&
	       format->channels == channels &&
	       format->samples_per_sec == sample_rate &&
	       format->bits_per_sample == 16 &&
	       format->block_align == channels * 2;
}

int rf_rdp_rdpsnd_choose_pcm_format(
	const struct rf_rdp_rdpsnd_client_formats *formats,
	uint32_t preferred_rate,
	uint16_t preferred_channels
)
{
	if (formats == NULL)
		return -1;

	for (size_t i = 0; i < formats->format_count; ++i) {
		if (pcm_format_matches(
			    &formats->formats[i],
			    preferred_rate,
			    preferred_channels
		    ))
			return (int)i;
	}
	for (size_t i = 0; i < formats->format_count; ++i) {
		if (pcm_format_matches(
			    &formats->formats[i],
			    44100,
			    preferred_channels
		    ))
			return (int)i;
	}
	for (size_t i = 0; i < formats->format_count; ++i) {
		if (formats->formats[i].tag == RF_RDP_RDPSND_WAVE_FORMAT_PCM &&
		    formats->formats[i].bits_per_sample == 16 &&
		    formats->formats[i].channels > 0 &&
		    formats->formats[i].block_align ==
			    formats->formats[i].channels * 2)
			return (int)i;
	}
	return -1;
}

size_t rf_rdp_rdpsnd_write_quality_mode(uint8_t *data, size_t capacity)
{
	if (capacity < 8 ||
	    !write_header(data, capacity, RF_RDP_RDPSND_SNDC_QUALITYMODE, 4))
		return 0;

	write_u16_le(data + 4, RF_RDP_RDPSND_QUALITY_MODE_DYNAMIC);
	write_u16_le(data + 6, 0);
	return 8;
}

size_t rf_rdp_rdpsnd_write_training(
	uint8_t *data,
	size_t capacity,
	uint16_t timestamp,
	uint16_t pack_size,
	const uint8_t *data_payload,
	size_t data_payload_length
)
{
	const size_t body_size = 4 + data_payload_length;
	const size_t total_size = 4 + body_size;

	if (data == NULL || body_size > UINT16_MAX || capacity < total_size ||
	    (data_payload == NULL && data_payload_length > 0) ||
	    data_payload_length != pack_size)
		return 0;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_RDPSND_SNDC_TRAINING,
		    (uint16_t)body_size
	    ))
		return 0;

	write_u16_le(data + 4, timestamp);
	write_u16_le(data + 6, pack_size);
	if (data_payload_length > 0)
		memcpy(data + 8, data_payload, data_payload_length);
	return total_size;
}

bool rf_rdp_rdpsnd_parse_training_confirm(
	const uint8_t *data,
	size_t length,
	uint16_t *timestamp,
	uint16_t *pack_size
)
{
	if (data == NULL || length < 8 ||
	    data[0] != RF_RDP_RDPSND_SNDC_TRAINING ||
	    read_u16_le(data + 2) < 4)
		return false;

	if (timestamp != NULL)
		*timestamp = read_u16_le(data + 4);
	if (pack_size != NULL)
		*pack_size = read_u16_le(data + 6);
	return true;
}

bool rf_rdp_rdpsnd_parse_wave_confirm(
	const uint8_t *data,
	size_t length,
	uint16_t *timestamp,
	uint8_t *confirmed_block_no
)
{
	if (data == NULL || length < 8 ||
	    data[0] != RF_RDP_RDPSND_SNDC_WAVECONFIRM ||
	    read_u16_le(data + 2) < 4)
		return false;

	if (timestamp != NULL)
		*timestamp = read_u16_le(data + 4);
	if (confirmed_block_no != NULL)
		*confirmed_block_no = data[6];
	return true;
}

size_t rf_rdp_rdpsnd_write_wave_info(
	uint8_t *data,
	size_t capacity,
	uint8_t block_no,
	uint16_t format_no,
	uint16_t timestamp,
	const uint8_t *pcm,
	size_t pcm_length
)
{
	if (data == NULL || capacity < RF_RDP_RDPSND_WAVE_INFO_LENGTH ||
	    pcm == NULL || pcm_length < 4 || pcm_length > UINT16_MAX - 8u)
		return 0;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_RDPSND_SNDC_WAVE,
		    (uint16_t)(8u + pcm_length)
	    ))
		return 0;

	write_u16_le(data + 4, timestamp);
	write_u16_le(data + 6, format_no);
	data[8] = block_no;
	data[9] = 0;
	data[10] = 0;
	data[11] = 0;
	memcpy(data + 12, pcm, 4);
	return RF_RDP_RDPSND_WAVE_INFO_LENGTH;
}

size_t rf_rdp_rdpsnd_write_wave_data(
	uint8_t *data,
	size_t capacity,
	const uint8_t *pcm,
	size_t pcm_length
)
{
	if (pcm_length <= 4)
		return 0;
	if (data == NULL || pcm == NULL || capacity < pcm_length - 4)
		return 0;

	memcpy(data, pcm + 4, pcm_length - 4);
	return pcm_length - 4;
}

size_t rf_rdp_rdpsnd_write_wave(
	uint8_t *data,
	size_t capacity,
	uint8_t block_no,
	uint16_t format_no,
	uint16_t timestamp,
	const uint8_t *pcm,
	size_t pcm_length
)
{
	size_t info_length = 0;
	size_t data_length = 0;

	if (capacity < RF_RDP_RDPSND_WAVE_INFO_LENGTH)
		return 0;
	info_length = rf_rdp_rdpsnd_write_wave_info(
		data,
		capacity,
		block_no,
		format_no,
		timestamp,
		pcm,
		pcm_length
	);
	if (info_length == 0)
		return 0;
	data_length = rf_rdp_rdpsnd_write_wave_data(
		data + info_length,
		capacity - info_length,
		pcm,
		pcm_length
	);
	if (pcm_length > 4 && data_length == 0)
		return 0;
	return info_length + data_length;
}

size_t rf_rdp_rdpsnd_write_close(uint8_t *data, size_t capacity)
{
	if (capacity < 4 ||
	    !write_header(data, capacity, RF_RDP_RDPSND_SNDC_CLOSE, 0))
		return 0;
	return 4;
}
