#ifndef __RF_CONVERTER_H__
#define __RF_CONVERTER_H__

#include <glib.h>

#include "rf-config.h"
#include "rf-common.h"

G_BEGIN_DECLS

#define RF_TYPE_CONVERTER rf_converter_get_type()
G_DECLARE_FINAL_TYPE(RfConverter, rf_converter, RF, CONVERTER, GObject)

RfConverter *rf_converter_new(RfConfig *config);
void rf_converter_set_card_path(RfConverter *this, const char *card_path);
int rf_converter_start(RfConverter *this);
bool rf_converter_is_running(RfConverter *this);
void rf_converter_stop(RfConverter *this);
GByteArray *rf_converter_convert(
	RfConverter *this,
	size_t length,
	const struct rf_buffer *bufs,
	unsigned int width,
	unsigned int height,
	struct rf_rect *damage
);

G_END_DECLS

#endif
