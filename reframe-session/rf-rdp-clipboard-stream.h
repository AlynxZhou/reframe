#ifndef __RF_RDP_CLIPBOARD_STREAM_H__
#define __RF_RDP_CLIPBOARD_STREAM_H__

#include <stddef.h>

#include <gio/gio.h>

void rf_rdp_clipboard_read_stream_async(
	GInputStream *stream,
	size_t max_bytes,
	GCancellable *cancellable,
	GAsyncReadyCallback callback,
	void *user_data
);
GByteArray *rf_rdp_clipboard_read_stream_finish(
	GInputStream *stream,
	GAsyncResult *result,
	GError **error
);

#endif
