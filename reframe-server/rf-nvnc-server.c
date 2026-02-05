#include <stdbool.h>
#include <glib-unix.h>
#define AML_UNSTABLE_API 1
#include <aml.h>
// XXX: There are several bugs in neatvnc's encoding, JPEG compress will make it
// crash on start, and tight encoding will make it crash with GNOME overview.
#include <neatvnc.h>
#include <pixman.h>
#include <libdrm/drm_fourcc.h>

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
	struct nvnc_fb_pool *pool;
	unsigned int clients;
	char *password;
	char *desktop_name;
	unsigned int width;
	unsigned int height;
	bool running;
};
G_DEFINE_TYPE(RfNVNCServer, rf_nvnc_server, RF_TYPE_VNC_SERVER)

static void _dispose(GObject *o)
{
	RfNVNCServer *this = RF_NVNC_SERVER(o);

	rf_vnc_server_stop(RF_VNC_SERVER(this));

	G_OBJECT_CLASS(rf_nvnc_server_parent_class)->dispose(o);
}

// static void _finalize(GObject *o)
// {
// 	RfNVNCServer *this = RF_NVNC_SERVER(o);

// 	G_OBJECT_CLASS(rf_nvnc_server_parent_class)->finalize(o);
// }

static void
_on_keysym_event(struct nvnc_client *client, uint32_t keysym, bool down)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfVNCServer *super = RF_VNC_SERVER(nvnc_get_userdata(nvnc));

	rf_vnc_server_handle_keysym_event(super, keysym, down);
}

static void
_on_keycode_event(struct nvnc_client *client, uint32_t keycode, bool down)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfVNCServer *super = RF_VNC_SERVER(nvnc_get_userdata(nvnc));

	rf_vnc_server_handle_keycode_event(super, keycode, down);
}

static void _on_pointer_event(
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
_on_clipboard_text(struct nvnc_client *client, const char *text, uint32_t length)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfVNCServer *super = RF_VNC_SERVER(nvnc_get_userdata(nvnc));

	rf_vnc_server_handle_clipboard_text(super, text);
}

static bool _on_resize_event(
	struct nvnc_client *client,
	const struct nvnc_desktop_layout *layout
)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);
	unsigned int width = nvnc_desktop_layout_get_width(layout);
	unsigned int height = nvnc_desktop_layout_get_height(layout);

	if (width != this->width || height != this->height) {
		rf_vnc_server_handle_resize_event(super, width, height);
	}

	return true;
}

static void _on_client_gone(struct nvnc_client *client)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

	if (this->clients-- == 1)
		rf_vnc_server_handle_last_client(super);
}

static void _on_new_client(struct nvnc_client *client)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	RfVNCServer *super = RF_VNC_SERVER(this);

	nvnc_set_client_cleanup_fn(client, _on_client_gone);
	if (++this->clients == 1)
		rf_vnc_server_handle_first_client(super);
}

static bool _on_auth(const char *username, const char *password, void *data)
{
	RfNVNCServer *this = data;

	return g_strcmp0(password, this->password) == 0;
}

static int _poll_aml(int fd, GIOCondition condition, void *data)
{
	RfNVNCServer *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	aml_poll(this->aml, 0);
	aml_dispatch(this->aml);

	return G_SOURCE_CONTINUE;
}

