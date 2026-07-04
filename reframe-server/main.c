#include <locale.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gmodule.h>

#include "config.h"
#include "rf-common.h"
#include "rf-streamer.h"
#include "rf-session.h"
#include "rf-converter.h"
#include "rf-resize.h"
#include "rf-remote-server.h"

struct this {
	GMainLoop *main_loop;
	RfConfig *config;
	RfStreamer *streamer;
	RfSession *session;
	RfConverter *converter;
	RfRemoteServer *remote;
	unsigned int width;
	unsigned int height;
	unsigned int reference_width;
	unsigned int reference_height;
	struct rf_viewport viewport;
	unsigned int rotation;
	double aspect_ratio;
	int64_t last_slow_pipeline_log_us;
	bool skip_damage;
	bool logged_frame;
};

static void update_viewport(struct this *this)
{
	rf_fit_viewport_to_reference(
		this->width,
		this->height,
		this->reference_width,
		this->reference_height,
		&this->viewport
	);
}

static void on_resize_event(
	RfRemoteServer *r,
	int width,
	int height,
	void *data
)
{
	struct this *this = data;

	this->width = width;
	this->height = height;
	update_viewport(this);
}

static void
on_frame(RfStreamer *s, size_t length, const struct rf_buffer *bufs, void *data)
{
	struct this *this = data;

	const struct rf_buffer *primary = &bufs[0];
	this->reference_width = primary->md.crtc_w;
	this->reference_height = primary->md.crtc_h;
	if (!rf_is_landscape(this->rotation)) {
		this->reference_width = primary->md.crtc_h;
		this->reference_height = primary->md.crtc_w;
	}
	if (this->width == 0 || this->height == 0) {
		this->width = this->reference_width;
		this->height = this->reference_height;
	}
	this->aspect_ratio = (double)primary->md.crtc_w / primary->md.crtc_h;
	if (!rf_is_landscape(this->rotation))
		this->aspect_ratio = 1 / this->aspect_ratio;
	update_viewport(this);
	rf_converter_set_viewport(this->converter, &this->viewport);
	if (!rf_remote_server_should_render_frame(this->remote))
		return;

	if (!rf_converter_is_running(this->converter)) {
		if (rf_converter_start(this->converter) < 0) {
			rf_remote_server_flush(this->remote);
			return;
		}
	}

	struct rf_rect damage;
	const int64_t convert_begin = g_get_monotonic_time();
	GByteArray *buf = rf_converter_convert(
		this->converter,
		length,
		bufs,
		this->width,
		this->height,
		this->skip_damage ? NULL : &damage
	);
	const int64_t convert_end = g_get_monotonic_time();
	if (buf != NULL) {
			if (!this->logged_frame) {
				g_message(
					"Frame: Converted first frame to %ux%u, viewport %u,%u %ux%u, damage %d,%d %ux%u.",
					this->width,
					this->height,
					this->viewport.x,
					this->viewport.y,
					this->viewport.w,
					this->viewport.h,
					this->skip_damage ? 0 : damage.x,
					this->skip_damage ? 0 : damage.y,
					this->skip_damage ? this->width : damage.w,
				this->skip_damage ? this->height : damage.h
			);
				this->logged_frame = true;
			}
			const int64_t update_begin = g_get_monotonic_time();
			rf_remote_server_update(
				this->remote,
				buf,
			this->width,
				this->height,
				this->skip_damage ? NULL : &damage
			);
			const int64_t update_end = g_get_monotonic_time();
			const int64_t total_us = update_end - convert_begin;
			if (total_us > G_USEC_PER_SEC / 15 &&
			    update_end - this->last_slow_pipeline_log_us > 5 * G_USEC_PER_SEC) {
				g_message(
					"Frame: Slow pipeline convert=%ldms update=%ldms total=%ldms.",
					(convert_end - convert_begin) / 1000,
					(update_end - update_begin) / 1000,
					total_us / 1000
				);
				this->last_slow_pipeline_log_us = update_end;
			}
		} else if (!this->logged_frame) {
			g_message("Frame: Converter returned no frame buffer for first frame.");
		}
}

