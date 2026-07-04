#ifndef __RF_RDP_CLIPBOARD_DISPLAY_H__
#define __RF_RDP_CLIPBOARD_DISPLAY_H__

#include <glib.h>

G_BEGIN_DECLS

const char *rf_rdp_clipboard_preferred_gdk_backend(
	const char *configured_backend,
	const char *wayland_display,
	const char *display
);

void rf_rdp_clipboard_configure_gdk_backend(void);

G_END_DECLS

#endif
