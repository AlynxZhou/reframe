#include <glib.h>

#include "rf-buffer.h"

G_DEFINE_BOXED_TYPE(RfBuffer, rf_buffer, rf_buffer_copy, rf_buffer_free)

RfBuffer *rf_buffer_copy(RfBuffer *this)
{
	return g_memdup2(this, sizeof(*this));
}

void rf_buffer_free(RfBuffer *this)
{
	return g_free(this);
}
