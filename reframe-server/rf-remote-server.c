#include <xkbcommon/xkbcommon.h>

#include "rf-remote-server.h"

#if !GLIB_CHECK_VERSION(2, 74, 0)
typedef void (*GSourceOnceFunc)(gpointer user_data);

typedef struct {
	GSourceOnceFunc func;
	gpointer data;
} IdleOnceData;

static gboolean idle_once_cb(gpointer user_data)
{
	IdleOnceData *d = user_data;

	d->func(d->data);
	g_free(d);

	return G_SOURCE_REMOVE;
}

static guint g_idle_add_once(GSourceOnceFunc func, gpointer data)
{
	IdleOnceData *d = g_new(IdleOnceData, 1);

	d->func = func;
	d->data = data;

	return g_idle_add(idle_once_cb, d);
}
#endif

typedef struct _RfRemoteServerPrivate {
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	unsigned int client_idle_id;
	bool clients;
} RfRemoteServerPrivate;
G_DEFINE_TYPE_WITH_PRIVATE(RfRemoteServer, rf_remote_server, G_TYPE_OBJECT)

enum {
	SIG_FIRST_CLIENT,
	SIG_LAST_CLIENT,
	SIG_RESIZE_EVENT,
	SIG_KEYBOARD_EVENT,
	SIG_POINTER_EVENT,
	SIG_CLIPBOARD_TEXT,
	N_SIGS
};

static unsigned int sigs[N_SIGS] = { 0 };

static void finalize(GObject *o)
{
	RfRemoteServer *this = RF_REMOTE_SERVER(o);
	RfRemoteServerPrivate *priv =
		rf_remote_server_get_instance_private(this);

	if (priv->client_idle_id != 0) {
		g_source_remove(priv->client_idle_id);
		priv->client_idle_id = 0;
	}
	g_clear_pointer(&priv->xkb_keymap, xkb_keymap_unref);
	g_clear_pointer(&priv->xkb_context, xkb_context_unref);

	G_OBJECT_CLASS(rf_remote_server_parent_class)->finalize(o);
}

static void rf_remote_server_class_init(RfRemoteServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = finalize;

	klass->start = NULL;
	klass->is_running = NULL;
	klass->stop = NULL;
	klass->set_desktop_name = NULL;
	klass->set_rdp_clipboard_socket_path = NULL;
	klass->set_rdp_audio_socket_path = NULL;
	klass->send_clipboard_text = NULL;
	klass->should_render_frame = NULL;
	klass->update = NULL;
	klass->flush = NULL;

	sigs[SIG_FIRST_CLIENT] = g_signal_new(
		"first-client",
		RF_TYPE_REMOTE_SERVER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		0
	);
	sigs[SIG_LAST_CLIENT] = g_signal_new(
		"last-client",
		RF_TYPE_REMOTE_SERVER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		0
	);
	sigs[SIG_RESIZE_EVENT] = g_signal_new(
		"resize-event",
		RF_TYPE_REMOTE_SERVER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		2,
		G_TYPE_INT,
		G_TYPE_INT
	);
	sigs[SIG_KEYBOARD_EVENT] = g_signal_new(
		"keyboard-event",
		RF_TYPE_REMOTE_SERVER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		2,
		G_TYPE_UINT,
		G_TYPE_BOOLEAN
	);
	sigs[SIG_POINTER_EVENT] = g_signal_new(
		"pointer-event",
		RF_TYPE_REMOTE_SERVER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		11,
		G_TYPE_DOUBLE,
		G_TYPE_DOUBLE,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN
	);
	sigs[SIG_CLIPBOARD_TEXT] = g_signal_new(
		"clipboard-text",
		RF_TYPE_REMOTE_SERVER,
		0,
		0,
		NULL,
		NULL,
		NULL,
		G_TYPE_NONE,
		1,
		G_TYPE_STRING
	);
}

static void rf_remote_server_init(RfRemoteServer *this)
{
	RfRemoteServerPrivate *priv =
		rf_remote_server_get_instance_private(this);

	priv->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (priv->xkb_context == NULL)
		g_error("Remote: Failed to create XKB context.");
	struct xkb_rule_names names = { NULL, NULL, NULL, NULL, NULL };
	priv->xkb_keymap = xkb_keymap_new_from_names(
		priv->xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS
	);
	if (priv->xkb_keymap == NULL)
		g_error("Remote: Failed to create XKB keymap.");
}

