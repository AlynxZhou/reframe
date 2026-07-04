#include "rf-rdp-clipboard-display.h"

#include <stdbool.h>

const char *rf_rdp_clipboard_preferred_gdk_backend(
	const char *configured_backend,
	const char *wayland_display,
	const char *display
)
{
	if (configured_backend != NULL && configured_backend[0] != '\0')
		return configured_backend;
	if (wayland_display != NULL && wayland_display[0] != '\0')
		return "wayland,x11";
	if (display != NULL && display[0] != '\0')
		return "x11";
	return NULL;
}

void rf_rdp_clipboard_configure_gdk_backend(void)
{
	const char *backend = rf_rdp_clipboard_preferred_gdk_backend(
		g_getenv("GDK_BACKEND"),
		g_getenv("WAYLAND_DISPLAY"),
		g_getenv("DISPLAY")
	);

	if (backend != NULL)
		g_setenv("GDK_BACKEND", backend, false);
}
