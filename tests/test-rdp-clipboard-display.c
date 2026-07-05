#include <assert.h>
#include <string.h>

#include "rf-rdp-clipboard-display.h"

static void test_uses_configured_backend(void)
{
	assert(strcmp(
		       rf_rdp_clipboard_preferred_gdk_backend(
			       "x11",
			       "wayland-1",
			       ":0"
		       ),
		       "x11"
	       ) == 0);
}

static void test_prefers_wayland_with_x11_fallback(void)
{
	assert(strcmp(
		       rf_rdp_clipboard_preferred_gdk_backend(
			       NULL,
			       "wayland-1",
			       ":0"
		       ),
		       "wayland,x11"
	       ) == 0);
}

static void test_uses_x11_without_wayland(void)
{
	assert(strcmp(
		       rf_rdp_clipboard_preferred_gdk_backend(NULL, NULL, ":0"),
		       "x11"
	       ) == 0);
}

static void test_leaves_backend_unset_without_display(void)
{
	assert(rf_rdp_clipboard_preferred_gdk_backend(NULL, NULL, NULL) == NULL);
}

int main(void)
{
	test_uses_configured_backend();
	test_prefers_wayland_with_x11_fallback();
	test_uses_x11_without_wayland();
	test_leaves_backend_unset_without_display();
	return 0;
}
