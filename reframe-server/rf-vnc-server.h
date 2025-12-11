#ifndef __RF_VNC_SERVER_H__
#define __RF_VNC_SERVER_H__

#include <gio/gio.h>

#include "rf-config.h"

G_BEGIN_DECLS

#define RF_TYPE_VNC_SERVER rf_vnc_server_get_type()
G_DECLARE_FINAL_TYPE(RfVNCServer, rf_vnc_server, RF, VNC_SERVER, GSocketService)

RfVNCServer *rf_vnc_server_new(RfConfig *config);
void rf_vnc_server_start(RfVNCServer *this);
void rf_vnc_server_stop(RfVNCServer *this);
void rf_vnc_server_set_desktop_name(RfVNCServer *this, const char *desktop_name);
void rf_vnc_server_update(
	RfVNCServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height
);
void rf_vnc_server_flush(RfVNCServer *this);

G_END_DECLS

#endif
