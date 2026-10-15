#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "rf-common.h"
#include "rf-vnc-server.h"

#define MOD_SHIFT XKB_MOD_NAME_SHIFT

// Old XKBcommon does not define `XKB_MOD_NAME_MOD5`.
#if defined(XKB_MOD_NAME_MOD5)
#	define MOD_ALTGR XKB_MOD_NAME_MOD5
#else
#	define MOD_ALTGR "Mod5"
#endif

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

typedef struct _RfVNCServerPrivate {
	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	unsigned int client_idle_id;
	bool connected;
	unsigned int clients;
	bool resize;
	bool share;
	bool running;
	// Modifiers currently held by the client, to synthesize only the missing
	// ones when a keysym needs another shift level.
	bool client_shift;
	bool client_altgr;
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

static void finalize(GObject *o)
{
	RfVNCServer *this = RF_VNC_SERVER(o);
	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (priv->client_idle_id != 0) {
		g_source_remove(priv->client_idle_id);
		priv->client_idle_id = 0;
	}
	g_clear_pointer(&priv->xkb_keymap, xkb_keymap_unref);
	g_clear_pointer(&priv->xkb_context, xkb_context_unref);

	G_OBJECT_CLASS(rf_vnc_server_parent_class)->finalize(o);
}

static void rf_vnc_server_class_init(RfVNCServerClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = finalize;

	klass->start = NULL;
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
		g_error("Input: Failed to create XKB context.");
}

void rf_vnc_server_start(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);

	g_return_if_fail(klass->start != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (priv->running)
		return;

	priv->running = true;

	priv->connected = false;
	priv->clients = 0;
	priv->resize = true;
	priv->share = true;
	priv->client_shift = false;
	priv->client_altgr = false;

	klass->start(this);

	// Ensure we always have a fallback keymap, though it could be incorrect.
	if (priv->xkb_keymap == NULL)
		rf_vnc_server_set_xkb_keymap(this, NULL, NULL, NULL, NULL, NULL);
	if (priv->xkb_keymap == NULL)
		g_error("Input: Failed to create fallback XKB keymap.");
}

void rf_vnc_server_stop(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);

	g_return_if_fail(klass->stop != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	klass->stop(this);

	g_clear_pointer(&priv->xkb_keymap, xkb_keymap_unref);

	priv->running = false;
}

void rf_vnc_server_set_desktop_name(RfVNCServer *this, const char *desktop_name)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(desktop_name != NULL);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);

	g_return_if_fail(klass->set_desktop_name != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	klass->set_desktop_name(this, desktop_name);
}

void rf_vnc_server_send_clipboard_text(RfVNCServer *this, const char *text)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(text != NULL);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);

	g_return_if_fail(klass->send_clipboard_text != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	klass->send_clipboard_text(this, text);
}

void rf_vnc_server_update(
	RfVNCServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(width > 0 && height > 0);

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);

	g_return_if_fail(klass->update != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	klass->update(this, buf, width, height, damage);
}

void rf_vnc_server_flush(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerClass *klass = RF_VNC_SERVER_GET_CLASS(this);

	g_return_if_fail(klass->flush != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	klass->flush(this);
}

void rf_vnc_server_set_resize(RfVNCServer *this, bool resize)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	priv->resize = resize;
	g_message(
		"VNC: Client resizing will be %s.",
		priv->resize ? "allowed" : "prohibited"
	);
}

void rf_vnc_server_set_share(RfVNCServer *this, bool share)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	priv->share = share;
	g_message(
		"VNC: Multiple connections will be %s.",
		priv->share ? "allowed" : "prohibited"
	);
}

void rf_vnc_server_set_xkb_keymap(
	RfVNCServer *this,
	const char *rules,
	const char *model,
	const char *layout,
	const char *variant,
	const char *options
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	struct xkb_rule_names names = { rules, model, layout, variant, options };
	struct xkb_keymap *xkb_keymap = xkb_keymap_new_from_names(
		priv->xkb_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS
	);
	if (xkb_keymap == NULL) {
		g_warning(
			"Input: Failed to create XKB keymap with rules %s, model %s, layout %s, variant %s, options %s.",
			rules,
			model,
			layout,
			variant,
			options
		);
		return;
	}

	g_clear_pointer(&priv->xkb_keymap, xkb_keymap_unref);
	priv->xkb_keymap = xkb_keymap;
	g_message(
		"Input: Created XKB keymap with rules %s, model %s, layout %s, variant %s, options %s.",
		rules,
		model,
		layout,
		variant,
		options
	);
}

