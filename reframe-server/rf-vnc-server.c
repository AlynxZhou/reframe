#include <xkbcommon/xkbcommon.h>

#include "rf-common.h"
#include "rf-vnc-server.h"

typedef struct _RfVNCServerPrivate {
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
} RfVNCServerPrivate;
G_DEFINE_TYPE_WITH_PRIVATE(RfVNCServer, rf_vnc_server, G_TYPE_OBJECT)

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

static void _finalize(GObject *o)
{
	RfVNCServer *this = RF_VNC_SERVER(o);
	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	g_clear_pointer(&priv->xkb_keymap, xkb_keymap_unref);
	g_clear_pointer(&priv->xkb_context, xkb_context_unref);

	G_OBJECT_CLASS(rf_vnc_server_parent_class)->finalize(o);
}

static void rf_vnc_server_class_init(RfVNCServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = _finalize;

	klass->start = NULL;
	klass->is_running = NULL;
	klass->stop = NULL;
	klass->set_desktop_name = NULL;
	klass->send_clipboard_text = NULL;
	klass->update = NULL;
	klass->flush = NULL;

	sigs[SIG_FIRST_CLIENT] = g_signal_new(
		"first-client",
		RF_TYPE_VNC_SERVER,
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
		RF_TYPE_VNC_SERVER,
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
		RF_TYPE_VNC_SERVER,
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
		RF_TYPE_VNC_SERVER,
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
		RF_TYPE_VNC_SERVER,
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
		RF_TYPE_VNC_SERVER,
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

static void rf_vnc_server_init(RfVNCServer *this)
{
	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	priv->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (priv->xkb_context == NULL)
		g_error("Failed to create XKB context.");
	struct xkb_rule_names names = { NULL, NULL, NULL, NULL, NULL };
	priv->xkb_keymap = xkb_keymap_new_from_names(
		priv->xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS
	);
	if (priv->xkb_keymap == NULL)
		g_error("Failed to create XKB context.");
}

void rf_vnc_server_start(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->start != NULL);

	klass->start(this);
}

bool rf_vnc_server_is_running(RfVNCServer *this)
{
	g_return_val_if_fail(RF_IS_VNC_SERVER(this), false);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_val_if_fail(klass->is_running != NULL, false);

	return klass->is_running(this);
}

void rf_vnc_server_stop(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->stop != NULL);

	klass->stop(this);
}

void rf_vnc_server_set_desktop_name(RfVNCServer *this, const char *desktop_name)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(desktop_name != NULL);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->set_desktop_name != NULL);

	klass->set_desktop_name(this, desktop_name);
}

void rf_vnc_server_send_clipboard_text(RfVNCServer *this, const char *text)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(text != NULL);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->send_clipboard_text != NULL);

	klass->send_clipboard_text(this, text);
}

void rf_vnc_server_update(
	RfVNCServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(buf != NULL);
	g_return_if_fail(width > 0 && height > 0);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->update != NULL);

	klass->update(this, buf, width, height);
}

void rf_vnc_server_flush(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);
	g_return_if_fail(klass->flush != NULL);

	klass->flush(this);
}

struct _iterate_data {
	xkb_keysym_t keysym;
	xkb_keycode_t keycode;
	xkb_level_index_t level;
};

