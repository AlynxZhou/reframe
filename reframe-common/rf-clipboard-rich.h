#ifndef __RF_CLIPBOARD_RICH_H__
#define __RF_CLIPBOARD_RICH_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <glib.h>

G_BEGIN_DECLS

#define RF_CLIPBOARD_RICH_MAX_BYTES (64u * 1024u * 1024u)

enum rf_clipboard_rich_format {
	RF_CLIPBOARD_RICH_FORMAT_TEXT = 1,
	RF_CLIPBOARD_RICH_FORMAT_HTML = 2,
	RF_CLIPBOARD_RICH_FORMAT_IMAGE_RGBA = 3
};

typedef struct rf_clipboard_rich_payload {
	char *text;
	GByteArray *html;
	GByteArray *image_rgba;
	uint32_t image_width;
	uint32_t image_height;
	size_t image_stride;
} RfClipboardRichPayload;

void rf_clipboard_rich_payload_init(struct rf_clipboard_rich_payload *payload);
void rf_clipboard_rich_payload_clear(struct rf_clipboard_rich_payload *payload);
bool rf_clipboard_rich_payload_set_text(
	struct rf_clipboard_rich_payload *payload,
	const char *text
);
bool rf_clipboard_rich_payload_set_html(
	struct rf_clipboard_rich_payload *payload,
	const uint8_t *html,
	size_t html_length
);
bool rf_clipboard_rich_payload_set_image_rgba(
	struct rf_clipboard_rich_payload *payload,
	const uint8_t *rgba,
	size_t rgba_length,
	uint32_t width,
	uint32_t height,
	size_t stride
);
bool rf_clipboard_rich_payload_has_data(
	const struct rf_clipboard_rich_payload *payload
);
GByteArray *rf_clipboard_rich_payload_serialize(
	const struct rf_clipboard_rich_payload *payload
);
bool rf_clipboard_rich_wire_equal(
	const GByteArray *left,
	const GByteArray *right
);
bool rf_clipboard_rich_payload_parse(
	const uint8_t *data,
	size_t length,
	struct rf_clipboard_rich_payload *payload
);

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC(
	RfClipboardRichPayload,
	rf_clipboard_rich_payload_clear
)

G_END_DECLS

#endif
