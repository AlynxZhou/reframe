#include <stdbool.h>
#include <glib-unix.h>
#include <aml.h>
#include <neatvnc.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

#include "config.h"
#include "rf-common.h"
#include "rf-nvnc-server.h"

struct _RfNVNCServer {
	RfVNCServer parent_instance;
	RfConfig *config;
	GByteArray *buf;
	GIOCondition io_flags;
	unsigned int aml_id;
	struct aml *aml;
	struct nvnc *nvnc;
	struct nvnc_display *display;
	unsigned int clients;
	char *password;
	char *desktop_name;
	unsigned int width;
	unsigned int height;
	bool resize;
	char *username;
	bool allow_broken_crypto;
	char *rsa_private_key_file;
	char *tls_private_key_file;
	char *tls_certificate_file;
	bool running;
};
G_DEFINE_TYPE(RfNVNCServer, rf_nvnc_server, RF_TYPE_VNC_SERVER)

static void dispose(GObject *o)
{
	RfNVNCServer *this = RF_NVNC_SERVER(o);
	RfVNCServer *super = RF_VNC_SERVER(this);

	rf_vnc_server_stop(super);

	G_OBJECT_CLASS(rf_nvnc_server_parent_class)->dispose(o);
}

// static void finalize(GObject *o)
// {
// 	RfNVNCServer *this = RF_NVNC_SERVER(o);

// 	G_OBJECT_CLASS(rf_nvnc_server_parent_class)->finalize(o);
// }

static void
on_keysym_event(struct nvnc_client *client, uint32_t keysym, bool down)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

	rf_vnc_server_handle_keysym_event(super, keysym, down);
}

static void
on_keycode_event(struct nvnc_client *client, uint32_t keycode, bool down)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

	rf_vnc_server_handle_keycode_event(super, keycode, down);
}

static void on_pointer_event(
	struct nvnc_client *client,
	uint16_t x,
	uint16_t y,
	enum nvnc_button_mask buttons
)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);
	double rx = (double)x / this->width;
	double ry = (double)y / this->height;
	// neatvnc does not follow RFB's ExtendedMouseButtons bits, correct it.
	uint32_t mask = (buttons & 0x7f) | ((buttons >> 7) << 8);

	rf_vnc_server_handle_pointer_event(super, rx, ry, mask);
}

static void
on_clipboard_text(struct nvnc_client *client, const char *text, uint32_t length)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

	rf_vnc_server_handle_clipboard_text(super, text);
}

static bool on_resize_event(
	struct nvnc_client *client,
	const struct nvnc_desktop_layout *layout
)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);
	unsigned int width = nvnc_desktop_layout_get_width(layout);
	unsigned int height = nvnc_desktop_layout_get_height(layout);

	if (!this->resize)
		return false;

	if (width != this->width || height != this->height)
		rf_vnc_server_handle_resize_event(super, width, height);

	return true;
}

static void on_client_gone(void *data)
{
	struct nvnc_client *client = data;
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

	if (this->clients-- == 1)
		rf_vnc_server_handle_last_client(super);
}

static void on_new_client(struct nvnc_client *client)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

#ifndef NEATVNC_UNSTABLE_API
	nvnc_client_set_userdata(client, client, on_client_gone);
#else
	nvnc_set_client_cleanup_fn(client, (nvnc_client_fn)on_client_gone);
#endif
	if (++this->clients == 1)
		rf_vnc_server_handle_first_client(super);
}

#ifndef NEATVNC_UNSTABLE_API
static bool check_credentials(RfNVNCServer *this, struct nvnc_auth_creds *creds)
{
	const char *username = nvnc_auth_creds_get_username(creds);
	const char *password = nvnc_auth_creds_get_password(creds);

	if (password == NULL)
		return nvnc_auth_creds_verify(creds, this->password);
	if (username == NULL)
		return false;
	if (g_strcmp0(username, this->username ? this->username : "") != 0)
		return false;
	if (g_strcmp0(password, this->password) != 0)
		return false;

	return true;
}

static void on_auth(struct nvnc_auth_creds *creds, void *data)
{
	RfNVNCServer *this = data;

	if (check_credentials(this, creds))
		nvnc_auth_creds_accept(creds);
	else
		nvnc_auth_creds_reject(creds, "Invalid username or password");
}
#else
static bool on_auth(const char *username, const char *password, void *data)
{
	RfNVNCServer *this = data;

	return g_strcmp0(password, this->password) == 0;
}
#endif

static int poll_aml(int fd, GIOCondition condition, void *data)
{
	RfNVNCServer *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	aml_poll(this->aml, 0);
	aml_dispatch(this->aml);

	return G_SOURCE_CONTINUE;
}

#ifndef NEATVNC_UNSTABLE_API
static void
listen_tcp(RfNVNCServer *this, const char *ip, const unsigned int port)
{
	// NULL is valid, but empty string will be ignored.
	if (ip != NULL && ip[0] == '\0')
		return;

	g_message("VNC: Listening on %s:%u.", ip, port);
	// It would be acceptable if we failed to listen to some addresses.
	if (nvnc_listen_tcp(this->nvnc, ip, port, NVNC_STREAM_NORMAL) != 0)
		g_warning("VNC: Failed to listen on %s:%u.", ip, port);
}
#endif

