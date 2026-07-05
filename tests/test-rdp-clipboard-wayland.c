#include <assert.h>
#include <string.h>

#include "rf-rdp-clipboard-image.h"
#include "rf-rdp-clipboard-wayland.h"

static void test_picks_html_before_text(void)
{
	const char *types[] = {
		"text/plain",
		"image/png",
		"text/html;charset=utf-8",
		NULL
	};

	assert(strcmp(
		       rf_rdp_clipboard_wayland_pick_html_mime(types),
		       "text/html;charset=utf-8"
	       ) == 0);
	assert(strcmp(
		       rf_rdp_clipboard_wayland_pick_text_mime(types),
		       "text/plain"
	       ) == 0);
	assert(strcmp(
		       rf_rdp_clipboard_wayland_pick_image_mime(types),
		       "image/png"
	       ) == 0);
}

static void test_picks_bmp_image_without_png(void)
{
	const char *types[] = {
		"text/plain",
		"image/jpeg",
		"image/bmp",
		"image/x-MS-bmp",
		NULL
	};
	const char *picked = rf_rdp_clipboard_wayland_pick_image_mime(types);

	assert(picked != NULL);
	assert(strcmp(picked, "image/jpeg") == 0);
}

static void test_payload_bytes_for_html_and_text(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t html[] = "<p>Hello <b>RDP</b></p>";
	g_autoptr(GBytes) html_bytes = NULL;
	g_autoptr(GBytes) text_bytes = NULL;
	g_autoptr(GBytes) context_bytes = NULL;
	gsize length = 0;
	const char *data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_text(&payload, "Hello RDP"));
	assert(rf_clipboard_rich_payload_set_html(&payload, html, sizeof(html) - 1));

	html_bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"text/html"
	);
	data = g_bytes_get_data(html_bytes, &length);
	assert(length == sizeof(html) - 1);
	assert(memcmp(data, html, length) == 0);

	text_bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"text/plain;charset=utf-8"
	);
	data = g_bytes_get_data(text_bytes, &length);
	assert(length == strlen("Hello RDP"));
	assert(memcmp(data, "Hello RDP", length) == 0);

	context_bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"text/_moz_htmlcontext"
	);
	data = g_bytes_get_data(context_bytes, &length);
	assert(length == strlen("<html><body>"));
	assert(memcmp(data, "<html><body>", length) == 0);
}

static void test_payload_bytes_for_image_png(void)
{
	g_auto(RfClipboardRichPayload) payload;
	g_auto(RfClipboardRichPayload) decoded;
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0xff, 0x00, 0x80
	};
	const uint8_t png_signature[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
	};
	g_autoptr(GBytes) png_bytes = NULL;
	gsize length = 0;
	const uint8_t *data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		rgba,
		sizeof(rgba),
		2,
		1,
		2 * 4
	));

	png_bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"image/png"
	);
	assert(png_bytes != NULL);
	data = g_bytes_get_data(png_bytes, &length);
	assert(length > sizeof(png_signature));
	assert(memcmp(data, png_signature, sizeof(png_signature)) == 0);

	rf_clipboard_rich_payload_init(&decoded);
	assert(rf_rdp_clipboard_image_payload_from_png(data, length, &decoded));
	assert(decoded.image_rgba != NULL);
	assert(decoded.image_width == 2);
	assert(decoded.image_height == 1);
	assert(decoded.image_stride == 2 * 4);
	assert(decoded.image_rgba->len == sizeof(rgba));
	assert(memcmp(decoded.image_rgba->data, rgba, sizeof(rgba)) == 0);
}

static void test_payload_bytes_for_image_html_fallback(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0xff, 0x00, 0x80
	};
	g_autoptr(GBytes) html_bytes = NULL;
	gsize length = 0;
	const char *data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		rgba,
		sizeof(rgba),
		2,
		1,
		2 * 4
	));

	html_bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"text/html"
	);
	assert(html_bytes != NULL);
	data = g_bytes_get_data(html_bytes, &length);
	assert(length > 0);
	assert(g_strstr_len(data, length, "<img") != NULL);
	assert(g_strstr_len(data, length, "src=\"data:image/png;base64,") != NULL);
}

static void test_payload_bytes_for_image_bmp(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t rgba[] = {
		0x10, 0x20, 0x30, 0xff,
		0x40, 0x50, 0x60, 0x80
	};
	g_autoptr(GBytes) bmp_bytes = NULL;
	gsize length = 0;
	const uint8_t *data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		rgba,
		sizeof(rgba),
		2,
		1,
		2 * 4
	));

	bmp_bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"image/bmp"
	);
	assert(bmp_bytes != NULL);
	data = g_bytes_get_data(bmp_bytes, &length);
	assert(length == 14 + 40 + sizeof(rgba));
	assert(data[0] == 'B');
	assert(data[1] == 'M');
	assert(data[54] == 0x30);
	assert(data[55] == 0x20);
	assert(data[56] == 0x10);
	assert(data[57] == 0xff);
}

static void test_payload_bytes_for_extra_image_formats(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0x00, 0xff, 0xff
	};
	const struct {
		const char *mime;
		const uint8_t *signature;
		size_t signature_length;
	} cases[] = {
		{ "image/tiff", (const uint8_t *)"II", 2 },
		{ "image/jpeg", (const uint8_t *)"\xff\xd8", 2 },
		{ "image/webp", (const uint8_t *)"RIFF", 4 }
	};

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		rgba,
		sizeof(rgba),
		2,
		1,
		2 * 4
	));

	for (size_t i = 0; i < G_N_ELEMENTS(cases); ++i) {
		g_autoptr(GBytes) bytes =
			rf_rdp_clipboard_wayland_payload_bytes_for_mime(
				&payload,
				cases[i].mime
			);
		gsize length = 0;
		const uint8_t *data = NULL;

		assert(bytes != NULL);
		data = g_bytes_get_data(bytes, &length);
		assert(length > cases[i].signature_length);
		assert(memcmp(data, cases[i].signature, cases[i].signature_length) == 0);
	}
}

extern bool rf_rdp_clipboard_image_payload_from_bytes(
	const uint8_t *image,
	size_t image_length,
	struct rf_clipboard_rich_payload *payload
);

static void test_decodes_non_png_wayland_image_payload(void)
{
	g_auto(RfClipboardRichPayload) payload;
	g_auto(RfClipboardRichPayload) decoded;
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0x00, 0xff, 0xff
	};
	g_autoptr(GBytes) webp = NULL;
	gsize length = 0;
	const uint8_t *data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		rgba,
		sizeof(rgba),
		2,
		1,
		2 * 4
	));
	webp = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&payload,
		"image/webp"
	);
	assert(webp != NULL);
	data = g_bytes_get_data(webp, &length);

	rf_clipboard_rich_payload_init(&decoded);
	assert(rf_rdp_clipboard_image_payload_from_bytes(data, length, &decoded));
	assert(decoded.image_rgba != NULL);
	assert(decoded.image_width == 2);
	assert(decoded.image_height == 1);
	assert(decoded.image_stride == 2 * 4);
}

int main(void)
{
	test_picks_html_before_text();
	test_picks_bmp_image_without_png();
	test_payload_bytes_for_html_and_text();
	test_payload_bytes_for_image_png();
	test_payload_bytes_for_image_html_fallback();
	test_payload_bytes_for_image_bmp();
	test_payload_bytes_for_extra_image_formats();
	test_decodes_non_png_wayland_image_payload();
	return 0;
}
