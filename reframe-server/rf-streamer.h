#ifndef __RF_STREAMER_H__
#define __RF_STREAMER_H__

#include <stdint.h>
#include <stdbool.h>
#include <gio/gio.h>

#include "rf-config.h"

G_BEGIN_DECLS

#define RF_TYPE_STREAMER rf_streamer_get_type()
G_DECLARE_FINAL_TYPE(RfStreamer, rf_streamer, RF, STREAMER, GSocketClient)

RfStreamer *rf_streamer_new(RfConfig *config);
void rf_streamer_set_socket_path(RfStreamer *this, const char *socket_path);
int rf_streamer_start(RfStreamer *this);
bool rf_streamer_is_running(RfStreamer *this);
void rf_streamer_stop(RfStreamer *this);
void rf_streamer_send_keyboard_event(
	RfStreamer *this,
	uint32_t keycode,
	bool down
);
void rf_streamer_send_pointer_event(
	RfStreamer *this,
	double rx,
	double ry,
	bool left,
	bool middle,
	bool right,
	bool back,
	bool forward,
	bool wup,
	bool wdown,
	bool wleft,
	bool wright
);
void rf_streamer_auth(RfStreamer *this, pid_t pid);

G_END_DECLS

#endif
