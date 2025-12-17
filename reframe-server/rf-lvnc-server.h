#ifndef __RF_LVNC_SERVER_H__
#define __RF_LVNC_SERVER_H__

#include "rf-config.h"
#include "rf-vnc-server.h"

G_BEGIN_DECLS

#define RF_TYPE_LVNC_SERVER rf_lvnc_server_get_type()
G_DECLARE_FINAL_TYPE(RfLVNCServer, rf_lvnc_server, RF, LVNC_SERVER, RfVNCServer)

RfVNCServer *rf_lvnc_server_new(RfConfig *config);

G_END_DECLS

#endif
