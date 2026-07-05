#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <gio/gio.h>

#include "rf-rdp-clipboard-stream.h"

struct read_result {
	GMainLoop *loop;
	GByteArray *bytes;
	GError *error;
};

static void on_read_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct read_result *result = data;

	result->bytes = rf_rdp_clipboard_read_stream_finish(
		G_INPUT_STREAM(source_object),
		res,
		&result->error
	);
	g_main_loop_quit(result->loop);
}

static void read_stream(
	GInputStream *stream,
	size_t max_bytes,
	struct read_result *result
)
{
	result->loop = g_main_loop_new(NULL, false);
	result->bytes = NULL;
	g_clear_error(&result->error);
	rf_rdp_clipboard_read_stream_async(
		stream,
		max_bytes,
		NULL,
		on_read_finish,
		result
	);
	g_main_loop_run(result->loop);
	g_main_loop_unref(result->loop);
	result->loop = NULL;
}

static void read_result_clear(struct read_result *result)
{
	g_clear_pointer(&result->bytes, g_byte_array_unref);
	g_clear_error(&result->error);
}

static void test_reads_stream_chunks_without_blocking_main_loop(void)
{
	const char text[] = "<p>remote html</p>";
	g_autoptr(GInputStream) stream = NULL;
	struct read_result result = { 0 };

	stream = g_memory_input_stream_new_from_data(text, strlen(text), NULL);
	read_stream(stream, 1024, &result);

	assert(result.error == NULL);
	assert(result.bytes != NULL);
	assert(result.bytes->len == strlen(text));
	assert(memcmp(result.bytes->data, text, strlen(text)) == 0);
	read_result_clear(&result);
}

static void test_rejects_stream_larger_than_limit(void)
{
	const char text[] = "abcdef";
	g_autoptr(GInputStream) stream = NULL;
	struct read_result result = { 0 };

	stream = g_memory_input_stream_new_from_data(text, strlen(text), NULL);
	read_stream(stream, 3, &result);

	assert(result.bytes == NULL);
	assert(result.error != NULL);
	assert(g_error_matches(result.error, G_IO_ERROR, G_IO_ERROR_FAILED));
	read_result_clear(&result);
}

int main(void)
{
	test_reads_stream_chunks_without_blocking_main_loop();
	test_rejects_stream_larger_than_limit();
	return 0;
}
