#ifndef __RF_APP_H__
#define __RF_APP_H__

#include <gio/gio.h>

G_BEGIN_DECLS

#define RF_TYPE_APP rf_app_get_type()
G_DECLARE_FINAL_TYPE(RfApp, rf_app, RF, APP, GApplication)

RfApp *rf_app_new(void);

G_END_DECLS

#endif