static void on_first_client(RfRemoteServer *r, void *data)
{
	struct this *this = data;

	this->rotation = rf_config_get_rotation(this->config);
	rf_apply_default_size_if_unset(
		rf_config_get_default_width(this->config),
		rf_config_get_default_height(this->config),
		&this->width,
		&this->height
	);
	update_viewport(this);
	// We always recalculate this on frame so here is not important.
	this->aspect_ratio = 1.0;
	this->logged_frame = false;

	if (rf_streamer_start(this->streamer) < 0)
		rf_remote_server_flush(this->remote);
}

static void on_last_client(RfRemoteServer *r, void *data)
{
	struct this *this = data;

	rf_converter_stop(this->converter);
	rf_streamer_stop(this->streamer);
	this->width = 0;
	this->height = 0;
	this->reference_width = 0;
	this->reference_height = 0;
	this->viewport = (struct rf_viewport){ 0 };
}

static void on_pointer_event(
	RfRemoteServer *r,
	double rx,
	double ry,
	bool left,
	bool middle,
	bool right,
	bool back,
	bool forward,
	bool wup,
	bool wdown,
	bool wleft,
	bool wright,
	void *data
)
{
	struct this *this = data;

	rf_map_point_from_viewport(
		rx,
		ry,
		this->width,
		this->height,
		&this->viewport,
		&rx,
		&ry
	);
	rf_streamer_send_pointer_event(
		this->streamer,
		rx,
		ry,
		left,
		middle,
		right,
		back,
		forward,
		wup,
		wdown,
		wleft,
		wright
	);
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
	g_autofree char *rdp_clipboard_socket_path = NULL;
	g_autofree char *rdp_audio_socket_path = NULL;
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
			{ "rdp-clipboard-socket",
			  0,
			  G_OPTION_FLAG_NONE,
			  G_OPTION_ARG_FILENAME,
			  &rdp_clipboard_socket_path,
			  "RDP rich clipboard socket path to communicate.",
			  "SOCKET" },
			{ "rdp-audio-socket",
			  0,
			  G_OPTION_FLAG_NONE,
			  G_OPTION_ARG_FILENAME,
			  &rdp_audio_socket_path,
			  "RDP audio socket path to communicate.",
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
		  "Skip damage region detection and always update the whole frame buffer (debug purpose).",
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
	if (rdp_clipboard_socket_path == NULL) {
		g_mkdir("/tmp/reframe-rdp-clipboard", 0755);
		rf_set_group("/tmp/reframe-rdp-clipboard");
		rdp_clipboard_socket_path =
			g_strdup("/tmp/reframe-rdp-clipboard/reframe-rdp-clipboard.sock");
	}
	if (rdp_audio_socket_path == NULL) {
		g_mkdir("/tmp/reframe-rdp-audio", 0755);
		rf_set_group("/tmp/reframe-rdp-audio");
		rdp_audio_socket_path =
			g_strdup("/tmp/reframe-rdp-audio/reframe-rdp-audio.sock");
	}

	g_message(
		"Skip damage region detection mode is %s.",
		skip_damage ? "enabled" : "disabled"
	);
	g_message("Using configuration file %s.", config_path);
	g_message("Using socket %s.", socket_path);
	g_message("Using session socket %s.", session_socket_path);
	g_message("Using RDP clipboard socket %s.", rdp_clipboard_socket_path);
	g_message("Using RDP audio socket %s.", rdp_audio_socket_path);

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

	g_autofree char *protocol = rf_config_get_remote_protocol(this->config);
	const bool protocol_is_rdp = g_strcmp0(protocol, "rdp") == 0;
	g_message("Remote: Protocol is %s.", protocol);
	const char *module_name = NULL;
	const char *module_subdir = NULL;
	const char *constructor_name = NULL;
		if (protocol_is_rdp) {
#ifdef HAVE_RDP
		module_subdir = "rdp";
		module_name = "lib" PROJECT_NAME "-rdp." G_MODULE_SUFFIX;
		constructor_name = "rf_rdp_server_new";
#else
		g_error("Remote: RDP protocol requested but RDP support is disabled.");
		return 1;
#endif
	} else if (g_strcmp0(protocol, "vnc") == 0) {
		module_subdir = "vnc";
		constructor_name = "rf_vnc_server_new";
#ifdef HAVE_NEATVNC
		// Only check VNC type when we have choices.
		g_autofree char *type = rf_config_get_vnc_type(this->config);
		g_message("VNC: Implementation is %s.", type);
		if (g_strcmp0(type, "neatvnc") == 0)
			module_name =
				"lib" PROJECT_NAME "-neatvnc." G_MODULE_SUFFIX;
		else
#endif
			module_name = "lib" PROJECT_NAME
				      "-libvncserver." G_MODULE_SUFFIX;
	} else {
		g_error("Remote: Unsupported protocol %s.", protocol);
	}
	g_autofree char *module_path = g_build_filename(
		LIBDIR, PROJECT_NAME, module_subdir, module_name, NULL
	);
	GModule *module = g_module_open(
		module_path, G_MODULE_BIND_LAZY | G_MODULE_BIND_LOCAL
	);
	if (module == NULL)
		g_error("Remote: Failed to load module %s: %s",
			module_path,
			g_module_error());
	RfRemoteServerNewFunc rf_remote_server_new;
	if (!g_module_symbol(
		    module, constructor_name, (void **)&rf_remote_server_new
	    ))
		g_error("Remote: Failed to find %s symbol in module %s: %s",
			constructor_name,
			module_path,
			g_module_error());
	this->remote = rf_remote_server_new(this->config);
	rf_remote_server_set_rdp_clipboard_socket_path(
		this->remote,
		rdp_clipboard_socket_path
	);
	rf_remote_server_set_rdp_audio_socket_path(
		this->remote,
		rdp_audio_socket_path
	);

	this->converter = rf_converter_new(this->config);
	this->session = rf_session_new();
	rf_session_set_socket_path(this->session, session_socket_path);
	this->streamer = rf_streamer_new(this->config);
	rf_streamer_set_socket_path(this->streamer, socket_path);
	g_signal_connect_swapped(
		this->streamer,
		"stop",
		G_CALLBACK(rf_remote_server_flush),
		this->remote
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
		G_CALLBACK(rf_remote_server_set_desktop_name),
		this->remote
	);
	g_signal_connect(this->streamer, "frame", G_CALLBACK(on_frame), this);
	if (!protocol_is_rdp) {
		g_signal_connect_swapped(
			this->session,
			"clipboard-text",
			G_CALLBACK(rf_remote_server_send_clipboard_text),
			this->remote
		);
	}
	g_signal_connect(
		this->remote, "first-client", G_CALLBACK(on_first_client), this
	);
	g_signal_connect(
		this->remote, "last-client", G_CALLBACK(on_last_client), this
	);
	g_signal_connect(
		this->remote, "resize-event", G_CALLBACK(on_resize_event), this
	);
	g_signal_connect_swapped(
		this->remote,
		"keyboard-event",
		G_CALLBACK(rf_streamer_send_keyboard_event),
		this->streamer
	);
	g_signal_connect(
		this->remote,
		"pointer-event",
		G_CALLBACK(on_pointer_event),
		this
	);
	if (!protocol_is_rdp) {
		g_signal_connect_swapped(
			this->remote,
			"clipboard-text",
			G_CALLBACK(rf_session_send_clipboard_text_msg),
			this->session
		);
	}
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
	rf_remote_server_start(this->remote);

	this->main_loop = g_main_loop_new(NULL, false);
	g_unix_signal_add(SIGINT, on_sigint, this);
	g_main_loop_run(this->main_loop);
	g_main_loop_unref(this->main_loop);

	rf_remote_server_stop(this->remote);
	// Destruction sequence is decided by signal callbacks.
	g_clear_object(&this->streamer);
	g_clear_object(&this->session);
	g_clear_object(&this->converter);
	g_clear_object(&this->remote);
	g_clear_pointer(&module, g_module_close);
	g_clear_object(&this->config);

	return 0;
}
