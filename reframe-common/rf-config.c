#include <glib.h>

#include "rf-config.h"

struct _RfConfig {
	GObject parent_object;
	GKeyFile *f;
};
G_DEFINE_TYPE(RfConfig, rf_config, G_TYPE_OBJECT)

#define RF_CONFIG_GROUP_REFRAME "reframe"
#define RF_CONFIG_GROUP_XKB "xkb"
#define RF_CONFIG_GROUP_VNC "vnc"
#define RF_CONFIG_GROUP_LIBVNCSERVER "libvncserver"
#define RF_CONFIG_GROUP_NEATVNC "neatvnc"

static void finalize(GObject *o)
{
	RfConfig *this = RF_CONFIG(o);

	g_clear_pointer(&this->f, g_key_file_free);

	G_OBJECT_CLASS(rf_config_parent_class)->finalize(o);
}

static void rf_config_init(RfConfig *this)
{
	this->f = g_key_file_new();
}

static void rf_config_class_init(RfConfigClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = finalize;
}

RfConfig *rf_config_new(const char *config_path)
{
	g_autoptr(GError) error = NULL;
	RfConfig *this = g_object_new(RF_TYPE_CONFIG, NULL);
	g_key_file_load_from_file(this->f, config_path, G_KEY_FILE_NONE, &error);
	if (error != NULL)
		g_warning(
			"Failed to load configuration from %s, will use default values!",
			config_path
		);
	return this;
}

static inline char *get_string(GKeyFile *f, const char *group, const char *key)
{
	g_autoptr(GError) error = NULL;
	char *value = g_key_file_get_string(f, group, key, &error);
	if (error != NULL || value == NULL || value[0] == '\0') {
		g_clear_pointer(&value, g_free);
		return NULL;
	}
	return value;
}

static inline int get_integer(
	GKeyFile *f,
	const char *group,
	const char *key,
	const int default_value
)
{
	g_autoptr(GError) error = NULL;
	int value = g_key_file_get_integer(f, group, key, &error);
	return error == NULL ? value : default_value;
}

static inline bool get_boolean(
	GKeyFile *f,
	const char *group,
	const char *key,
	const bool default_value
)
{
	g_autoptr(GError) error = NULL;
	int value = g_key_file_get_boolean(f, group, key, &error);
	return error == NULL ? value : default_value;
}

char *rf_config_get_card_path(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autofree char *card =
		get_string(this->f, RF_CONFIG_GROUP_REFRAME, "card");
	if (card == NULL)
		return NULL;
	return g_build_filename(G_DIR_SEPARATOR_S "dev", "dri", card, NULL);
}

char *rf_config_get_connector(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_REFRAME, "connector");
}

unsigned int rf_config_get_rotation(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	unsigned int rotation =
		get_integer(this->f, RF_CONFIG_GROUP_REFRAME, "rotation", 0);
	if (rotation % 90 != 0) {
		g_warning(
			"Got invalid monitor rotation angle %u, valid angles are clockwise 0, 90, 180, 270.",
			rotation
		);
		rotation = rotation / 90 * 90;
	}
	return rotation % 360;
}

unsigned int rf_config_get_desktop_width(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	return get_integer(this->f, RF_CONFIG_GROUP_REFRAME, "desktop-width", 0);
}

unsigned int rf_config_get_desktop_height(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	return get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "desktop-height", 0
	);
}

int rf_config_get_monitor_x(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	return get_integer(this->f, RF_CONFIG_GROUP_REFRAME, "monitor-x", 0);
}

int rf_config_get_monitor_y(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	return get_integer(this->f, RF_CONFIG_GROUP_REFRAME, "monitor-y", 0);
}

unsigned int rf_config_get_default_width(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	return get_integer(this->f, RF_CONFIG_GROUP_REFRAME, "default-width", 0);
}

unsigned int rf_config_get_default_height(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	return get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "default-height", 0
	);
}

bool rf_config_get_resize(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	return get_boolean(this->f, RF_CONFIG_GROUP_REFRAME, "resize", true);
}

bool rf_config_get_share(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	return get_boolean(this->f, RF_CONFIG_GROUP_REFRAME, "share", true);
}

bool rf_config_get_cursor(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	return get_boolean(this->f, RF_CONFIG_GROUP_REFRAME, "cursor", true);
}

bool rf_config_get_wakeup(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	return get_boolean(this->f, RF_CONFIG_GROUP_REFRAME, "wakeup", true);
}

enum rf_wakeup_device rf_config_get_wakeup_device(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), RF_WAKEUP_DEVICE_KEYBOARD);

	g_autofree char *wakeup_device =
		get_string(this->f, RF_CONFIG_GROUP_REFRAME, "wakeup-device");
	if (g_strcmp0(wakeup_device, "pointer") == 0)
		return RF_WAKEUP_DEVICE_POINTER;
	return RF_WAKEUP_DEVICE_KEYBOARD;
}