bool rf_vnc_server_handle_resize_event(
	RfVNCServer *this,
	unsigned int width,
	unsigned int height
)
{
	g_return_val_if_fail(RF_IS_VNC_SERVER(this), false);
	g_return_val_if_fail(width > 0 && height > 0, false);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return false;

	if (!priv->resize)
		return false;

	g_debug("VNC: Received resize event for width %d and height %d.",
		width,
		height);
	g_signal_emit(this, sigs[SIG_RESIZE_EVENT], 0, width, height);

	return true;
}

struct iterate_data {
	xkb_keysym_t keysym;
	xkb_keycode_t keycode;
	xkb_level_index_t level;
	xkb_mod_mask_t mask;
	int cost;
};

static int popcount_mask(xkb_mod_mask_t m)
{
	int n = 0;
	for (; m != 0; m &= m - 1)
		++n;
	return n;
}

// Find the key producing the keysym with the fewest modifiers: on layouts like
// French AZERTY, digits are on the Shift level and many symbols on the AltGr
// level, so the level found must be applied, not only the keycode.
static void iterate_keys(struct xkb_keymap *map, xkb_keycode_t key, void *data)
{
	struct iterate_data *idata = data;

	// Only keys the uinput device declares can be sent.
	if (RF_KEY_CODE_XKB_TO_EV(key) >= RF_KEYBOARD_MAX)
		return;

	xkb_level_index_t num_levels =
		xkb_keymap_num_levels_for_key(map, key, 0);
	for (xkb_level_index_t level = 0; level < num_levels; ++level) {
		const xkb_keysym_t *syms;
		const int num_syms = xkb_keymap_key_get_syms_by_level(
			map, key, 0, level, &syms
		);
		for (int k = 0; k < num_syms; ++k) {
			if (syms[k] != idata->keysym)
				continue;

			xkb_mod_mask_t masks[8];
			const size_t num_masks =
				xkb_keymap_key_get_mods_for_level(
					map,
					key,
					0,
					level,
					masks,
					G_N_ELEMENTS(masks)
				);
			xkb_mod_mask_t mask = num_masks > 0 ? masks[0] : 0;
			int cost = popcount_mask(mask);
			for (size_t m = 1; m < num_masks; ++m) {
				int c = popcount_mask(masks[m]);
				if (c < cost) {
					mask = masks[m];
					cost = c;
				}
			}
			if (idata->keycode == XKB_KEYCODE_INVALID ||
			    cost < idata->cost) {
				idata->keycode = key;
				idata->level = level;
				idata->mask = mask;
				idata->cost = cost;
			}
		}
	}
}

static bool is_shift_keysym(uint32_t keysym)
{
	return keysym == XKB_KEY_Shift_L || keysym == XKB_KEY_Shift_R;
}

static bool is_altgr_keysym(uint32_t keysym)
{
	return keysym == XKB_KEY_ISO_Level3_Shift ||
	       keysym == XKB_KEY_Mode_switch;
}

void rf_vnc_server_handle_keysym_event(
	RfVNCServer *this,
	uint32_t keysym,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	if (is_shift_keysym(keysym))
		priv->client_shift = down;
	else if (is_altgr_keysym(keysym))
		priv->client_altgr = down;

	struct iterate_data idata = {
		.keysym = keysym,
		.keycode = XKB_KEYCODE_INVALID,
		.level = 0,
		.mask = 0,
		.cost = 0,
	};
	xkb_keymap_key_for_each(priv->xkb_keymap, iterate_keys, &idata);
	if (idata.keycode == XKB_KEYCODE_INVALID) {
		g_warning(
			"Input: Failed to find keysym %04x in keymap.", keysym
		);
		return;
	}
	const uint32_t keycode = RF_KEY_CODE_XKB_TO_EV(idata.keycode);
	g_debug("Input: Received key %s for keysym %04x and keycode %u at level %u.",
		down ? "down" : "up",
		keysym,
		keycode,
		idata.level);

	// We don't need to inject Shift/AltGr and key releases.
	const bool can_inject = down && !is_shift_keysym(keysym) &&
				!is_altgr_keysym(keysym);

	const xkb_mod_index_t shift_index =
		xkb_keymap_mod_get_index(priv->xkb_keymap, MOD_SHIFT);
	const xkb_mod_index_t altgr_index =
		xkb_keymap_mod_get_index(priv->xkb_keymap, MOD_ALTGR);
	const bool need_shift = shift_index != XKB_MOD_INVALID &&
				(idata.mask & (1u << shift_index));
	const bool need_altgr = altgr_index != XKB_MOD_INVALID &&
				(idata.mask & (1u << altgr_index));

	// Temporarily bring the modifiers to the state the level needs, then
	// restore what the client holds.
	const bool inject_shift = can_inject && need_shift &&
				  !priv->client_shift;
	const bool inject_altgr = can_inject && need_altgr &&
				  !priv->client_altgr;

	if (inject_shift)
		g_signal_emit(
			this,
			sigs[SIG_KEYBOARD_EVENT],
			0,
			KEY_LEFTSHIFT,
			need_shift
		);
	if (inject_altgr)
		g_signal_emit(
			this,
			sigs[SIG_KEYBOARD_EVENT],
			0,
			KEY_RIGHTALT,
			need_altgr
		);

	g_signal_emit(this, sigs[SIG_KEYBOARD_EVENT], 0, keycode, down);

	if (inject_altgr)
		g_signal_emit(
			this,
			sigs[SIG_KEYBOARD_EVENT],
			0,
			KEY_RIGHTALT,
			priv->client_altgr
		);
	if (inject_shift)
		g_signal_emit(
			this,
			sigs[SIG_KEYBOARD_EVENT],
			0,
			KEY_LEFTSHIFT,
			priv->client_shift
		);
}

