#include "rf-common.h"
#include "rf-config.h"

struct _RfConfig {
	GObject parent_object;
	GKeyFile *f;
};
G_DEFINE_TYPE(RfConfig, rf_config, G_TYPE_OBJECT)

#define RF_CONFIG_GROUP "reframe"

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
	RfConfig *this = g_object_new(RF_TYPE_CONFIG, NULL);
	g_key_file_load_from_file(this->f, config_path, G_KEY_FILE_NONE, NULL);
	return this;
}

char *rf_config_get_card_path(RfConfig *this)
{
	g_autoptr(GError) error = NULL;
	char *card =
		g_key_file_get_string(this->f, RF_CONFIG_GROUP, "card", &error);
	if (error != NULL)
		return g_strdup("/dev/dri/card0");
	char *card_path = g_strdup_printf("/dev/dri/%s", card);
	g_free(card);
	return card_path;
}

char *rf_config_get_connector(RfConfig *this)
{
	g_autoptr(GError) error = NULL;
	char *connector = g_key_file_get_string(this->f, RF_CONFIG_GROUP,
						"connector", &error);
	if (error != NULL)
		return g_strdup("eDP-1");
	return connector;
}

unsigned int rf_config_get_fps(RfConfig *this)
{
	g_autoptr(GError) error = NULL;
	unsigned int fps =
		g_key_file_get_integer(this->f, RF_CONFIG_GROUP, "fps", &error);
	if (error != NULL)
		return 30;
	return fps;
}

unsigned int rf_config_get_port(RfConfig *this)
{
	g_autoptr(GError) error = NULL;
	unsigned int port = g_key_file_get_integer(this->f, RF_CONFIG_GROUP,
						   "port", &error);
	if (error != NULL)
		return 5933;
	return port;
}
