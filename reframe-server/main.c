#include <locale.h>
#include <glib-unix.h>
#include <gmodule.h>

#include "config.h"
#include "rf-common.h"
#include "rf-streamer.h"
#include "rf-session.h"
#include "rf-converter.h"
#include "rf-vnc-server.h"

struct this {
	GMainLoop *main_loop;
	RfConfig *config;
	RfStreamer *streamer;
	RfSession *session;
	RfConverter *converter;
	RfVNCServer *vnc;
	unsigned int width;
	unsigned int height;
	unsigned int rotation;
	bool skip_damage;
};

static void on_resize_event(RfVNCServer *v, int width, int height, void *data)
{
	struct this *this = data;

	this->width = width;
	this->height = height;
}

static void
on_frame(RfStreamer *s, size_t length, const struct rf_buffer *bufs, void *data)
{
	struct this *this = data;

	const struct rf_buffer *primary = &bufs[0];
	if (this->width == 0 || this->height == 0) {
		if (this->rotation % 180 == 0) {
			this->width = primary->md.crtc_w;
			this->height = primary->md.crtc_h;
		} else {
			this->width = primary->md.crtc_h;
			this->height = primary->md.crtc_w;
		}
	}

	if (!rf_converter_is_running(this->converter)) {
		if (rf_converter_start(this->converter) < 0) {
			rf_vnc_server_flush(this->vnc);
			return;
		}
	}

	struct rf_rect damage;
	GByteArray *buf = rf_converter_convert(
		this->converter,
		length,
		bufs,
		this->width,
		this->height,
		this->skip_damage ? NULL : &damage
	);
	if (buf != NULL)
		rf_vnc_server_update(
			this->vnc,
			buf,
			this->width,
			this->height,
			this->skip_damage ? NULL : &damage
		);
}

static void on_first_client(RfVNCServer *v, void *data)
{
	struct this *this = data;

	this->rotation = rf_config_get_rotation(this->config);
	this->width = rf_config_get_default_width(this->config);
	this->height = rf_config_get_default_height(this->config);

	if (rf_streamer_start(this->streamer) < 0)
		rf_vnc_server_flush(this->vnc);
}

static void on_last_client(RfVNCServer *v, void *data)
{
	struct this *this = data;

	rf_converter_stop(this->converter);
	rf_streamer_stop(this->streamer);
}