void rf_vnc_server_handle_keycode_event(
	RfVNCServer *this,
	uint32_t keycode,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
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

void rf_vnc_server_handle_pointer_event(
	RfVNCServer *this,
	double rx,
	double ry,
	uint32_t mask
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	const bool left = mask & 1;
	const bool middle = mask & (1 << 1);
	const bool right = mask & (1 << 2);
	const bool wup = mask & (1 << 3);
	const bool wdown = mask & (1 << 4);
	const bool wleft = mask & (1 << 5);
	const bool wright = mask & (1 << 6);
	// Generally the 7th bit is reserved.
	const bool back = mask & (1 << 8);
	const bool forward = mask & (1 << 9);
	g_debug("Input: Received pointer at x %f and y %f, raw button %#x, "
		"left %s, middle %s, right %s, back %s, forward %s, "
		"wheel up %s, wheel down %s, wheel left %s, wheel right %s.",
		rx,
		ry,
		mask,
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

void rf_vnc_server_handle_clipboard_text(RfVNCServer *this, const char *text)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));
	g_return_if_fail(text != NULL);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	g_debug("VNC: Received clipboard text %s.", text);
	g_signal_emit(this, sigs[SIG_CLIPBOARD_TEXT], 0, text);
}

// Those signals must be delayed to next event by using `g_idle_add()`, because
// they are related to create/destroy resources and triggered during processing
// events, it is not OK to release resources while still processing events so we
// have to delay them until events are done.
//
// We must always update the connected status and always handle the latest
// connected status (not the connected status at dispatch time), otherwise a
// disconnecting last client will block a connecting first client.
//
// See <https://github.com/AlynxZhou/reframe/pull/26#issuecomment-4414394405>.

static void emit_client_signal(void *data)
{
	RfVNCServer *this = data;
	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	priv->client_idle_id = 0;

	if (!priv->running)
		return;

	g_debug("Signal: Emitting VNC %s client signal.",
		priv->connected ? "first" : "last");
	g_signal_emit(
		this,
		priv->connected ? sigs[SIG_FIRST_CLIENT] :
				  sigs[SIG_LAST_CLIENT],
		0
	);
}

static void handle_first_client(RfVNCServer *this)
{
	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	priv->connected = true;
	// Reset possibly dirty state.
	priv->client_shift = false;
	priv->client_altgr = false;

	if (priv->client_idle_id != 0)
		return;
	priv->client_idle_id = g_idle_add_once(emit_client_signal, this);
}

static void handle_last_client(RfVNCServer *this)
{
	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	priv->connected = false;

	if (priv->client_idle_id != 0)
		return;
	priv->client_idle_id = g_idle_add_once(emit_client_signal, this);
}

bool rf_vnc_server_handle_new_client(RfVNCServer *this)
{
	g_return_val_if_fail(RF_IS_VNC_SERVER(this), false);

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return false;

	++priv->clients;
	if (priv->clients == 1) {
		handle_first_client(this);
	} else if (!priv->share) {
		--priv->clients;
		g_message(
			"VNC: Incoming connection is refused because multiple connections are prohibited."
		);
		return false;
	}

	return true;
}

void rf_vnc_server_handle_client_gone(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	RfVNCServerPrivate *priv = rf_vnc_server_get_instance_private(this);

	if (!priv->running)
		return;

	if (priv->clients == 0)
		return;

	if (priv->clients == 1)
		handle_last_client(this);
	--priv->clients;
}
