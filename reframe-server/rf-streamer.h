#ifndef __RF_STREAMER_H__
#define __RF_STREAMER_H__

#include <stdbool.h>
#include <gio/gio.h>

#include "rf-config.h"

G_BEGIN_DECLS

#define RF_TYPE_STREAMER rf_streamer_get_type()
G_DECLARE_FINAL_TYPE(RfStreamer, rf_streamer, RF, STREAMER, GObject)

RfStreamer *rf_streamer_new(RfConfig *config);
void rf_streamer_set_socket_path(RfStreamer *s, const char *socket_path);
int rf_streamer_start(RfStreamer *this);
void rf_streamer_stop(RfStreamer *s);
void rf_streamer_send_keyboard_event(RfStreamer *this, uint32_t keycode, bool down);
void rf_streamer_send_pointer_event(RfStreamer *this, double rx, double ry, bool left, bool middle, bool right, bool wup, bool wdown);

G_END_DECLS

#endif