static void start(RfVNCServer *super)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (this->running)
		return;

	this->password = rf_config_get_vnc_password(this->config);
	this->desktop_name = rf_config_get_connector(this->config);
	this->width = rf_config_get_default_width(this->config);
	this->height = rf_config_get_default_height(this->config);
	if (this->width == 0 || this->height == 0) {
		// `0x0` is not a valid size for VNC, so we set a initial value
		// here and wait until it resizes to first frame size in main.
		// Remember this is only for making VNC happy and we keep `0x0`
		// in main so it knows we need to resize later.
		this->width = 800;
		this->height = 600;
	} else {
		g_message(
			"VNC: Got default width %u and height %u.",
			this->width,
			this->height
		);
	}
	this->resize = rf_config_get_resize(this->config);
	g_message(
		"VNC: Client resizing will be %s.",
		this->resize ? "allowed" : "prohibited"
	);
	this->username =
		rf_config_get_neatvnc_username(this->config);
	this->allow_broken_crypto =
		rf_config_get_neatvnc_allow_broken_crypto(this->config);
	this->rsa_private_key_file =
		rf_config_get_neatvnc_rsa_private_key_file(this->config);
	this->tls_private_key_file =
		rf_config_get_neatvnc_tls_private_key_file(this->config);
	this->tls_certificate_file =
		rf_config_get_neatvnc_tls_certificate_file(this->config);

	this->clients = 0;

	this->aml = aml_new();
	aml_set_default(this->aml);

	g_autofree char **ips = rf_config_get_vnc_ip_list(this->config);
	const unsigned int port = rf_config_get_vnc_port(this->config);
#ifndef NEATVNC_UNSTABLE_API
	this->nvnc = nvnc_new();
	if (ips != NULL) {
		for (int i = 0; ips[i] != NULL; ++i)
			listen_tcp(this, ips[i], port);
	} else {
		listen_tcp(this, NULL, port);
	}
#else
	g_warning("VNC: We only support 1 IP to listen with neatvnc unstable.");
	char *ip = NULL;
	if (ips != NULL)
		ip = ips[0];
	if (ip != NULL && ip[0] == '\0')
		ip = NULL;
	g_message("VNC: Listening on %s:%u.", ip, port);
	this->nvnc = nvnc_open(ip, port);
	if (this->nvnc == NULL)
		g_error("VNC: Failed to listen on %s:%u.", ip, port);
#endif
	nvnc_set_userdata(this->nvnc, this, NULL);
	this->display = nvnc_display_new(0, 0);
	if (this->display == NULL)
		g_error("VNC: Failed to create neatvnc display.");
	nvnc_add_display(this->nvnc, this->display);
	if (this->desktop_name != NULL)
		nvnc_set_name(this->nvnc, this->desktop_name);
	nvnc_set_key_fn(this->nvnc, on_keysym_event);
	nvnc_set_key_code_fn(this->nvnc, on_keycode_event);
	nvnc_set_pointer_fn(this->nvnc, on_pointer_event);
	nvnc_set_cut_text_fn(this->nvnc, on_clipboard_text);
	nvnc_set_desktop_layout_fn(this->nvnc, on_resize_event);
	nvnc_set_new_client_fn(this->nvnc, on_new_client);
	enum nvnc_auth_flags auth_flags = NVNC_AUTH_REQUIRE_AUTH;
#ifndef NEATVNC_UNSTABLE_API
	if (this->allow_broken_crypto)
		auth_flags |= NVNC_AUTH_ALLOW_BROKEN_CRYPTO;
#endif
	if (this->password != NULL && this->password[0] != '\0')
		nvnc_enable_auth(this->nvnc, auth_flags, on_auth, this);
	if (this->rsa_private_key_file != NULL) {
		if (nvnc_set_rsa_creds(this->nvnc,
		                       this->rsa_private_key_file) < 0) {
			g_error("VNC: Failed to set RSA credentials.");
		};
	}
	if (this->tls_private_key_file != NULL &&
	    this->tls_certificate_file != NULL) {
		if (nvnc_set_tls_creds(this->nvnc,
		                       this->tls_private_key_file,
		                       this->tls_certificate_file) < 0) {
			g_error("VNC: Failed to set TLS credentials.");
		}
	}
	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, this->width, this->height);
#ifndef NEATVNC_UNSTABLE_API
	struct nvnc_frame *frame = nvnc_frame_new(
		this->width, this->height, DRM_FORMAT_XBGR8888, this->width
	);
	memset(nvnc_frame_get_addr(frame),
	       0,
	       this->width * this->height * nvnc_frame_get_pixel_size(frame));
	nvnc_frame_set_damage(frame, &region);
	nvnc_display_feed_frame(this->display, frame);
	nvnc_frame_unref(frame);
