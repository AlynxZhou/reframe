#ifndef __RF_NVNC_SERVER_H__
#define __RF_NVNC_SERVER_H__

#include "glib-object.h"
#include "rf-config.h"
#include "rf-vnc-server.h"

G_BEGIN_DECLS

#define RF_TYPE_NVNC_SERVER rf_nvnc_server_get_type()
G_DECLARE_FINAL_TYPE(RfNVNCServer, rf_nvnc_server, RF, NVNC_SERVER, RfVNCServer)

RfVNCServer *rf_nvnc_server_new(RfConfig *config);

G_END_DECLS

#endif
