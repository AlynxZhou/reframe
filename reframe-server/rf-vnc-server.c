#include "rf-vnc-server.h"

G_DEFINE_TYPE(RfVNCServer, rf_vnc_server, RF_TYPE_REMOTE_SERVER)

static void rf_vnc_server_class_init(RfVNCServerClass *klass)
{
	(void)klass;
}

static void rf_vnc_server_init(RfVNCServer *this)
{
	(void)this;
}

void rf_vnc_server_start(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_start(RF_REMOTE_SERVER(this));
}

bool rf_vnc_server_is_running(RfVNCServer *this)
{
	g_return_val_if_fail(RF_IS_VNC_SERVER(this), false);

	return rf_remote_server_is_running(RF_REMOTE_SERVER(this));
}

void rf_vnc_server_stop(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_stop(RF_REMOTE_SERVER(this));
}

void rf_vnc_server_set_desktop_name(RfVNCServer *this, const char *desktop_name)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_set_desktop_name(
		RF_REMOTE_SERVER(this), desktop_name
	);
}

void rf_vnc_server_send_clipboard_text(RfVNCServer *this, const char *text)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_send_clipboard_text(RF_REMOTE_SERVER(this), text);
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

	rf_remote_server_update(
		RF_REMOTE_SERVER(this), buf, width, height, damage
	);
}

void rf_vnc_server_flush(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_flush(RF_REMOTE_SERVER(this));
}

void rf_vnc_server_handle_resize_event(
	RfVNCServer *this,
	unsigned int width,
	unsigned int height
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_resize_event(
		RF_REMOTE_SERVER(this), width, height
	);
}

void rf_vnc_server_handle_keysym_event(
	RfVNCServer *this,
	uint32_t keysym,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_keysym_event(
		RF_REMOTE_SERVER(this), keysym, down
	);
}

void rf_vnc_server_handle_keycode_event(
	RfVNCServer *this,
	uint32_t keycode,
	bool down
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_keycode_event(
		RF_REMOTE_SERVER(this), keycode, down
	);
}

void rf_vnc_server_handle_pointer_event(
	RfVNCServer *this,
	double rx,
	double ry,
	uint32_t mask
)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_pointer_event(
		RF_REMOTE_SERVER(this), rx, ry, mask
	);
}

void rf_vnc_server_handle_clipboard_text(RfVNCServer *this, const char *text)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_clipboard_text(RF_REMOTE_SERVER(this), text);
}

void rf_vnc_server_handle_first_client(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_first_client(RF_REMOTE_SERVER(this));
}

void rf_vnc_server_handle_last_client(RfVNCServer *this)
{
	g_return_if_fail(RF_IS_VNC_SERVER(this));

	rf_remote_server_handle_last_client(RF_REMOTE_SERVER(this));
}