#else
	struct nvnc_fb *fb = nvnc_fb_new(
		this->width, this->height, DRM_FORMAT_XBGR8888, this->width
	);
	memset(nvnc_fb_get_addr(fb),
	       0,
	       this->width * this->height * nvnc_fb_get_pixel_size(fb));

	nvnc_display_feed_buffer(this->display, fb, &region);
	nvnc_fb_unref(fb);
#endif
	pixman_region_fini(&region);

	// Integrate aml into GLib's main loop.
	this->aml_id = g_unix_fd_add(
		aml_get_fd(this->aml), this->io_flags, poll_aml, this
	);

	this->running = true;
}

static bool is_running(RfVNCServer *super)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	return this->running;
}

static void stop(RfVNCServer *super)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (!this->running)
		return;

	this->running = false;

	rf_vnc_server_flush(super);
	if (this->aml_id != 0) {
		g_source_remove(this->aml_id);
		this->aml_id = 0;
	}
	nvnc_remove_display(this->nvnc, this->display);
	g_clear_pointer(&this->display, nvnc_display_unref);
#ifndef NEATVNC_UNSTABLE_API
	g_clear_pointer(&this->nvnc, nvnc_del);
#else
	g_clear_pointer(&this->nvnc, nvnc_close);
#endif
	if (this->aml != NULL) {
		aml_set_default(NULL);
		aml_unref(this->aml);
		this->aml = NULL;
	}
	g_clear_pointer(&this->buf, g_byte_array_unref);
	g_clear_pointer(&this->desktop_name, g_free);
	g_clear_pointer(&this->password, g_free);
}

static void set_desktop_name(RfVNCServer *super, const char *desktop_name)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	g_clear_pointer(&this->desktop_name, g_free);
	this->desktop_name = g_strdup(desktop_name);
#ifndef NEATVNC_UNSTABLE_API
	nvnc_set_name(this->nvnc, this->desktop_name);
#endif
}

static void send_clipboard_text(RfVNCServer *super, const char *text)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (!this->running)
		return;

	nvnc_send_cut_text(this->nvnc, text, strlen(text) + 1);
}

static void
update(RfVNCServer *super,
       GByteArray *buf,
       unsigned int width,
       unsigned int height,
       const struct rf_rect *damage)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (!this->running)
		return;

	if (this->buf != buf) {
		g_clear_pointer(&this->buf, g_byte_array_unref);
		this->buf = g_byte_array_ref(buf);
	}
	if (this->width != width || this->height != height) {
		this->width = width;
		this->height = height;
		// nvnc_display_set_logical_size(this->display, width, height);
	}
	struct pixman_region16 region;
	if (damage != NULL)
		pixman_region_init_rect(
			&region, damage->x, damage->y, damage->w, damage->h
		);
	else
		pixman_region_init_rect(
			&region, 0, 0, this->width, this->height
		);
#ifndef NEATVNC_UNSTABLE_API
	struct nvnc_frame *frame = nvnc_frame_from_raw(
		this->buf->data,
		this->width,
		this->height,
		DRM_FORMAT_XBGR8888,
		this->width
	);
	nvnc_frame_set_damage(frame, &region);
	nvnc_display_feed_frame(this->display, frame);
	nvnc_frame_unref(frame);
#else
	struct nvnc_fb *fb = nvnc_fb_from_buffer(
		this->buf->data,
		this->width,
		this->height,
		DRM_FORMAT_XBGR8888,
		this->width
	);
	nvnc_display_feed_buffer(this->display, fb, &region);
	nvnc_fb_unref(fb);
#endif
	pixman_region_fini(&region);
}

static void flush(RfVNCServer *super)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (!this->running)
		return;

	struct nvnc_client *client = nvnc_client_first(this->nvnc);
	while (client != NULL) {
		struct nvnc_client *next = nvnc_client_next(client);
		nvnc_client_close(client);
		client = next;
	}
}

static void rf_nvnc_server_class_init(RfNVNCServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);
	RfVNCServerClass *v_class = RF_VNC_SERVER_CLASS(klass);

	o_class->dispose = dispose;
	// o_class->finalize = finalize;

	v_class->start = start;
	v_class->is_running = is_running;
	v_class->stop = stop;
	v_class->set_desktop_name = set_desktop_name;
	v_class->send_clipboard_text = send_clipboard_text;
	v_class->update = update;
	v_class->flush = flush;
}

static void rf_nvnc_server_init(RfNVNCServer *this)
{
	this->config = NULL;
	this->buf = NULL;
	this->io_flags = G_IO_IN | G_IO_PRI;
	this->aml_id = 0;
	this->aml = NULL;
	this->nvnc = NULL;
	this->display = NULL;
	this->password = NULL;
	this->desktop_name = NULL;
	this->width = 0;
	this->height = 0;
	this->resize = true;
	this->running = false;
}

G_MODULE_EXPORT RfVNCServer *rf_vnc_server_new(RfConfig *config)
{
	RfNVNCServer *this = g_object_new(RF_TYPE_NVNC_SERVER, NULL);
	this->config = config;
	return RF_VNC_SERVER(this);
}
