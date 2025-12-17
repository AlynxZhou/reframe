#include "config.h"
#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-streamer.h"
#include "rf-converter.h"
#include "rf-vnc-server.h"
#include "rf-lvnc-server.h"
#ifdef HAVE_NEATVNC
#	include "rf-nvnc-server.h"
#endif

struct _this {
	GMainLoop *main_loop;
	RfConfig *config;
	RfStreamer *streamer;
	RfConverter *converter;
	RfVNCServer *vnc;
	unsigned int width;
	unsigned int height;
	unsigned int rotation;
};

static void
_on_resize_event(RfVNCServer *v, int width, int height, gpointer data)
{
	struct _this *this = data;

	this->width = width;
	this->height = height;
}

static void
_on_frame(RfStreamer *s, size_t length, const RfBuffer *bufs, gpointer data)
{
	struct _this *this = data;

	const RfBuffer *primary = &bufs[0];
	if (this->width == 0 || this->height == 0) {
		if (this->rotation % 180 == 0) {
			this->width = primary->md.crtc_w;
			this->height = primary->md.crtc_h;
		} else {
			this->width = primary->md.crtc_h;
			this->height = primary->md.crtc_w;
		}
	}

	if (!rf_converter_is_running(this->converter))
		if (rf_converter_start(this->converter) < 0)
			rf_vnc_server_flush(this->vnc);

	g_autoptr(GByteArray) buf = rf_converter_convert(
		this->converter, length, bufs, this->width, this->height
	);
	if (buf != NULL)
		rf_vnc_server_update(this->vnc, buf, this->width, this->height);
}

static void _on_first_client(RfVNCServer *v, gpointer data)
{
	struct _this *this = data;

	this->rotation = rf_config_get_rotation(this->config);
	this->width = rf_config_get_default_width(this->config);
	this->height = rf_config_get_default_height(this->config);

	if (rf_streamer_start(this->streamer) < 0)
		rf_vnc_server_flush(this->vnc);
}

static void _on_last_client(RfVNCServer *v, gpointer data)
{
	struct _this *this = data;

	rf_streamer_stop(this->streamer);
	rf_converter_stop(this->converter);
}

int main(int argc, char *argv[])
{
	g_autofree char *config_path = NULL;
	g_autofree char *socket_path = NULL;
	// `gboolean` is `int`, but `bool` may be `char`! Passing `bool` pointer
	// to `GOptionContext` leads into overflow!
	gboolean version = FALSE;
	g_autofree char **args = g_strdupv(argv);
	g_autoptr(GError) error = NULL;

	GOptionEntry options[] = { { "version",
				     'v',
				     G_OPTION_FLAG_NONE,
				     G_OPTION_ARG_NONE,
				     &version,
				     "Display version and exit.",
				     NULL },
				   { "socket",
				     's',
				     G_OPTION_FLAG_NONE,
				     G_OPTION_ARG_FILENAME,
				     &socket_path,
				     "Socket path to communicate.",
				     "SOCKET" },
				   { "config",
				     'c',
				     G_OPTION_FLAG_NONE,
				     G_OPTION_ARG_FILENAME,
				     &config_path,
				     "Configuration file path.",
				     "PATH" },
				   { NULL,
				     0,
				     G_OPTION_FLAG_NONE,
				     G_OPTION_ARG_NONE,
				     NULL,
				     NULL,
				     NULL } };
	g_autoptr(GOptionContext)
		context = g_option_context_new(" - ReFrame Server");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse_strv(context, &args, &error)) {
		g_warning("Failed to parse options: %s.", error->message);
		g_clear_pointer(&error, g_error_free);
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	// Use `g_strdup` here to make `g_autofree` happy.
	if (socket_path == NULL)
		socket_path = g_strdup("/tmp/reframe.sock");

	const char *XKB_DEFAULT_LAYOUT = getenv("XKB_DEFAULT_LAYOUT");
	if (XKB_DEFAULT_LAYOUT == 0 || g_strcmp0(XKB_DEFAULT_LAYOUT, "") == 0) {
		g_message(
			"XKB_DEFAULT_LAYOUT is empty, using US layout by default."
		);
		setenv("XKB_DEFAULT_LAYOUT", "us", 1);
	}

	g_autofree struct _this *this = g_malloc0(sizeof(*this));
	g_message("Using configuration file %s.", config_path);
	this->config = rf_config_new(config_path);
	this->rotation = rf_config_get_rotation(this->config);
	this->width = rf_config_get_default_width(this->config);
	this->height = rf_config_get_default_height(this->config);
	this->streamer = rf_streamer_new(this->config);
	g_message("Using socket %s.", socket_path);
	rf_streamer_set_socket_path(this->streamer, socket_path);
	this->converter = rf_converter_new(this->config);
#ifdef HAVE_NEATVNC
	g_autofree char *type = rf_config_get_vnc_type(this->config);
	g_debug("VNC: Implementation type is %s.", type);
	if (g_strcmp0(type, "neatvnc") == 0)
		this->vnc = rf_nvnc_server_new(this->config);
	else
#endif
		this->vnc = rf_lvnc_server_new(this->config);
	g_signal_connect_swapped(
		this->streamer,
		"stop",
		G_CALLBACK(rf_vnc_server_flush),
		this->vnc
	);
	g_signal_connect_swapped(
		this->streamer,
		"card-path",
		G_CALLBACK(rf_converter_set_card_path),
		this->converter
	);
	g_signal_connect_swapped(
		this->streamer,
		"connector-name",
		G_CALLBACK(rf_vnc_server_set_desktop_name),
		this->vnc
	);
	g_signal_connect(this->streamer, "frame", G_CALLBACK(_on_frame), this);
	g_signal_connect(
		this->vnc, "first-client", G_CALLBACK(_on_first_client), this
	);
	g_signal_connect(
		this->vnc, "last-client", G_CALLBACK(_on_last_client), this
	);
	g_signal_connect(
		this->vnc, "resize-event", G_CALLBACK(_on_resize_event), this
	);
	g_signal_connect_swapped(
		this->vnc,
		"keyboard-event",
		G_CALLBACK(rf_streamer_send_keyboard_event),
		this->streamer
	);
	g_signal_connect_swapped(
		this->vnc,
		"pointer-event",
		G_CALLBACK(rf_streamer_send_pointer_event),
		this->streamer
	);
	rf_vnc_server_start(this->vnc);

	this->main_loop = g_main_loop_new(NULL, FALSE);
	g_main_loop_run(this->main_loop);

	rf_vnc_server_stop(this->vnc);
	g_object_unref(this->vnc);
	g_object_unref(this->converter);
	g_object_unref(this->streamer);
	g_object_unref(this->config);

	return 0;
}
