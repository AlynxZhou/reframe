#ifndef __RF_REMOTE_SERVER_H__
#define __RF_REMOTE_SERVER_H__

#include <stdbool.h>
#include <stdint.h>
#include <gio/gio.h>

#include "rf-common.h"
#include "rf-config.h"

G_BEGIN_DECLS

#define RF_TYPE_REMOTE_SERVER rf_remote_server_get_type()
G_DECLARE_DERIVABLE_TYPE(
	RfRemoteServer,
	rf_remote_server,
	RF,
	REMOTE_SERVER,
	GObject
)

struct _RfRemoteServerClass {
	GObjectClass parent_class;
	void (*start)(RfRemoteServer *this);
	bool (*is_running)(RfRemoteServer *this);
	void (*stop)(RfRemoteServer *this);
		void (*set_desktop_name)(RfRemoteServer *this, const char *desktop_name);
		void (*set_rdp_clipboard_socket_path)(
			RfRemoteServer *this,
			const char *socket_path
		);
		void (*send_clipboard_text)(RfRemoteServer *this, const char *text);
	bool (*should_render_frame)(RfRemoteServer *this);
	void (*update)(
		RfRemoteServer *this,
		GByteArray *buf,
		unsigned int width,
		unsigned int height,
		const struct rf_rect *damage
	);
	void (*flush)(RfRemoteServer *this);
};

typedef RfRemoteServer *(*RfRemoteServerNewFunc)(RfConfig *config);

void rf_remote_server_start(RfRemoteServer *this);
bool rf_remote_server_is_running(RfRemoteServer *this);
void rf_remote_server_stop(RfRemoteServer *this);
void rf_remote_server_set_desktop_name(
	RfRemoteServer *this,
	const char *desktop_name
);
void rf_remote_server_set_rdp_clipboard_socket_path(
	RfRemoteServer *this,
	const char *socket_path
);
void rf_remote_server_send_clipboard_text(RfRemoteServer *this, const char *text);
bool rf_remote_server_should_render_frame(RfRemoteServer *this);
void rf_remote_server_update(
	RfRemoteServer *this,
	GByteArray *buf,
	unsigned int width,
	unsigned int height,
	const struct rf_rect *damage
);
void rf_remote_server_flush(RfRemoteServer *this);
void rf_remote_server_handle_resize_event(
	RfRemoteServer *this,
	unsigned int width,
	unsigned int height
);
void rf_remote_server_handle_keysym_event(
	RfRemoteServer *this,
	uint32_t keysym,
	bool down
);
void rf_remote_server_handle_keycode_event(
	RfRemoteServer *this,
	uint32_t keycode,
	bool down
);
void rf_remote_server_handle_pointer_event(
	RfRemoteServer *this,
	double rx,
	double ry,
	uint32_t mask
);
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
);
void rf_remote_server_handle_clipboard_text(
	RfRemoteServer *this,
	const char *text
);
void rf_remote_server_handle_first_client(RfRemoteServer *this);
void rf_remote_server_handle_last_client(RfRemoteServer *this);

G_END_DECLS

#endif
