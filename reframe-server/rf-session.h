#ifndef __RF_SESSION_H__
#define __RF_SESSION_H__

#include <stdbool.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define RF_TYPE_SESSION rf_session_get_type()
G_DECLARE_FINAL_TYPE(RfSession, rf_session, RF, SESSION, GSocketService)

RfSession *rf_session_new(void);
void rf_session_set_socket_path(RfSession *this, const char *socket_path);
int rf_session_start(RfSession *this);
bool rf_session_is_running(RfSession *this);
void rf_session_stop(RfSession *this);
void rf_session_send_clipboard_text_msg(
	RfSession *this,
	const char *clipboard_text
);

G_END_DECLS

#endif
