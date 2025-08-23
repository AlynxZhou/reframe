#include <glib.h>

#include "rf-buffer.h"

G_DEFINE_BOXED_TYPE(RfBuffer, rf_buffer, rf_buffer_copy, rf_buffer_free)

RfBuffer *rf_buffer_copy(RfBuffer *d)
{
	return g_memdup2(d, sizeof(*d));
}

void rf_buffer_free(RfBuffer *d)
{
	return g_free(d);
}
