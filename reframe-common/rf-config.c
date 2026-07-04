#include <glib.h>

#include "rf-config.h"

struct _RfConfig {
	GObject parent_object;
	GKeyFile *f;
};
G_DEFINE_TYPE(RfConfig, rf_config, G_TYPE_OBJECT)

#define RF_CONFIG_GROUP_REFRAME "reframe"
#define RF_CONFIG_GROUP_REMOTE "remote"
#define RF_CONFIG_GROUP_VNC "vnc"
#define RF_CONFIG_GROUP_LIBVNCSERVER "libvncserver"
#define RF_CONFIG_GROUP_NEATVNC "neatvnc"
#define RF_CONFIG_GROUP_RDP "rdp"

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

char *rf_config_get_card_path(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	g_autofree char *card = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_REFRAME, "card", &error
	);
	if (error != NULL || card == NULL || card[0] == '\0') {
		g_clear_pointer(&card, g_free);
		return NULL;
	}
	char *card_path =
		g_build_filename(G_DIR_SEPARATOR_S "dev", "dri", card, NULL);
	return card_path;
}

char *rf_config_get_connector(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *connector = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_REFRAME, "connector", &error
	);
	if (error != NULL || connector == NULL || connector[0] == '\0') {
		g_clear_pointer(&connector, g_free);
		return NULL;
	}
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

bool rf_config_get_resize(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	int resize = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_REFRAME, "resize", &error
	);
	if (error != NULL)
		return true;
	return resize;
}

bool rf_config_get_cursor(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	int cursor = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_REFRAME, "cursor", &error
	);
	if (error != NULL)
		return true;
	return cursor;
}

bool rf_config_get_wakeup(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	int wakeup = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_REFRAME, "wakeup", &error
	);
	if (error != NULL)
		return true;
	return wakeup;
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

	g_autoptr(GError) error = NULL;
	unsigned int fps = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_REFRAME, "fps", &error
	);
	if (error != NULL)
		return 30;
	return fps;
}

char *rf_config_get_remote_protocol(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *protocol = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_REMOTE, "protocol", &error
	);
	if (error != NULL || protocol == NULL || protocol[0] == '\0') {
		g_clear_pointer(&protocol, g_free);
		return g_strdup("vnc");
	}
	g_strstrip(protocol);
	if (protocol[0] == '\0') {
		g_clear_pointer(&protocol, g_free);
		return g_strdup("vnc");
	}
	return protocol;
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

char *rf_config_get_neatvnc_username(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *username = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "username", &error
	);
	if (error != NULL || username == NULL || username[0] == '\0')
		return NULL;
	return username;
}

bool rf_config_get_neatvnc_allow_broken_crypto(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	int allow_broken_crypto = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_NEATVNC, "allow-broken-crypto", &error
	);
	if (error != NULL)
		return false;
	return allow_broken_crypto;
}

char *rf_config_get_neatvnc_rsa_private_key_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *key_file = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "rsa-private-key-file", &error
	);
	if (error != NULL || key_file == NULL || key_file[0] == '\0')
		return NULL;
	return key_file;
}

char *rf_config_get_neatvnc_tls_private_key_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *key_file = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "tls-private-key-file", &error
	);
	if (error != NULL || key_file == NULL || key_file[0] == '\0')
		return NULL;
	return key_file;
}

char *rf_config_get_neatvnc_tls_certificate_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *certificate_file = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_NEATVNC, "tls-certificate-file", &error
	);
	if (error != NULL || certificate_file == NULL ||
	    certificate_file[0] == '\0')
		return NULL;
	return certificate_file;
}

char **rf_config_get_rdp_ip_list(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char **ips = g_key_file_get_string_list(
		this->f, RF_CONFIG_GROUP_RDP, "ip", NULL, &error
	);
	if (error != NULL || ips == NULL || ips[0] == NULL) {
		g_clear_pointer(&ips, g_strfreev);
		return NULL;
	}
	for (int i = 0; ips[i] != NULL; ++i) {
		g_strstrip(ips[i]);
		g_debug("RDP: ips[%d]='%s'", i, ips[i]);
	}
	return ips;
}

unsigned int rf_config_get_rdp_port(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 3389);

	g_autoptr(GError) error = NULL;
	unsigned int port = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_RDP, "port", &error
	);
	if (error != NULL)
		return 3389;
	return port;
}

char *rf_config_get_rdp_tls_private_key_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *key_file = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "tls-private-key-file", &error
	);
	if (error != NULL || key_file == NULL || key_file[0] == '\0') {
		g_clear_pointer(&key_file, g_free);
		return NULL;
	}
	return key_file;
}

char *rf_config_get_rdp_tls_certificate_file(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *certificate_file = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "tls-certificate-file", &error
	);
	if (error != NULL || certificate_file == NULL ||
	    certificate_file[0] == '\0') {
		g_clear_pointer(&certificate_file, g_free);
		return NULL;
	}
	return certificate_file;
}

char *rf_config_get_rdp_username(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *username = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "username", &error
	);
	if (error != NULL || username == NULL || username[0] == '\0') {
		g_clear_pointer(&username, g_free);
		return NULL;
	}
	return username;
}

char *rf_config_get_rdp_domain(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *domain =
		g_key_file_get_string(this->f, RF_CONFIG_GROUP_RDP, "domain", &error);
	if (error != NULL || domain == NULL || domain[0] == '\0') {
		g_clear_pointer(&domain, g_free);
		return NULL;
	}
	return domain;
}

char *rf_config_get_rdp_password(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *password = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "password", &error
	);
	if (error != NULL || password == NULL || password[0] == '\0') {
		g_clear_pointer(&password, g_free);
		return NULL;
	}
	return password;
}

