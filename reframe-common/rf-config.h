#ifndef __RF_CONFIG_H__
#define __RF_CONFIG_H__

#include <stdbool.h>
#include <glib-object.h>

G_BEGIN_DECLS

enum rf_wakeup_device { RF_WAKEUP_DEVICE_KEYBOARD, RF_WAKEUP_DEVICE_POINTER };

enum rf_damage_type {
	RF_DAMAGE_TYPE_DUMB,
	RF_DAMAGE_TYPE_CPU,
	RF_DAMAGE_TYPE_GPU
};

enum rf_vnc_backend { RF_VNC_BACKEND_LIBVNCSERVER, RF_VNC_BACKEND_NEATVNC };

#define RF_TYPE_CONFIG rf_config_get_type()
G_DECLARE_FINAL_TYPE(RfConfig, rf_config, RF, CONFIG, GObject)

RfConfig *rf_config_new(const char *config_path);
char *rf_config_get_card_path(RfConfig *this);
char *rf_config_get_connector(RfConfig *this);
unsigned int rf_config_get_rotation(RfConfig *this);
unsigned int rf_config_get_desktop_width(RfConfig *this);
unsigned int rf_config_get_desktop_height(RfConfig *this);
int rf_config_get_monitor_x(RfConfig *this);
int rf_config_get_monitor_y(RfConfig *this);
unsigned int rf_config_get_default_width(RfConfig *this);
unsigned int rf_config_get_default_height(RfConfig *this);
bool rf_config_get_resize(RfConfig *this);
bool rf_config_get_share(RfConfig *this);
bool rf_config_get_cursor(RfConfig *this);
bool rf_config_get_wakeup(RfConfig *this);
enum rf_wakeup_device rf_config_get_wakeup_device(RfConfig *this);
enum rf_damage_type rf_config_get_damage(RfConfig *this);
unsigned int rf_config_get_fps(RfConfig *this);
char **rf_config_get_vnc_ip_list(RfConfig *this);
unsigned int rf_config_get_vnc_port(RfConfig *this);
char *rf_config_get_vnc_password(RfConfig *this);
enum rf_vnc_backend rf_config_get_vnc_backend(RfConfig *this);
char *rf_config_get_neatvnc_username(RfConfig *this);
bool rf_config_get_neatvnc_allow_broken_crypto(RfConfig *this);
char *rf_config_get_neatvnc_rsa_private_key_file(RfConfig *this);
char *rf_config_get_neatvnc_tls_private_key_file(RfConfig *this);
char *rf_config_get_neatvnc_tls_certificate_file(RfConfig *this);

G_END_DECLS

#endif
