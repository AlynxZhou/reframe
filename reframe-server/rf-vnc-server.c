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

static void rf_vnc_server_class_init(RfVNCServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = _finalize;

	klass->start = NULL;
	klass->is_running = NULL;
	klass->stop = NULL;
	klass->set_desktop_name = NULL;
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
		7,
		G_TYPE_DOUBLE,
		G_TYPE_DOUBLE,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN,
		G_TYPE_BOOLEAN
	);
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

void rf_vnc_server_handle_keyboard_event(
	RfVNCServer *this,
	uint32_t keysym,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

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

	bool left = mask & 1;
	bool middle = mask & (1 << 1);
	bool right = mask & (1 << 2);
	bool wup = mask & (1 << 3);
	bool wdown = mask & (1 << 4);
	g_debug("Input: Received pointer at x %f and y %f, left %s, middle %s, right %s, wheel up %s, wheel down %s.",
		rx,
		ry,
		_true_or_false(left),
		_true_or_false(middle),
		_true_or_false(right),
		_true_or_false(wup),
		_true_or_false(wdown));
	g_signal_emit(
		this,
		sigs[SIG_POINTER_EVENT],
		0,
		rx,
		ry,
		left,
		middle,
		right,
		wup,
		wdown
	);
}

void rf_vnc_server_handle_resize_event(
	RfVNCServer *this,
	unsigned int width,
	unsigned int height
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(width > 0 && height > 0);

	g_debug("VNC: Emitting resize signal for width %d and height %d.",
		width,
		height);
	g_signal_emit(this, sigs[SIG_RESIZE_EVENT], 0, width, height);
}

// Those signals must be delayed to next event by using `g_idle_add()`, because
// they are related to create/destroy resources and triggered during processing
// events, it is not OK to release resources while still processing events so we
// have to delay them until events are done.

static gboolean _emit_first_client(gpointer data)
{
	RfVNCServer *this = data;

	g_debug("VNC: Emitting first client signal.");
	g_signal_emit(this, sigs[SIG_FIRST_CLIENT], 0);

	return G_SOURCE_REMOVE;
}

static gboolean _emit_last_client(gpointer data)
{
	RfVNCServer *this = data;

	g_debug("VNC: Emitting last client signal.");
	g_signal_emit(this, sigs[SIG_LAST_CLIENT], 0);

	return G_SOURCE_REMOVE;
}

void rf_vnc_server_handle_first_client(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	g_idle_add(_emit_first_client, this);
}

void rf_vnc_server_handle_last_client(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	g_idle_add(_emit_last_client, this);
}
