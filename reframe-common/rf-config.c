#include "glib.h"
#include "rf-common.h"
#include "rf-config.h"

struct _RfConfig {
	GObject parent_object;
	GKeyFile *f;
};
G_DEFINE_TYPE(RfConfig, rf_config, G_TYPE_OBJECT)

#define RF_CONFIG_GROUP_REFRAME "reframe"
#define RF_CONFIG_GROUP_VNC "vnc"

static void _finalize(GObject *o)
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

	o_class->finalize = _finalize;
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

char *rf_config_get_card_path(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *card = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_REFRAME, "card", &error
	);
	if (error != NULL || card == NULL || card[0] == '\0')
		return NULL;
	char *card_path = g_strdup_printf("/dev/dri/%s", card);
	g_free(card);
	return card_path;
}

char *rf_config_get_connector(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *connector = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_REFRAME, "connector", &error
	);
	if (error != NULL || connector == NULL || connector[0] == '\0')
		return NULL;
	return connector;
}

unsigned int rf_config_get_rotation(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	g_autoptr(GError) error = NULL;
	unsigned int rotation = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "rotation", &error
	);
	if (error != NULL)
		return 0;
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

	g_autoptr(GError) error = NULL;
	unsigned int desktop_width = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "desktop-width", &error
	);
	if (error != NULL)
		return 0;
	return desktop_width;
}

unsigned int rf_config_get_desktop_height(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	g_autoptr(GError) error = NULL;
	unsigned int desktop_height = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "desktop-height", &error
	);
	if (error != NULL)
		return 0;
	return desktop_height;
}

int rf_config_get_monitor_x(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	g_autoptr(GError) error = NULL;
	int monitor_x = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "monitor-x", &error
	);
	if (error != NULL)
		return 0;
	return monitor_x;
}

int rf_config_get_monitor_y(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	g_autoptr(GError) error = NULL;
	int monitor_y = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "monitor-y", &error
	);
	if (error != NULL)
		return 0;
	return monitor_y;
}

unsigned int rf_config_get_default_width(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	g_autoptr(GError) error = NULL;
	unsigned int default_width = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "default-width", &error
	);
	if (error != NULL)
		return 0;
	return default_width;
}

unsigned int rf_config_get_default_height(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 0);

	g_autoptr(GError) error = NULL;
	unsigned int default_height = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "default-height", &error
	);
	if (error != NULL)
		return 0;
	return default_height;
}

bool rf_config_get_cursor(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	gboolean cursor = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_REFRAME, "cursor", &error
	);
	if (error != NULL)
		return true;
	return cursor ? true : false;
}

bool rf_config_get_wakeup(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	gboolean wakeup = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_REFRAME, "wakeup", &error
	);
	if (error != NULL)
		return true;
	return wakeup ? true : false;
}

unsigned int rf_config_get_fps(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 30);

	g_autoptr(GError) error = NULL;
	unsigned int fps = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "fps", &error
	);
	if (error != NULL)
		return 30;
	return fps;
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
	if (error != NULL || password == NULL || password[0] == '\0')
		return NULL;
	return password;
}

char *rf_config_get_vnc_type(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *type = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_VNC, "type", &error
	);
	if (error != NULL || type == NULL || type[0] == '\0')
		return NULL;
	return type;
}
