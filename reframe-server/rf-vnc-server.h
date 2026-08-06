#ifndef __RF_VNC_SERVER_H__
#define __RF_VNC_SERVER_H__

#include <stdint.h>
#include <stdbool.h>
#include <gio/gio.h>

#include "rf-config.h"
#include "rf-common.h"

G_BEGIN_DECLS

#define RF_TYPE_VNC_SERVER rf_vnc_server_get_type()
G_DECLARE_DERIVABLE_TYPE(RfVNCServer, rf_vnc_server, RF, VNC_SERVER, GObject)

struct _RfVNCServerClass {
	GObjectClass parent_class;
	/**
	 * Start to listen to VNC connections.
	 *
	 * You should load configurations and init states here.
	 */
	void (*start)(RfVNCServer *this);
	/**
	 * Stop to listen to VNC connections.
	 *
	 * You should call rf_vnc_server_flush() to disconnect all connections.
	 */
	void (*stop)(RfVNCServer *this);
	/**
	 * Update the VNC buffer and state.
	 *
	 * If @buf is %NULL, you should ignore it, and update the VNC state only.
	 *
	 * If @damage is %NULL, the whole buffer is damaged.
	 */
	void (*update)(
		RfVNCServer *this,
		GByteArray *buf,
		unsigned int width,
		unsigned int height,
		const struct rf_rect *damage
	);
	/**
	 * Disconnect all VNC connections.
	 */
	void (*flush)(RfVNCServer *this);
	void (*set_desktop_name)(RfVNCServer *this, const char *desktop_name);
	void (*send_clipboard_text)(RfVNCServer *this, const char *text);
};

typedef RfVNCServer *(*RfVNCServerNew)(RfConfig *config);

void rf_vnc_server_start(RfVNCServer *this);
void rf_vnc_server_stop(RfVNCServer *this);
void rf_vnc_server_update(
	RfVNCServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage
);
void rf_vnc_server_flush(RfVNCServer *this);
void rf_vnc_server_set_desktop_name(RfVNCServer *this, const char *desktop_name);
void rf_vnc_server_send_clipboard_text(RfVNCServer *this, const char *text);
void rf_vnc_server_set_resize(RfVNCServer *this, bool resize);
void rf_vnc_server_set_share(RfVNCServer *this, bool share);
/**
 * You should call this when client sends resize request, and decide to allow or
 * prohibit this resize request by the return value.
 */
bool rf_vnc_server_handle_resize_event(
	RfVNCServer *this,
	unsigned int width,
	unsigned int height
);
void rf_vnc_server_handle_keysym_event(
	RfVNCServer *this,
	uint32_t keysym,
	bool down
);
void rf_vnc_server_handle_keycode_event(
	RfVNCServer *this,
	uint32_t keycode,
	bool down
);
void rf_vnc_server_handle_pointer_event(
	RfVNCServer *this,
	double rx,
	double ry,
	uint32_t mask
);
void rf_vnc_server_handle_clipboard_text(RfVNCServer *this, const char *text);
/**
 * You should call this when a new client is incoming, and decide to accept or
 * refuse this client by the return value.
 */
bool rf_vnc_server_handle_new_client(RfVNCServer *this);
/**
 * You should call this when a client is gone.
 */
void rf_vnc_server_handle_client_gone(RfVNCServer *this);

G_END_DECLS

#endif
