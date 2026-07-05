#ifndef __RF_RDP_SERVER_H__
#define __RF_RDP_SERVER_H__

#include "rf-remote-server.h"

G_BEGIN_DECLS

#define RF_TYPE_RDP_SERVER rf_rdp_server_get_type()
G_DECLARE_FINAL_TYPE(RfRDPServer, rf_rdp_server, RF, RDP_SERVER, RfRemoteServer)

G_MODULE_EXPORT RfRemoteServer *rf_rdp_server_new(RfConfig *config);

G_END_DECLS

#endif
