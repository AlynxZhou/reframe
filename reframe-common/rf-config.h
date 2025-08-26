#ifndef __RF_CONFIG_H__
#define __RF_CONFIG_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define RF_TYPE_CONFIG rf_config_get_type()
G_DECLARE_FINAL_TYPE(RfConfig, rf_config, RF, CONFIG, GObject)

RfConfig *rf_config_new(const char *config_path);
char *rf_config_get_card_path(RfConfig *this);
char *rf_config_get_connector(RfConfig *this);
unsigned int rf_config_get_desktop_width(RfConfig *this);
unsigned int rf_config_get_desktop_height(RfConfig *this);
int rf_config_get_monitor_x(RfConfig *this);
int rf_config_get_monitor_y(RfConfig *this);
unsigned int rf_config_get_fps(RfConfig *this);
unsigned int rf_config_get_port(RfConfig *this);

G_END_DECLS

#endif
