#ifndef __RF_STREAMER_H__
#define __RF_STREAMER_H__

#include <gio/gio.h>

#include "rf-config.h"

G_BEGIN_DECLS

#define RF_TYPE_STREAMER rf_streamer_get_type()
G_DECLARE_FINAL_TYPE(RfStreamer, rf_streamer, RF, STREAMER, GObject)

RfStreamer *rf_streamer_new(RfConfig *config);
void rf_streamer_set_socket_path(RfStreamer *s, const char *socket_path);
void rf_streamer_start(RfStreamer *s);
void rf_streamer_stop(RfStreamer *s);

G_END_DECLS

#endif
