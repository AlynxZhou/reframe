#include <stdbool.h>
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
	GThread *thread;
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

// Because neatvnc runs in its own thread, we need to queue events to main
// thread, otherwise a event might comes when the previous event is not sent.

struct _keysym_event {
	RfVNCServer *vnc;
	uint32_t keysym;
	bool down;
};

static void _handle_keysym_event(gpointer data)
{
	g_autofree struct _keysym_event *e = data;
	rf_vnc_server_handle_keysym_event(e->vnc, e->keysym, e->down);
	g_object_unref(e->vnc);
}

static void
_on_keysym_event(struct nvnc_client *client, uint32_t keysym, bool down)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	struct _keysym_event *e = g_malloc0(sizeof(*e));
	e->vnc = RF_VNC_SERVER(g_object_ref(this));
	e->keysym = keysym;
	e->down = down;
	g_idle_add_once(_handle_keysym_event, e);
}

struct _keycode_event {
	RfVNCServer *vnc;
	uint32_t keycode;
	bool down;
};

static void _handle_keycode_event(gpointer data)
{
	g_autofree struct _keycode_event *e = data;
	rf_vnc_server_handle_keycode_event(e->vnc, e->keycode, e->down);
	g_object_unref(e->vnc);
}

static void
_on_keycode_event(struct nvnc_client *client, uint32_t keycode, bool down)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	struct _keycode_event *e = g_malloc0(sizeof(*e));
	e->vnc = RF_VNC_SERVER(g_object_ref(this));
	e->keycode = keycode;
	e->down = down;
	g_idle_add_once(_handle_keycode_event, e);
}

struct _pointer_event {
	RfVNCServer *vnc;
	double rx;
	double ry;
	uint32_t mask;
};

static void _handle_pointer_event(gpointer data)
{
	g_autofree struct _pointer_event *e = data;
	rf_vnc_server_handle_pointer_event(e->vnc, e->rx, e->ry, e->mask);
	g_object_unref(e->vnc);
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
	struct _pointer_event *e = g_malloc0(sizeof(*e));
	e->vnc = RF_VNC_SERVER(g_object_ref(this));
	e->rx = (double)x / this->width;
	e->ry = (double)y / this->height;
	// neatvnc does not follow RFB's ExtendedMouseButtons bits, correct it.
	e->mask = (buttons & 0x7f) | ((buttons >> 7) << 8);
	g_idle_add_once(_handle_pointer_event, e);
}

struct _clipboard_text {
	RfVNCServer *vnc;
	char *text;
};

static void _handle_clipboard_text(gpointer data)
{
	g_autofree struct _clipboard_text *e = data;
	rf_vnc_server_handle_clipboard_text(e->vnc, e->text);
	g_free(e->text);
	g_object_unref(e->vnc);
}

static void
_on_clipboard_text(struct nvnc_client *client, const char *text, uint32_t length)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	struct _clipboard_text *e = g_malloc0(sizeof(*e));
	e->vnc = RF_VNC_SERVER(g_object_ref(this));
	e->text = g_strdup(text);
	g_idle_add_once(_handle_clipboard_text, e);
}

struct _resize_event {
	RfVNCServer *vnc;
	unsigned int width;
	unsigned int height;
};

static void _handle_resize_event(gpointer data)
{
	g_autofree struct _resize_event *e = data;
	rf_vnc_server_handle_resize_event(e->vnc, e->width, e->height);
	g_object_unref(e->vnc);
}

static bool _on_resize_event(
	struct nvnc_client *client,
	const struct nvnc_desktop_layout *layout
)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	unsigned int width = nvnc_desktop_layout_get_width(layout);
	unsigned int height = nvnc_desktop_layout_get_height(layout);
	if (width != this->width || height != this->height) {
		struct _resize_event *e = g_malloc0(sizeof(*e));
		e->vnc = RF_VNC_SERVER(g_object_ref(this));
		e->width = width;
		e->height = height;
		g_idle_add_once(_handle_resize_event, e);
	}
	return true;
}

static void _on_client_gone(struct nvnc_client *client)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	if (this->clients-- == 1)
		rf_vnc_server_handle_last_client(RF_VNC_SERVER(this));
}

static void _on_new_client(struct nvnc_client *client)
{
	struct nvnc *nvnc = nvnc_client_get_server(client);
	RfNVNCServer *this = nvnc_get_userdata(nvnc);
	nvnc_set_client_cleanup_fn(client, _on_client_gone);
	if (++this->clients == 1)
		rf_vnc_server_handle_first_client(RF_VNC_SERVER(this));
}

static bool _on_auth(const char *username, const char *password, void *data)
{
	RfNVNCServer *this = data;
	return g_strcmp0(password, this->password) == 0;
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

	unsigned int port = rf_config_get_vnc_port(this->config);
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
	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, this->width, this->height);
	nvnc_display_feed_buffer(this->display, fb, &damage);
	pixman_region_fini(&damage);
	nvnc_fb_pool_release(this->pool, fb);

	// Well no one really likes your event loop, please do not re-invent the
	// wheel and hard code another library with it next time. It would be
	// easier if we could integrate neatvnc's events into GLib's event loop.
	this->thread =
		g_thread_new("reframe-aml", (GThreadFunc)aml_run, this->aml);

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
	aml_exit(this->aml);
	g_thread_join(this->thread);
	g_clear_pointer(&this->thread, g_thread_unref);
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
	unsigned int height)
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
	struct pixman_region16 damage;
	pixman_region_init_rect(&damage, 0, 0, this->width, this->height);
	nvnc_display_feed_buffer(this->display, fb, &damage);
	pixman_region_fini(&damage);
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
	this->thread = NULL;
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