void rf_remote_server_start(RfRemoteServer *this)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->start != NULL);

	klass->start(this);
}

bool rf_remote_server_is_running(RfRemoteServer *this)
{
	g_return_val_if_fail(RF_IS_REMOTE_SERVER(this), false);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_val_if_fail(klass->is_running != NULL, false);

	return klass->is_running(this);
}

void rf_remote_server_stop(RfRemoteServer *this)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->stop != NULL);

	klass->stop(this);
}

void rf_remote_server_set_desktop_name(
	RfRemoteServer *this,
	const char *desktop_name
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(desktop_name != NULL);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->set_desktop_name != NULL);

	klass->set_desktop_name(this, desktop_name);
}

void rf_remote_server_set_rdp_clipboard_socket_path(
	RfRemoteServer *this,
	const char *socket_path
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(socket_path != NULL);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	if (klass->set_rdp_clipboard_socket_path == NULL)
		return;

	klass->set_rdp_clipboard_socket_path(this, socket_path);
}

void rf_remote_server_set_rdp_audio_socket_path(
	RfRemoteServer *this,
	const char *socket_path
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(socket_path != NULL);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	if (klass->set_rdp_audio_socket_path == NULL)
		return;

	klass->set_rdp_audio_socket_path(this, socket_path);
}

void rf_remote_server_send_clipboard_text(RfRemoteServer *this, const char *text)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(text != NULL);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->send_clipboard_text != NULL);

	klass->send_clipboard_text(this, text);
}

bool rf_remote_server_should_render_frame(RfRemoteServer *this)
{
	g_return_val_if_fail(RF_IS_REMOTE_SERVER(this), false);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	if (klass->should_render_frame == NULL)
		return true;

	return klass->should_render_frame(this);
}

void rf_remote_server_update(
	RfRemoteServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(buf != NULL);
	g_return_if_fail(width > 0 && height > 0);

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->update != NULL);

	klass->update(this, buf, width, height, damage);
}

void rf_remote_server_flush(RfRemoteServer *this)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	RfRemoteServerClass *klass = RF_REMOTE_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->flush != NULL);

	klass->flush(this);
}

struct iterate_data {
	xkb_keysym_t keysym;
	xkb_keycode_t keycode;
	xkb_level_index_t level;
};

static void iterate_keys(struct xkb_keymap *map, xkb_keycode_t key, void *data)
{
	struct iterate_data *idata = data;
	if (idata->keycode != XKB_KEYCODE_INVALID)
		return;

	xkb_level_index_t num_levels =
		xkb_keymap_num_levels_for_key(map, key, 0);
	for (xkb_level_index_t i = 0; i < num_levels; ++i) {
		const xkb_keysym_t *syms;
		const int num_syms =
			xkb_keymap_key_get_syms_by_level(map, key, 0, i, &syms);
		for (int k = 0; k < num_syms; ++k) {
			if (syms[k] == idata->keysym) {
				idata->keycode = key;
				idata->level = i;
				return;
			}
		}
	}
}

void rf_remote_server_handle_resize_event(
	RfRemoteServer *this,
	unsigned int width,
	unsigned int height
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(width > 0 && height > 0);

	if (!rf_remote_server_is_running(this))
		return;

	g_debug("Remote: Received resize event for width %d and height %d.",
		width,
		height);
	g_signal_emit(this, sigs[SIG_RESIZE_EVENT], 0, width, height);
}

