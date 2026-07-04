#include <assert.h>
#include <string.h>

#include <gio/gio.h>

#include "rf-rdp-audio-stream.h"

static void test_roundtrip_pcm_message(void)
{
	GError *error = NULL;
	g_autoptr(GBytes) bytes = NULL;
	GInputStream *input = NULL;
	GOutputStream *output = NULL;
	struct rf_rdp_audio_pcm_header header = { 0 };
	GByteArray *pcm = NULL;
	const uint8_t samples[8] = { 1, 0, 2, 0, 3, 0, 4, 0 };

	output = g_memory_output_stream_new_resizable();

	assert(rf_rdp_audio_stream_write_pcm(
		output,
		48000,
		2,
		20,
		123456,
		samples,
		sizeof(samples),
		&error
	));
	assert(error == NULL);
	assert(g_output_stream_close(output, NULL, &error));
	assert(error == NULL);
	bytes = g_memory_output_stream_steal_as_bytes(
		G_MEMORY_OUTPUT_STREAM(output)
	);
	input = g_memory_input_stream_new_from_bytes(bytes);

	assert(rf_rdp_audio_stream_read_pcm(input, &header, &pcm, &error));
	assert(error == NULL);
	assert(header.sample_rate == 48000);
	assert(header.channels == 2);
	assert(header.frame_ms == 20);
	assert(header.timestamp_us == 123456);
	assert(pcm->len == sizeof(samples));
	assert(memcmp(pcm->data, samples, sizeof(samples)) == 0);

	g_byte_array_unref(pcm);
	g_object_unref(input);
	g_object_unref(output);
}

static void test_rejects_invalid_pcm_format(void)
{
	GError *error = NULL;
	g_autoptr(GOutputStream) output = NULL;
	const uint8_t samples[8] = { 0 };

	output = g_memory_output_stream_new_resizable();
	assert(!rf_rdp_audio_stream_write_pcm(
		output,
		32000,
		2,
		20,
		0,
		samples,
		sizeof(samples),
		&error
	));
	assert(error != NULL);
	assert(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT));
	g_clear_error(&error);
}

int main(void)
{
	test_roundtrip_pcm_message();
	test_rejects_invalid_pcm_format();
	return 0;
}
