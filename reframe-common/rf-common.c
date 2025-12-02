#include "rf-common.h"

ssize_t rf_send_header(GSocketConnection *connection, char type, size_t length)
{
	ssize_t ret = 0;
	GOutputStream *os = g_io_stream_get_output_stream(G_IO_STREAM(connection));
	ret = g_output_stream_write(os, &type, sizeof(type), NULL, NULL);
	if (ret <= 0)
		return ret;
	ret = g_output_stream_write(os, &length, sizeof(length), NULL, NULL);
	return ret;
}
