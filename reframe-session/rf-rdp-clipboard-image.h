#ifndef __RF_RDP_CLIPBOARD_IMAGE_H__
#define __RF_RDP_CLIPBOARD_IMAGE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <gdk/gdk.h>
#include <glib.h>

#include "rf-clipboard-rich.h"

G_BEGIN_DECLS

GBytes *rf_rdp_clipboard_image_png_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
);
GBytes *rf_rdp_clipboard_image_bmp_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
);
GBytes *rf_rdp_clipboard_image_tiff_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
);
GBytes *rf_rdp_clipboard_image_jpeg_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
);
GBytes *rf_rdp_clipboard_image_webp_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
);
GBytes *rf_rdp_clipboard_image_html_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
);
bool rf_rdp_clipboard_image_payload_from_texture(
	GdkTexture *texture,
	struct rf_clipboard_rich_payload *payload
);
bool rf_rdp_clipboard_image_payload_from_png(
	const uint8_t *png,
	size_t png_length,
	struct rf_clipboard_rich_payload *payload
);
bool rf_rdp_clipboard_image_payload_from_bytes(
	const uint8_t *image,
	size_t image_length,
	struct rf_clipboard_rich_payload *payload
);

G_END_DECLS

#endif
