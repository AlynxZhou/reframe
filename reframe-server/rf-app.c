#include "config.h"
#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-app.h"
#include "rf-streamer.h"
#include "rf-converter.h"
#include "rf-vnc-server.h"
#include "rf-win.h"

struct _RfApp {
	GApplication parent_instance;
	char *socket_path;
	char *config_path;
	RfConfig *config;
	RfWin *win;
	RfStreamer *streamer;
	RfConverter *converter;
	RfVNCServer *vnc;
	unsigned int width;
	unsigned int height;
	bool debug_win_opt;
};
G_DEFINE_TYPE(RfApp, rf_app, G_TYPE_APPLICATION)

static void _on_size_request(RfVNCServer *v, int width, int height,
			     gpointer data)
{
	RfApp *this = data;
	g_debug("Get new size request %dx%d.", width, height);
	this->width = width;
	this->height = height;
}

static void _on_frame(RfStreamer *s, const RfBuffer *b, gpointer data)
{
	RfApp *this = data;
	unsigned char *buf = rf_converter_convert(this->converter, b,
						  this->width, this->height);
	rf_vnc_server_update(this->vnc, buf);
	if (this->debug_win_opt)
		rf_win_draw(this->win, buf, this->width, this->height);
	g_free(buf);
}

static void _activate(GApplication *g_app)
{
	// RfApp *this = RF_APP(g_app);

	G_APPLICATION_CLASS(rf_app_parent_class)->activate(g_app);
}

static void _startup(GApplication *g_app)
{
	RfApp *this = RF_APP(g_app);

	this->config = rf_config_new(this->config_path);

	this->streamer = rf_streamer_new(this->config);
	rf_streamer_set_socket_path(this->streamer, this->socket_path);
	this->converter = rf_converter_new();
	this->vnc = rf_vnc_server_new(this->config);

	g_signal_connect(this->streamer, "frame", G_CALLBACK(_on_frame), this);
	g_signal_connect_swapped(this->vnc, "first-client",
				 G_CALLBACK(rf_streamer_start), this->streamer);
	g_signal_connect_swapped(this->vnc, "last-client",
				 G_CALLBACK(rf_streamer_stop), this->streamer);
	g_signal_connect(this->vnc, "size-request",
			 G_CALLBACK(_on_size_request), this);

	rf_vnc_server_start(this->vnc);

	if (this->debug_win_opt) {
		gtk_init();
		this->win = rf_win_new();
		g_signal_connect_swapped(this->win, "map",
					 G_CALLBACK(rf_streamer_start),
					 this->streamer);
		g_signal_connect_swapped(this->win, "unmap",
					 G_CALLBACK(rf_streamer_stop),
					 this->streamer);
		gtk_window_present(GTK_WINDOW(this->win));
	}

	g_application_hold(g_app);

	G_APPLICATION_CLASS(rf_app_parent_class)->startup(g_app);
}

static void _shutdown(GApplication *g_app)
{
	RfApp *this = RF_APP(g_app);

	if (this->win != NULL) {
		gtk_window_destroy(GTK_WINDOW(this->win));
		this->win = NULL;
	}

	G_APPLICATION_CLASS(rf_app_parent_class)->shutdown(g_app);
}

// See <https://docs.gtk.org/gio/signal.Application.handle-local-options.html>.
static int _handle_local_options(GApplication *g_app, GVariantDict *options)
{
	// RfApp *this = RF_APP(g_app);

	if (g_variant_dict_contains(options, "version")) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	return -1;
}

static void rf_app_init(RfApp *this)
{
	this->config = NULL;
	this->win = NULL;
	this->streamer = NULL;
	this->converter = NULL;
	this->vnc = NULL;
	this->config_path = "/etc/reframe/default.conf";
	this->socket_path = "/tmp/reframe.sock";
	this->width = RF_DEFAULT_WIDTH;
	this->height = RF_DEFAULT_HEIGHT;
	this->debug_win_opt = false;

	// g_set_application_name("ReFrame");

	const GOptionEntry options[] = {
		{ "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL,
		  "Display version and exit.", NULL },
		{ "socket", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
		  &this->socket_path, "Socket path of streamer.", "SOCKET" },
		{ "config", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
		  &this->config_path, "Configuration file path.", "PATH" },
		{ "debug-win", 'w', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
		  &this->debug_win_opt, "Show debug window.", NULL },
		{ NULL, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, NULL,
		  NULL }
	};

	g_application_add_main_option_entries(G_APPLICATION(this), options);
}

static void rf_app_class_init(RfAppClass *klass)
{
	GApplicationClass *g_app_class = G_APPLICATION_CLASS(klass);

	g_app_class->activate = _activate;
	g_app_class->startup = _startup;
	g_app_class->shutdown = _shutdown;

	g_app_class->handle_local_options = _handle_local_options;
}

RfApp *rf_app_new(void)
{
	return g_object_new(RF_TYPE_APP, "application-id", "one.alynx.reframe",
			    NULL);
}
