#include "config.h"
#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-streamer.h"
#include "rf-converter.h"
#include "rf-vnc-server.h"

struct _this {
	GMainLoop *main_loop;
	RfConfig *config;
	RfStreamer *streamer;
	RfConverter *converter;
	RfVNCServer *vnc;
	unsigned int width;
	unsigned int height;
	bool debug_win_opt;
};

static void _on_resize_event(RfVNCServer *v, int width, int height,
			     gpointer data)
{
	struct _this *this = data;

	g_debug("Get new size request: width %d, height %d.", width, height);
	this->width = width;
	this->height = height;
}

static void _on_frame(RfStreamer *s, const RfBuffer *b, gpointer data)
{
	struct _this *this = data;

	unsigned char *buf = rf_converter_convert(this->converter, b,
						  this->width, this->height);
	rf_vnc_server_update(this->vnc, buf);
	g_free(buf);
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
	g_autofree struct _this *this = g_malloc0(sizeof(*this));

	GOptionEntry options[] = {
		{ "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &version,
		  "Display version and exit.", NULL },
		{ "socket", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
		  &socket_path, "Socket path of streamer.", "SOCKET" },
		{ "config", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
		  &config_path, "Configuration file path.", "PATH" },
		{ NULL, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, NULL,
		  NULL }
	};
	g_autoptr(GOptionContext) context = g_option_context_new(" - ReFrame streamer");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse_strv(context, &args, &error)) {
		g_error("Failed to parse options.");
		return EXIT_FAILURE;
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	// Use `g_strdup` here to make `g_autofree` happy.
	if (socket_path == NULL)
		socket_path = g_strdup("/tmp/reframe.sock");

	this->config = rf_config_new(config_path);

	this->streamer = rf_streamer_new(this->config);
	rf_streamer_set_socket_path(this->streamer, socket_path);
	this->converter = rf_converter_new();
	this->vnc = rf_vnc_server_new(this->config);

	g_signal_connect(this->streamer, "frame", G_CALLBACK(_on_frame), this);
	g_signal_connect_swapped(this->vnc, "first-client",
				 G_CALLBACK(rf_streamer_start), this->streamer);
	g_signal_connect_swapped(this->vnc, "last-client",
				 G_CALLBACK(rf_streamer_stop), this->streamer);
	g_signal_connect(this->vnc, "resize-event",
			 G_CALLBACK(_on_resize_event), this);
	g_signal_connect_swapped(this->vnc, "keyboard-event",
				 G_CALLBACK(rf_streamer_send_keyboard_event), this->streamer);
	g_signal_connect_swapped(this->vnc, "pointer-event",
			 G_CALLBACK(rf_streamer_send_pointer_event), this->streamer);

	rf_vnc_server_start(this->vnc);

	this->main_loop = g_main_loop_new(NULL, FALSE);

	g_main_loop_run(this->main_loop);

	return 0;
}
