#include "rf-rdp-rdpsnd.h"

#include <string.h>

static const int16_t ima_step_index_table[] = {
	-1, -1, -1, -1, 2, 4, 6, 8,
	-1, -1, -1, -1, 2, 4, 6, 8
};

static const int16_t ima_step_size_table[] = {
	7,     8,     9,     10,    11,    12,    13,    14,
	16,    17,    19,    21,    23,    25,    28,    31,
	34,    37,    41,    45,    50,    55,    60,    66,
	73,    80,    88,    97,    107,   118,   130,   143,
	157,   173,   190,   209,   230,   253,   279,   307,
	337,   371,   408,   449,   494,   544,   598,   658,
	724,   796,   876,   963,   1060,  1166,  1282,  1411,
	1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
	3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
	7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
	15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
	32767
};

static const struct {
	uint8_t byte_num;
	uint8_t byte_shift;
} ima_stereo_encode_map[] = {
	{ 0, 0 }, { 4, 0 }, { 0, 4 }, { 4, 4 },
	{ 1, 0 }, { 5, 0 }, { 1, 4 }, { 5, 4 },
	{ 2, 0 }, { 6, 0 }, { 2, 4 }, { 6, 4 },
	{ 3, 0 }, { 7, 0 }, { 3, 4 }, { 7, 4 }
};

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
	const size_t length = 18 + (format != NULL ? format->extra_size : 0);

	if (data == NULL || format == NULL || format->extra_size > RF_RDP_RDPSND_MAX_FORMAT_EXTRA ||
	    capacity < length)
		return false;

	write_u16_le(data, format->tag);
	write_u16_le(data + 2, format->channels);
	write_u32_le(data + 4, format->samples_per_sec);
	write_u32_le(data + 8, format->avg_bytes_per_sec);
	write_u16_le(data + 12, format->block_align);
	write_u16_le(data + 14, format->bits_per_sample);
	write_u16_le(data + 16, format->extra_size);
	if (format->extra_size > 0)
		memcpy(data + 18, format->extra, format->extra_size);
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
	format->extra_size = extra_size < RF_RDP_RDPSND_MAX_FORMAT_EXTRA ?
		extra_size :
		RF_RDP_RDPSND_MAX_FORMAT_EXTRA;
	if (format->extra_size > 0)
		memcpy(format->extra, data + 18, format->extra_size);
	if (bytes_read != NULL)
		*bytes_read = 18 + extra_size;
	return true;
}

const char *rf_rdp_rdpsnd_format_name(uint16_t tag)
{
	switch (tag) {
	case RF_RDP_RDPSND_WAVE_FORMAT_PCM:
		return "PCM";
	case RF_RDP_RDPSND_WAVE_FORMAT_DVI_ADPCM:
		return "DVI ADPCM";
	default:
		return "unknown";
	}
}

uint16_t rf_rdp_rdpsnd_dvi_adpcm_samples_per_block(
	uint32_t sample_rate,
	uint16_t frame_ms
)
{
	uint32_t samples = sample_rate * frame_ms / 1000u;

	if (samples < 2)
		samples = 2;
	if ((samples % 2) != 0)
		samples++;
	if (samples > UINT16_MAX)
		return UINT16_MAX - 1u;
	return (uint16_t)samples;
}

struct rf_rdp_rdpsnd_audio_format rf_rdp_rdpsnd_make_dvi_adpcm_format(
	uint32_t sample_rate,
	uint16_t channels,
	uint16_t samples_per_block
)
{
	struct rf_rdp_rdpsnd_audio_format format = { 0 };
	uint32_t block_align = 0;

	if (channels == 0)
		channels = 1;
	if (samples_per_block < 2)
		samples_per_block = 2;
	if ((samples_per_block % 2) != 0)
		samples_per_block++;

	block_align = 4u * channels + (samples_per_block / 2u) * channels;
	if (block_align > UINT16_MAX)
		block_align = UINT16_MAX;

	format.tag = RF_RDP_RDPSND_WAVE_FORMAT_DVI_ADPCM;
	format.channels = channels;
	format.samples_per_sec = sample_rate;
	format.block_align = (uint16_t)block_align;
	format.bits_per_sample = 4;
	format.avg_bytes_per_sec =
		samples_per_block > 0 ?
			(uint32_t)(((uint64_t)sample_rate * block_align) /
				   samples_per_block) :
			0;
	format.extra_size = 2;
	write_u16_le(format.extra, samples_per_block);
	return format;
}

