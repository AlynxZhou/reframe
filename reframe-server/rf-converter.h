#ifndef __RF_CONVERTER_H__
#define __RF_CONVERTER_H__

#include <glib.h>

#include "rf-buffer.h"

G_BEGIN_DECLS

#define RF_TYPE_CONVERTER rf_converter_get_type()
G_DECLARE_FINAL_TYPE(RfConverter, rf_converter, RF, CONVERTER, GObject)

RfConverter *rf_converter_new(void);
unsigned char *rf_converter_convert(RfConverter *c, const RfBuffer *b,
				    unsigned int width, unsigned int height);

G_END_DECLS

#endif
