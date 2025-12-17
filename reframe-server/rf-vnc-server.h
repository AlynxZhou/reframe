#ifndef __RF_VNC_SERVER_H__
#define __RF_VNC_SERVER_H__

#include <stdbool.h>
#include <gio/gio.h>

#include "rf-config.h"

G_BEGIN_DECLS

#define RF_TYPE_VNC_SERVER rf_vnc_server_get_type()
G_DECLARE_DERIVABLE_TYPE(RfVNCServer, rf_vnc_server, RF, VNC_SERVER, GObject)

struct _RfVNCServerClass {
	GObjectClass parent_class;
	void (*start)(RfVNCServer *this);
	bool (*is_running)(RfVNCServer *this);
	void (*stop)(RfVNCServer *this);
	void (*set_desktop_name)(RfVNCServer *this, const char *desktop_name);
	void (*update)(
		RfVNCServer *this,
		GByteArray *buf,
		unsigned int width,
		unsigned int height
	);
	void (*flush)(RfVNCServer *this);
};

RfVNCServer *rf_vnc_server_new(RfConfig *config);
void rf_vnc_server_start(RfVNCServer *this);
bool rf_vnc_server_is_running(RfVNCServer *this);
void rf_vnc_server_stop(RfVNCServer *this);
void rf_vnc_server_set_desktop_name(RfVNCServer *this, const char *desktop_name);
void rf_vnc_server_update(
	RfVNCServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height
);
void rf_vnc_server_flush(RfVNCServer *this);
void rf_vnc_server_handle_keyboard_event(
	RfVNCServer *this,
	uint32_t keysym,
	bool down
);
void rf_vnc_server_handle_pointer_event(
	RfVNCServer *this,
	double rx,
	double ry,
	uint32_t mask
);
void rf_vnc_server_handle_resize_event(
	RfVNCServer *this,
	unsigned int width,
	unsigned int height
);
void rf_vnc_server_handle_first_client(RfVNCServer *this);
void rf_vnc_server_handle_last_client(RfVNCServer *this);

G_END_DECLS

#endif