size_t rf_rdp_rdpsnd_write_server_formats(
	uint8_t *data,
	size_t capacity,
	const struct rf_rdp_rdpsnd_audio_format *formats,
	size_t format_count,
	uint8_t last_block_confirmed
)
{
	size_t body_size = 20;
	size_t total_size = 0;
	size_t offset = 4;

	if (data == NULL || formats == NULL || format_count == 0 ||
	    format_count > RF_RDP_RDPSND_MAX_FORMATS)
		return 0;
	for (size_t i = 0; i < format_count; ++i) {
		if (formats[i].extra_size > RF_RDP_RDPSND_MAX_FORMAT_EXTRA ||
		    body_size > SIZE_MAX - 18u - formats[i].extra_size)
			return 0;
		body_size += 18u + formats[i].extra_size;
	}
	total_size = 4 + body_size;
	if (body_size > UINT16_MAX || capacity < total_size)
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
		offset += 18 + formats[i].extra_size;
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

static bool dvi_adpcm_format_matches(
	const struct rf_rdp_rdpsnd_audio_format *format,
	uint32_t sample_rate,
	uint16_t channels
)
{
	if (format == NULL)
		return false;
	return format->tag == RF_RDP_RDPSND_WAVE_FORMAT_DVI_ADPCM &&
	       format->channels == channels &&
	       format->samples_per_sec == sample_rate &&
	       format->bits_per_sample == 4 &&
	       format->block_align >= channels * 4u + channels &&
	       format->extra_size >= 2 &&
	       read_u16_le(format->extra) >= 2;
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

int rf_rdp_rdpsnd_choose_audio_format(
	const struct rf_rdp_rdpsnd_client_formats *formats,
	uint32_t preferred_rate,
	uint16_t preferred_channels,
	bool prefer_adpcm
)
{
	if (formats == NULL)
		return -1;

	if (prefer_adpcm) {
		for (size_t i = 0; i < formats->format_count; ++i) {
			if (dvi_adpcm_format_matches(
				    &formats->formats[i],
				    preferred_rate,
				    preferred_channels
			    ))
				return (int)i;
		}
	}
	return rf_rdp_rdpsnd_choose_pcm_format(
		formats,
		preferred_rate,
		preferred_channels
	);
}

static int16_t read_i16_le(const uint8_t *data)
{
	return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

static uint8_t ima_encode_sample(
	int16_t *last_sample,
	int16_t *last_step,
	int16_t sample
)
{
	int32_t step = ima_step_size_table[*last_step];
	int32_t error = (int32_t)sample - *last_sample;
	int32_t delta = error;
	int32_t diff = step >> 3;
	uint8_t encoded = 0;

	if (error < 0) {
		encoded = 8;
		error = -error;
	}
	if (error >= step) {
		encoded |= 4;
		error -= step;
	}
	step >>= 1;
	if (error >= step) {
		encoded |= 2;
		error -= step;
	}
	step >>= 1;
	if (error >= step) {
		encoded |= 1;
		error -= step;
	}

	if (delta < 0)
		diff = delta + error - diff;
	else
		diff = delta - error + diff;
	diff += *last_sample;
	if (diff < -32768)
		diff = -32768;
	else if (diff > 32767)
		diff = 32767;
	*last_sample = (int16_t)diff;

	*last_step += ima_step_index_table[encoded];
	if (*last_step < 0)
		*last_step = 0;
	else if ((size_t)*last_step >= sizeof(ima_step_size_table) / sizeof(ima_step_size_table[0]))
		*last_step =
			(int16_t)(sizeof(ima_step_size_table) / sizeof(ima_step_size_table[0]) - 1);
	return encoded;
}

size_t rf_rdp_rdpsnd_encode_dvi_adpcm(
	uint8_t *dst,
	size_t dst_capacity,
	const uint8_t *pcm,
	size_t pcm_length,
	const struct rf_rdp_rdpsnd_audio_format *format
)
{
	int16_t last_sample[2] = { 0 };
	int16_t last_step[2] = { 0 };
	const uint16_t channels = format != NULL ? format->channels : 0;
	const size_t block_align = format != NULL ? format->block_align : 0;
	const size_t sample_count = channels > 0 ? pcm_length / (channels * 2u) : 0;
	size_t src_frame = 0;
	size_t out = 0;

	if (dst == NULL || pcm == NULL || format == NULL ||
	    format->tag != RF_RDP_RDPSND_WAVE_FORMAT_DVI_ADPCM ||
	    (channels != 1 && channels != 2) || format->bits_per_sample != 4 ||
	    block_align == 0 || dst_capacity < block_align ||
	    pcm_length == 0 || (pcm_length % (channels * 2u)) != 0)
		return 0;

	if (channels == 1 && block_align < 5)
		return 0;
	if (channels == 2 && (block_align < 16 || ((block_align - 8) % 8) != 0))
		return 0;

	memset(dst, 0, block_align);
	for (uint16_t channel = 0; channel < channels; ++channel) {
		if (sample_count > 0)
			last_sample[channel] = read_i16_le(pcm + channel * 2u);
		write_u16_le(dst + out, (uint16_t)last_sample[channel]);
		dst[out + 2] = 0;
		dst[out + 3] = 0;
		out += 4;
	}

	if (channels == 1) {
		while (out < block_align) {
			uint8_t encoded = 0;

			for (unsigned int nibble = 0; nibble < 2; ++nibble) {
				int16_t sample = last_sample[0];

				if (src_frame < sample_count)
					sample = read_i16_le(pcm + src_frame * 2u);
				encoded |= ima_encode_sample(
					&last_sample[0],
					&last_step[0],
					sample
				) << (nibble * 4u);
				src_frame++;
			}
			dst[out++] = encoded;
		}
		return block_align;
	}

	while (out + 8 <= block_align) {
		uint8_t group[8] = { 0 };

		for (size_t i = 0; i < 16; ++i) {
			const size_t channel = i % 2u;
			int16_t sample = last_sample[channel];

			if (src_frame < sample_count)
				sample = read_i16_le(
					pcm + (src_frame * channels + channel) * 2u
				);
			group[ima_stereo_encode_map[i].byte_num] |=
				ima_encode_sample(
					&last_sample[channel],
					&last_step[channel],
					sample
				) << ima_stereo_encode_map[i].byte_shift;
			if (channel == 1)
				src_frame++;
		}
		memcpy(dst + out, group, sizeof(group));
		out += sizeof(group);
	}
	return block_align;
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
	if (data == NULL || pcm == NULL || capacity < pcm_length)
		return 0;

	memset(data, 0, 4);
	memcpy(data + 4, pcm + 4, pcm_length - 4);
	return pcm_length;
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

size_t rf_rdp_rdpsnd_write_wave2(
	uint8_t *data,
	size_t capacity,
	uint8_t block_no,
	uint16_t format_no,
	uint16_t timestamp,
	uint32_t audio_timestamp,
	const uint8_t *pcm,
	size_t pcm_length
)
{
	const size_t header_length = 16;
	const size_t body_size = 12 + pcm_length;
	const size_t total_size = header_length + pcm_length;

	if (data == NULL || pcm == NULL || pcm_length == 0 ||
	    body_size > UINT16_MAX || capacity < total_size)
		return 0;
	if (!write_header(
		    data,
		    capacity,
		    RF_RDP_RDPSND_SNDC_WAVE2,
		    (uint16_t)body_size
	    ))
		return 0;

	write_u16_le(data + 4, timestamp);
	write_u16_le(data + 6, format_no);
	data[8] = block_no;
	data[9] = 0;
	data[10] = 0;
	data[11] = 0;
	write_u32_le(data + 12, audio_timestamp);
	memcpy(data + header_length, pcm, pcm_length);
	return total_size;
}

size_t rf_rdp_rdpsnd_write_close(uint8_t *data, size_t capacity)
{
	if (capacity < 4 ||
	    !write_header(data, capacity, RF_RDP_RDPSND_SNDC_CLOSE, 0))
		return 0;
	return 4;
}