static void _start(RfVNCServer *super)
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

	const unsigned int port = rf_config_get_vnc_port(this->config);
	g_message("VNC: Listening on port %u.", port);

	this->clients = 0;

	this->aml = aml_new();
	aml_set_default(this->aml);

	this->nvnc = nvnc_open("0.0.0.0", port);
	if (this->nvnc == NULL)
		g_error("Failed to listen on port %u.", port);
	nvnc_set_userdata(this->nvnc, this, NULL);
	this->display = nvnc_display_new(0, 0);
	if (this->display == NULL)
		g_error("VNC: Failed to create neatvnc display.");
	nvnc_add_display(this->nvnc, this->display);
	this->pool = nvnc_fb_pool_new(
		this->width, this->height, DRM_FORMAT_XBGR8888, this->width
	);
	if (this->desktop_name != NULL)
		nvnc_set_name(this->nvnc, this->desktop_name);
	nvnc_set_key_fn(this->nvnc, _on_keysym_event);
	nvnc_set_key_code_fn(this->nvnc, _on_keycode_event);
	nvnc_set_pointer_fn(this->nvnc, _on_pointer_event);
	nvnc_set_cut_text_fn(this->nvnc, _on_clipboard_text);
	nvnc_set_desktop_layout_fn(this->nvnc, _on_resize_event);
	nvnc_set_new_client_fn(this->nvnc, _on_new_client);
	if (this->password != NULL && this->password[0] != '\0')
		nvnc_enable_auth(
			this->nvnc, NVNC_AUTH_REQUIRE_AUTH, _on_auth, this
		);
	struct nvnc_fb *fb = nvnc_fb_pool_acquire(this->pool);
	memset(nvnc_fb_get_addr(fb),
	       0,
	       this->width * this->height * RF_BYTES_PER_PIXEL);
	struct pixman_region16 region;
	pixman_region_init_rect(&region, 0, 0, this->width, this->height);
	nvnc_display_feed_buffer(this->display, fb, &region);
	pixman_region_fini(&region);
	nvnc_fb_pool_release(this->pool, fb);

	// Integrate aml into GLib's main loop.
	this->aml_id = g_unix_fd_add(
		aml_get_fd(this->aml), this->io_flags, _poll_aml, this
	);

	this->running = true;
}

static bool _is_running(RfVNCServer *super)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	return this->running;
}

static void _stop(RfVNCServer *super)
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
	g_clear_pointer(&this->display, nvnc_display_unref);
	g_clear_pointer(&this->nvnc, nvnc_close);
	aml_set_default(NULL);
	g_clear_pointer(&this->aml, aml_unref);
	g_clear_pointer(&this->buf, g_byte_array_unref);
	g_clear_pointer(&this->desktop_name, g_free);
	g_clear_pointer(&this->password, g_free);
}

static void _set_desktop_name(RfVNCServer *super, const char *desktop_name)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	g_clear_pointer(&this->desktop_name, g_free);
	this->desktop_name = g_strdup(desktop_name);
	// Well this does not work because VNC does not update desktop name to
	// client after it is inited. Clients will get the previous desktop name
	// which may not be correct.
	// nvnc_set_name(this->nvnc, this->desktop_name);
}

static void _send_clipboard_text(RfVNCServer *super, const char *text)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (!this->running)
		return;

	nvnc_send_cut_text(this->nvnc, text, strlen(text) + 1);
}

static void
_update(RfVNCServer *super,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage)
{
	RfNVNCServer *this = RF_NVNC_SERVER(super);

	if (!this->running)
		return;

	if (this->buf != buf || this->width != width ||
	    this->height != height) {
		if (this->buf != buf) {
			g_clear_pointer(&this->buf, g_byte_array_unref);
			this->buf = g_byte_array_ref(buf);
		}
		if (this->width != width || this->height != height) {
			this->width = width;
			this->height = height;
			nvnc_fb_pool_resize(
				this->pool,
				this->width,
				this->height,
				DRM_FORMAT_XBGR8888,
				this->width
			);
		}
	}
	struct nvnc_fb *fb = nvnc_fb_pool_acquire(this->pool);
	memcpy(nvnc_fb_get_addr(fb),
	       this->buf->data,
	       this->width * this->height * RF_BYTES_PER_PIXEL);
	struct pixman_region16 region;
	if (damage != NULL)
		pixman_region_init_rect(
			&region, damage->x, damage->y, damage->w, damage->h
		);
	else
		pixman_region_init_rect(
			&region, 0, 0, this->width, this->height
		);
	nvnc_display_feed_buffer(this->display, fb, &region);
	pixman_region_fini(&region);
	nvnc_fb_pool_release(this->pool, fb);
}

static void _flush(RfVNCServer *super)
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

	o_class->dispose = _dispose;
	// o_class->finalize = _finalize;

	v_class->start = _start;
	v_class->is_running = _is_running;
	v_class->stop = _stop;
	v_class->set_desktop_name = _set_desktop_name;
	v_class->send_clipboard_text = _send_clipboard_text;
	v_class->update = _update;
	v_class->flush = _flush;
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
	this->pool = NULL;
	this->password = NULL;
	this->desktop_name = NULL;
	this->width = 0;
	this->height = 0;
	this->running = false;
}

RfVNCServer *rf_nvnc_server_new(RfConfig *config)
{
	RfNVNCServer *this = g_object_new(RF_TYPE_NVNC_SERVER, NULL);
	this->config = config;
	return RF_VNC_SERVER(this);
}