static int on_sigint(void *data)
{
	struct this *this = data;

	g_main_loop_quit(this->main_loop);

	return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");

	g_autofree char *config_path = NULL;
	g_autofree char *socket_path = NULL;
	g_autofree char *session_socket_path = NULL;
	// `gboolean` is `int`, but `bool` may be `char`! Passing `bool` pointer
	// to `GOptionContext` leads into overflow!
	int version = false;
	int skip_damage = false;
	g_autoptr(GError) error = NULL;

	GOptionEntry options[] = {
		{ "version",
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
		  "Streamer socket path to communicate.",
		  "SOCKET" },
		{ "session-socket",
		  'S',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_FILENAME,
		  &session_socket_path,
		  "Session socket path to communicate.",
		  "SOCKET" },
		{ "config",
		  'c',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_FILENAME,
		  &config_path,
		  "Configuration file path.",
		  "PATH" },
		{ "skip-damage",
		  'D',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  &skip_damage,
		  "Skip damage region tracking and always update the whole frame buffer (debug purpose).",
		  NULL },
		{ NULL,
		  0,
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  NULL,
		  NULL,
		  NULL }
	};
	g_autoptr(GOptionContext)
		context = g_option_context_new(" - ReFrame Server");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_warning("Failed to parse options: %s.", error->message);
		g_clear_pointer(&error, g_error_free);
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	if (socket_path == NULL)
		socket_path = g_strdup("/tmp/reframe/reframe.sock");
	// We ensure the default dir, user ensure the argument dir.
	if (session_socket_path == NULL) {
		g_mkdir("/tmp/reframe-session", 0755);
		rf_set_group("/tmp/reframe-session");
		session_socket_path =
			g_strdup("/tmp/reframe-session/reframe-session.sock");
	}

	g_message(
		"Skip damage region tracking mode is %s.",
		skip_damage ? "enabled" : "disabled"
	);
	g_message("Using configuration file %s.", config_path);
	g_message("Using socket %s.", socket_path);
	g_message("Using session socket %s.", session_socket_path);

	const char *xkb_default_layout = g_getenv("XKB_DEFAULT_LAYOUT");
	if (xkb_default_layout == NULL || xkb_default_layout[0] == '\0') {
		g_message(
			"XKB_DEFAULT_LAYOUT is empty, using US layout by default."
		);
		g_setenv("XKB_DEFAULT_LAYOUT", "us", true);
	}

	g_autofree struct this *this = g_malloc0(sizeof(*this));
	this->skip_damage = skip_damage;
	this->config = rf_config_new(config_path);
	this->rotation = rf_config_get_rotation(this->config);
	this->width = rf_config_get_default_width(this->config);
	this->height = rf_config_get_default_height(this->config);

	const char *module_name = NULL;
#ifdef HAVE_NEATVNC
	// Only check VNC type when we have choices.
	g_autofree char *type = rf_config_get_vnc_type(this->config);
	g_message("VNC: Implementation is %s.", type);
	if (g_strcmp0(type, "neatvnc") == 0)
		module_name = "lib" PROJECT_NAME "-neatvnc." G_MODULE_SUFFIX;
	else
#endif
		module_name = "lib" PROJECT_NAME
			      "-libvncserver." G_MODULE_SUFFIX;
	g_autofree char *module_path = g_build_filename(
		LIBDIR, PROJECT_NAME, "vnc", module_name, NULL
	);
	GModule *module = g_module_open(
		module_path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL
	);
	if (module == NULL)
		g_error("VNC: Failed to load module %s: %s",
			module_path,
			g_module_error());
	RfVNCServerNewFunc rf_vnc_server_new;
	if (!g_module_symbol(
		    module, "rf_vnc_server_new", (void **)&rf_vnc_server_new
	    ))
		g_error("VNC: Failed to find rf_vnc_server_new symbol in module %s: %s",
			module_path,
			g_module_error());
	this->vnc = rf_vnc_server_new(this->config);

	this->converter = rf_converter_new(this->config);
	this->session = rf_session_new();
	rf_session_set_socket_path(this->session, session_socket_path);
	this->streamer = rf_streamer_new(this->config);
	rf_streamer_set_socket_path(this->streamer, socket_path);
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
	g_signal_connect(this->streamer, "frame", G_CALLBACK(on_frame), this);
	g_signal_connect_swapped(
		this->session,
		"clipboard-text",
		G_CALLBACK(rf_vnc_server_send_clipboard_text),
		this->vnc
	);
	g_signal_connect(
		this->vnc, "first-client", G_CALLBACK(on_first_client), this
	);
	g_signal_connect(
		this->vnc, "last-client", G_CALLBACK(on_last_client), this
	);
	g_signal_connect(
		this->vnc, "resize-event", G_CALLBACK(on_resize_event), this
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
	g_signal_connect_swapped(
		this->vnc,
		"clipboard-text",
		G_CALLBACK(rf_session_send_clipboard_text_msg),
		this->session
	);
	g_signal_connect_swapped(
		this->streamer,
		"start",
		G_CALLBACK(rf_session_start),
		this->session
	);
	g_signal_connect_swapped(
		this->streamer,
		"stop",
		G_CALLBACK(rf_session_stop),
		this->session
	);
	g_signal_connect_swapped(
		this->session,
		"auth",
		G_CALLBACK(rf_streamer_auth),
		this->streamer
	);
	g_signal_connect_swapped(
		this->streamer,
		"auth",
		G_CALLBACK(rf_session_auth),
		this->session
	);
	rf_vnc_server_start(this->vnc);

	this->main_loop = g_main_loop_new(NULL, false);
	g_unix_signal_add(SIGINT, on_sigint, this);
	g_main_loop_run(this->main_loop);
	g_main_loop_unref(this->main_loop);

	rf_vnc_server_stop(this->vnc);
	// Destruction sequence is decided by signal callbacks.
	g_clear_object(&this->streamer);
	g_clear_object(&this->session);
	g_clear_object(&this->converter);
	g_clear_object(&this->vnc);
	g_clear_pointer(&module, g_module_close);
	g_clear_object(&this->config);

	return 0;
}