bool rf_config_get_rdp_nla(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), false);

	g_autoptr(GError) error = NULL;
	int nla = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_RDP, "nla", &error
	);
	if (error != NULL)
		return false;
	return nla;
}

char *rf_config_get_rdp_graphics(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *graphics = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "graphics", &error
	);
	if (error != NULL || graphics == NULL || graphics[0] == '\0') {
		g_clear_pointer(&graphics, g_free);
		return g_strdup("auto");
	}
	g_strstrip(graphics);
	if (graphics[0] == '\0') {
		g_clear_pointer(&graphics, g_free);
		return g_strdup("auto");
	}
	return graphics;
}

bool rf_config_get_rdp_clipboard(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), true);

	g_autoptr(GError) error = NULL;
	int clipboard = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_RDP, "clipboard", &error
	);
	if (error != NULL)
		return true;
	return clipboard;
}

char *rf_config_get_rdp_avc_encoder(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *encoder = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "avc-encoder", &error
	);
	if (error != NULL || encoder == NULL || encoder[0] == '\0') {
		g_clear_pointer(&encoder, g_free);
		return g_strdup("auto");
	}
	g_strstrip(encoder);
	if (encoder[0] == '\0') {
		g_clear_pointer(&encoder, g_free);
		return g_strdup("auto");
	}
	return encoder;
}

int rf_config_get_rdp_video_quality(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), RF_CONFIG_RDP_VIDEO_QUALITY_AUTO);

	g_autoptr(GError) error = NULL;
	g_autofree char *quality = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "video-quality", &error
	);
	if (error != NULL || quality == NULL)
		return RF_CONFIG_RDP_VIDEO_QUALITY_AUTO;

	g_strstrip(quality);
	if (quality[0] == '\0' || g_strcmp0(quality, "auto") == 0)
		return RF_CONFIG_RDP_VIDEO_QUALITY_AUTO;

	char *end = NULL;
	const gint64 value = g_ascii_strtoll(quality, &end, 10);
	if (end == quality || end == NULL || end[0] != '\0' ||
	    value < 0 || value > 3) {
		g_warning(
			"RDP: Invalid video-quality '%s', using auto.",
			quality
		);
		return RF_CONFIG_RDP_VIDEO_QUALITY_AUTO;
	}
	return (int)value;
}

unsigned int rf_config_get_rdp_video_quality_max(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 3);

	g_autoptr(GError) error = NULL;
	const int quality = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_RDP, "video-quality-max", &error
	);
	if (error != NULL)
		return 3;
	if (quality < 0 || quality > 3) {
		g_warning(
			"RDP: Invalid video-quality-max %d, using 3.",
			quality
		);
		return 3;
	}
	return (unsigned int)quality;
}

unsigned int rf_config_get_rdp_target_bandwidth_mbps(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 20);

	g_autoptr(GError) error = NULL;
	const int bandwidth = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_RDP, "target-bandwidth-mbps", &error
	);
	if (error != NULL)
		return 20;
	if (bandwidth <= 0) {
		g_warning(
			"RDP: Invalid target-bandwidth-mbps %d, using 20.",
			bandwidth
		);
		return 20;
	}
	return (unsigned int)bandwidth;
}

bool rf_config_get_rdp_audio(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), false);

	g_autoptr(GError) error = NULL;
	const int audio = g_key_file_get_boolean(
		this->f, RF_CONFIG_GROUP_RDP, "audio", &error
	);
	if (error != NULL)
		return false;
	return audio;
}

unsigned int rf_config_get_rdp_audio_sample_rate(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 48000);

	g_autoptr(GError) error = NULL;
	const int sample_rate = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_RDP, "audio-sample-rate", &error
	);
	if (error != NULL)
		return 48000;
	if (sample_rate != 44100 && sample_rate != 48000) {
		g_warning(
			"RDP: Invalid audio-sample-rate %d, using 48000.",
			sample_rate
		);
		return 48000;
	}
	return (unsigned int)sample_rate;
}

unsigned int rf_config_get_rdp_audio_channels(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 2);

	g_autoptr(GError) error = NULL;
	const int channels = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_RDP, "audio-channels", &error
	);
	if (error != NULL)
		return 2;
	if (channels != 1 && channels != 2) {
		g_warning("RDP: Invalid audio-channels %d, using 2.", channels);
		return 2;
	}
	return (unsigned int)channels;
}

unsigned int rf_config_get_rdp_audio_frame_ms(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), 20);

	g_autoptr(GError) error = NULL;
	const int frame_ms = g_key_file_get_integer(
		this->f, RF_CONFIG_GROUP_RDP, "audio-frame-ms", &error
	);
	if (error != NULL)
		return 20;
	if (frame_ms != 10 && frame_ms != 20 && frame_ms != 40) {
		g_warning("RDP: Invalid audio-frame-ms %d, using 20.", frame_ms);
		return 20;
	}
	return (unsigned int)frame_ms;
}

char *rf_config_get_rdp_audio_codec(RfConfig *this)
{
	g_return_val_if_fail(RF_IS_CONFIG(this), NULL);

	g_autoptr(GError) error = NULL;
	char *codec = g_key_file_get_string(
		this->f, RF_CONFIG_GROUP_RDP, "audio-codec", &error
	);
	if (error != NULL || codec == NULL) {
		g_clear_pointer(&codec, g_free);
		return g_strdup("auto");
	}
	g_strstrip(codec);
	if (g_strcmp0(codec, "auto") == 0 ||
	    g_strcmp0(codec, "pcm") == 0 ||
	    g_strcmp0(codec, "adpcm") == 0)
		return codec;

	g_warning("RDP: Invalid audio-codec '%s', using auto.", codec);
	g_clear_pointer(&codec, g_free);
	return g_strdup("auto");
}
