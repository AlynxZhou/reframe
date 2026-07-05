#ifndef __RF_RDP_CLIPBOARD_WAYLAND_H__
#define __RF_RDP_CLIPBOARD_WAYLAND_H__

#include <stdbool.h>

#include <glib.h>

#include "rf-clipboard-rich.h"

G_BEGIN_DECLS

typedef struct rf_rdp_clipboard_wayland RfRdpClipboardWayland;

typedef void (*RfRdpClipboardWaylandPayloadFunc)(
	const struct rf_clipboard_rich_payload *payload,
	void *data
);

RfRdpClipboardWayland *rf_rdp_clipboard_wayland_new(
	RfRdpClipboardWaylandPayloadFunc on_payload,
	void *data
);
void rf_rdp_clipboard_wayland_free(RfRdpClipboardWayland *clipboard);
bool rf_rdp_clipboard_wayland_set_payload(
	RfRdpClipboardWayland *clipboard,
	const struct rf_clipboard_rich_payload *payload
);

const char *rf_rdp_clipboard_wayland_pick_html_mime(
	const char * const *mime_types
);
const char *rf_rdp_clipboard_wayland_pick_text_mime(
	const char * const *mime_types
);
const char *rf_rdp_clipboard_wayland_pick_image_mime(
	const char * const *mime_types
);
GBytes *rf_rdp_clipboard_wayland_payload_bytes_for_mime(
	const struct rf_clipboard_rich_payload *payload,
	const char *mime_type
);

G_END_DECLS

#endif