static void _iterate_keys(struct xkb_keymap *map, xkb_keycode_t key, void *data)
{
	struct _iterate_data *idata = data;
	if (idata->keycode != XKB_KEYCODE_INVALID)
		return;

	xkb_level_index_t num_levels =
		xkb_keymap_num_levels_for_key(map, key, 0);
	for (xkb_level_index_t i = 0; i < num_levels; ++i) {
		const xkb_keysym_t *syms;
		int num_syms =
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

void rf_vnc_server_handle_resize_event(
	RfVNCServer *this,
	unsigned int width,
	unsigned int height
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(width > 0 && height > 0);

	if (!rf_vnc_server_is_running(this))
		return;

	g_debug("VNC: Received resize event for width %d and height %d.",
		width,
		height);
	g_signal_emit(this, sigs[SIG_RESIZE_EVENT], 0, width, height);
}

void rf_vnc_server_handle_keysym_event(
	RfVNCServer *this,
	uint32_t keysym,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (!rf_vnc_server_is_running(this))
		return;

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);
	struct _iterate_data idata = {
		.keysym = keysym,
		.keycode = XKB_KEYCODE_INVALID,
		.level = 0,
	};
	xkb_keymap_key_for_each(priv->xkb_keymap, _iterate_keys, &idata);
	if (idata.keycode == XKB_KEYCODE_INVALID) {
		g_warning(
			"Input: Failed to find keysym %04x in keymap.", keysym
		);
		return;
	}
	uint32_t keycode = RF_KEY_CODE_XKB_TO_EV(idata.keycode);
	g_debug("Input: Received key %s for keysym %04x and keycode %u.",
		down ? "down" : "up",
		keysym,
		keycode);
	g_signal_emit(this, sigs[SIG_KEYBOARD_EVENT], 0, keycode, down);
}

void rf_vnc_server_handle_keycode_event(
	RfVNCServer *this,
	uint32_t keycode,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (!rf_vnc_server_is_running(this))
		return;

	g_debug("Input: Received key %s for keycode %u.",
		down ? "down" : "up",
		keycode);
	g_signal_emit(this, sigs[SIG_KEYBOARD_EVENT], 0, keycode, down);
}

static inline char *_true_or_false(bool b)
{
	return b ? "true" : "false";
}

void rf_vnc_server_handle_pointer_event(
	RfVNCServer *this,
	double rx,
	double ry,
	uint32_t mask
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (!rf_vnc_server_is_running(this))
		return;

	bool left = mask & 1;
	bool middle = mask & (1 << 1);
	bool right = mask & (1 << 2);
	bool wup = mask & (1 << 3);
	bool wdown = mask & (1 << 4);
	bool wleft = mask & (1 << 5);
	bool wright = mask & (1 << 6);
	// Generally the 7th bit is reserved.
	bool back = mask & (1 << 8);
	bool forward = mask & (1 << 9);
	g_debug("Input: Received pointer at x %f and y %f, raw button %#x, "
		"left %s, middle %s, right %s, back %s, forward %s, "
		"wheel up %s, wheel down %s, wheel left %s, wheel right %s.",
		rx,
		ry,
		mask,
		_true_or_false(left),
		_true_or_false(middle),
		_true_or_false(right),
		_true_or_false(back),
		_true_or_false(forward),
		_true_or_false(wup),
		_true_or_false(wdown),
		_true_or_false(wleft),
		_true_or_false(wright));
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

void rf_vnc_server_handle_clipboard_text(RfVNCServer *this, const char *text)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(text != NULL);

	if (!rf_vnc_server_is_running(this))
		return;

	g_debug("VNC: Received clipboard text %s.", text);
	g_signal_emit(this, sigs[SIG_CLIPBOARD_TEXT], 0, text);
}

// Those signals must be delayed to next event by using `g_idle_add()`, because
// they are related to create/destroy resources and triggered during processing
// events, it is not OK to release resources while still processing events so we
// have to delay them until events are done.

static void _emit_first_client(gpointer data)
{
	RfVNCServer *this = data;
	g_debug("Signal: Emitting VNC first client signal.");
	g_signal_emit(this, sigs[SIG_FIRST_CLIENT], 0);
}

void rf_vnc_server_handle_first_client(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (!rf_vnc_server_is_running(this))
		return;

	g_idle_add_once(_emit_first_client, this);
}

static void _emit_last_client(gpointer data)
{
	RfVNCServer *this = data;
	g_debug("Signal: Emitting VNC last client signal.");
	g_signal_emit(this, sigs[SIG_LAST_CLIENT], 0);
}

void rf_vnc_server_handle_last_client(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	if (!rf_vnc_server_is_running(this))
		return;

	g_idle_add_once(_emit_last_client, this);
}