void rf_remote_server_handle_keysym_event(
	RfRemoteServer *this,
	uint32_t keysym,
	bool down
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	if (!rf_remote_server_is_running(this))
		return;

	RfRemoteServerPrivate *priv =
		rf_remote_server_get_instance_private(this);
	struct iterate_data idata = {
		.keysym = keysym,
		.keycode = XKB_KEYCODE_INVALID,
		.level = 0,
	};
	xkb_keymap_key_for_each(priv->xkb_keymap, iterate_keys, &idata);
	if (idata.keycode == XKB_KEYCODE_INVALID) {
		g_warning("Input: Failed to find keysym %04x in keymap.", keysym);
		return;
	}
	const uint32_t keycode = RF_KEY_CODE_XKB_TO_EV(idata.keycode);
	g_debug("Input: Received key %s for keysym %04x and keycode %u.",
		down ? "down" : "up",
		keysym,
		keycode);
	g_signal_emit(this, sigs[SIG_KEYBOARD_EVENT], 0, keycode, down);
}

void rf_remote_server_handle_keycode_event(
	RfRemoteServer *this,
	uint32_t keycode,
	bool down
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	if (!rf_remote_server_is_running(this))
		return;

	g_debug("Input: Received key %s for keycode %u.",
		down ? "down" : "up",
		keycode);
	g_signal_emit(this, sigs[SIG_KEYBOARD_EVENT], 0, keycode, down);
}

static inline char *true_or_false(bool b)
{
	return b ? "true" : "false";
}

void rf_remote_server_handle_pointer_state(
	RfRemoteServer *this,
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
	bool wright
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	if (!rf_remote_server_is_running(this))
		return;

	g_debug("Input: Received pointer at x %f and y %f, "
		"left %s, middle %s, right %s, back %s, forward %s, "
		"wheel up %s, wheel down %s, wheel left %s, wheel right %s.",
		rx,
		ry,
		true_or_false(left),
		true_or_false(middle),
		true_or_false(right),
		true_or_false(back),
		true_or_false(forward),
		true_or_false(wup),
		true_or_false(wdown),
		true_or_false(wleft),
		true_or_false(wright));
	g_signal_emit(
		this,
		sigs[SIG_POINTER_EVENT],
		0,
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

void rf_remote_server_handle_pointer_event(
	RfRemoteServer *this,
	double rx,
	double ry,
	uint32_t mask
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	const bool left = mask & 1;
	const bool middle = mask & (1 << 1);
	const bool right = mask & (1 << 2);
	const bool wup = mask & (1 << 3);
	const bool wdown = mask & (1 << 4);
	const bool wleft = mask & (1 << 5);
	const bool wright = mask & (1 << 6);
	const bool back = mask & (1 << 8);
	const bool forward = mask & (1 << 9);

	rf_remote_server_handle_pointer_state(
		this,
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

void rf_remote_server_handle_clipboard_text(
	RfRemoteServer *this,
	const char *text
)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));
	g_return_if_fail(text != NULL);

	if (!rf_remote_server_is_running(this))
		return;

	g_debug("Remote: Received clipboard text %s.", text);
	g_signal_emit(this, sigs[SIG_CLIPBOARD_TEXT], 0, text);
}

static void emit_client_signal(void *data)
{
	RfRemoteServer *this = data;
	RfRemoteServerPrivate *priv =
		rf_remote_server_get_instance_private(this);

	priv->client_idle_id = 0;

	if (!rf_remote_server_is_running(this))
		return;

	g_debug("Signal: Emitting remote %s client signal.",
		priv->clients ? "first" : "last");
	g_signal_emit(
		this,
		priv->clients ? sigs[SIG_FIRST_CLIENT] : sigs[SIG_LAST_CLIENT],
		0
	);
}

void rf_remote_server_handle_first_client(RfRemoteServer *this)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	if (!rf_remote_server_is_running(this))
		return;

	RfRemoteServerPrivate *priv =
		rf_remote_server_get_instance_private(this);
	priv->clients = true;

	if (priv->client_idle_id != 0)
		return;
	priv->client_idle_id = g_idle_add_once(emit_client_signal, this);
}

void rf_remote_server_handle_last_client(RfRemoteServer *this)
{
	g_return_if_fail(RF_IS_REMOTE_SERVER(this));

	if (!rf_remote_server_is_running(this))
		return;

	RfRemoteServerPrivate *priv =
		rf_remote_server_get_instance_private(this);
	priv->clients = false;

	if (priv->client_idle_id != 0)
		return;
	priv->client_idle_id = g_idle_add_once(emit_client_signal, this);
}
