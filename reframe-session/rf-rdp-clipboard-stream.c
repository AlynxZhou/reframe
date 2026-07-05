#include "rf-rdp-clipboard-stream.h"

#define RF_RDP_CLIPBOARD_STREAM_CHUNK_SIZE 4096u

struct stream_read_request {
	GTask *task;
	GByteArray *bytes;
	size_t max_bytes;
};

static void stream_read_request_free(struct stream_read_request *request)
{
	if (request == NULL)
		return;
	g_clear_object(&request->task);
	g_clear_pointer(&request->bytes, g_byte_array_unref);
	g_free(request);
}

static void read_next_chunk(struct stream_read_request *request);

static void on_read_chunk(GObject *source_object, GAsyncResult *res, void *data)
{
	struct stream_read_request *request = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(
		G_INPUT_STREAM(source_object),
		res,
		&error
	);
	gsize size = 0;
	const uint8_t *chunk = NULL;

	if (bytes == NULL) {
		g_task_return_error(request->task, g_steal_pointer(&error));
		stream_read_request_free(request);
		return;
	}

	chunk = g_bytes_get_data(bytes, &size);
	if (size == 0) {
		GByteArray *result = g_steal_pointer(&request->bytes);

		g_task_return_pointer(
			request->task,
			result,
			(GDestroyNotify)g_byte_array_unref
		);
		stream_read_request_free(request);
		return;
	}

	if (size > request->max_bytes ||
	    request->bytes->len > request->max_bytes - size) {
		g_task_return_new_error(
			request->task,
			G_IO_ERROR,
			G_IO_ERROR_FAILED,
			"clipboard payload too large"
		);
		stream_read_request_free(request);
		return;
	}

	g_byte_array_append(request->bytes, chunk, size);
	read_next_chunk(request);
}

static void read_next_chunk(struct stream_read_request *request)
{
	GInputStream *stream = g_task_get_source_object(request->task);

	g_input_stream_read_bytes_async(
		stream,
		RF_RDP_CLIPBOARD_STREAM_CHUNK_SIZE,
		G_PRIORITY_DEFAULT,
		g_task_get_cancellable(request->task),
		on_read_chunk,
		request
	);
}

void rf_rdp_clipboard_read_stream_async(
	GInputStream *stream,
	size_t max_bytes,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	void *user_data
)
{
	struct stream_read_request *request = NULL;

	g_return_if_fail(G_IS_INPUT_STREAM(stream));

	request = g_new0(struct stream_read_request, 1);
	request->task = g_task_new(stream, cancellable, callback, user_data);
	request->bytes = g_byte_array_new();
	request->max_bytes = max_bytes;
	read_next_chunk(request);
}

GByteArray *rf_rdp_clipboard_read_stream_finish(
	GInputStream *stream,
	GAsyncResult *result,
	GError **error
)
{
	g_return_val_if_fail(g_task_is_valid(result, stream), NULL);

	return g_task_propagate_pointer(G_TASK(result), error);
}