enum rf_damage_type rf_config_get_damage(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), RF_DAMAGE_TYPE_CPU);

	g_autoptr(GError) error = NULL;
	g_autofree char *damage = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_REFRAME, "damage", &error
	);
	// The default implementation is CPU if upgraded from a old version.
	if (error != NULL || damage == NULL)
		return RF_DAMAGE_TYPE_CPU;
	if (g_strcmp0(damage, "gpu") == 0)
		return RF_DAMAGE_TYPE_GPU;
	if (g_strcmp0(damage, "cpu") == 0)
		return RF_DAMAGE_TYPE_CPU;
	// Empty string or others means dumb.
	return RF_DAMAGE_TYPE_DUMB;
}

unsigned int rf_config_get_fps(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 30);

	return get_integer(this->f, RF_CONFIG_GROUP_REFRAME, "fps", 30);
}

char *rf_config_get_xkb_rules(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_XKB, "rules");
}

char *rf_config_get_xkb_model(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_XKB, "model");
}

char *rf_config_get_xkb_layout(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_XKB, "layout");
}

char *rf_config_get_xkb_variant(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_XKB, "variant");
}

char *rf_config_get_xkb_options(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_XKB, "options");
}

char **rf_config_get_vnc_ip_list(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char **ips = g_key_file_get_string_list(
		this->f, RF_CONFIG_GROUP_VNC, "ip", NULL, &error
	);
	if (error != NULL || ips == NULL || ips[0] == NULL) {
		g_clear_pointer(&ips, g_strfreev);
		return NULL;
	}
	for (int i = 0; ips[i] != NULL; ++i) {
		// GKeyFile uses `;` as seperator and does not handle spaces.
		g_strstrip(ips[i]);
		g_debug("VNC: ips[%d]='%s'", i, ips[i]);
	}
	return ips;
}

unsigned int rf_config_get_vnc_port(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 5933);

	g_autoptr(GError) error = NULL;
	unsigned int port = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_VNC, "port", &error
	);
	// Backward compatibility.
	if (error != NULL) {
		g_clear_pointer(&error, g_error_free);
		g_warning(
			"Please move port to the new [vnc] group to adapt to the updated configuration format."
		);
		port = g_key_file_get_integer(
			this->f, RF_CONFIG_GROUP_REFRAME, "port", &error
		);
	}
	if (error != NULL)
		return 5933;
	return port;
}

char *rf_config_get_vnc_password(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *password = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_VNC, "password", &error
	);
	// Backward compatibility.
	if (error != NULL) {
		g_clear_pointer(&error, g_error_free);
		g_clear_pointer(&password, g_free);
		g_warning(
			"Please move password to the new [vnc] group to adapt to the updated configuration format."
		);
		password = g_key_file_get_string(
			this->f, RF_CONFIG_GROUP_REFRAME, "password", &error
		);
	}
	if (error != NULL || password == NULL || password[0] == '\0') {
		g_clear_pointer(&password, g_free);
		return NULL;
	}
	return password;
}

enum rf_vnc_backend rf_config_get_vnc_backend(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), RF_VNC_BACKEND_LIBVNCSERVER);

	g_autoptr(GError) error = NULL;
	g_autofree char *backend = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_VNC, "backend", &error
	);
	// Backward compatibility.
	if (error != NULL) {
		g_clear_pointer(&error, g_error_free);
		g_clear_pointer(&backend, g_free);
		g_warning(
			"Please rename type in the [vnc] group to backend to adapt to the updated configuration format."
		);
		backend = g_key_file_get_string(
			this->f, RF_CONFIG_GROUP_VNC, "type", &error
		);
	}
	if (error != NULL || backend == NULL)
		return RF_VNC_BACKEND_LIBVNCSERVER;
	if (g_strcmp0(backend, "neatvnc") == 0)
		return RF_VNC_BACKEND_NEATVNC;
	return RF_VNC_BACKEND_LIBVNCSERVER;
}

char *rf_config_get_neatvnc_username(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(this->f, RF_CONFIG_GROUP_NEATVNC, "username");
}

bool rf_config_get_neatvnc_allow_broken_crypto(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), false);

	return get_boolean(
		this->f, RF_CONFIG_GROUP_NEATVNC, "allow-broken-crypto", false
	);
}

char *rf_config_get_neatvnc_rsa_private_key_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "rsa-private-key-file"
	);
}

char *rf_config_get_neatvnc_tls_private_key_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "tls-private-key-file"
	);
}

char *rf_config_get_neatvnc_tls_certificate_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	return get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "tls-certificate-file"
	);
}
