#include <assert.h>
#include <stdbool.h>
#include <string.h>

#include <gio/gio.h>
#include <gtk/gtk.h>

#include "rf-clipboard-rich.h"
#include "rf-rdp-clipboard-provider.h"

struct write_mime_request {
	GMainLoop *loop;
	bool ok;
	GError *error;
};

static void on_write_mime_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct write_mime_request *request = data;

	request->ok = gdk_content_provider_write_mime_type_finish(
		GDK_CONTENT_PROVIDER(source_object),
		res,
		&request->error
	);
	g_main_loop_quit(request->loop);
}

static GByteArray *write_mime_type(
	GdkContentProvider *provider,
	const char *mime_type
)
{
	g_autoptr(GOutputStream) output = g_memory_output_stream_new_resizable();
	g_autoptr(GMainLoop) loop = g_main_loop_new(NULL, FALSE);
	struct write_mime_request request = {
		.loop = loop
	};
	const void *data = NULL;
	size_t length = 0;
	GByteArray *bytes = NULL;

	gdk_content_provider_write_mime_type_async(
		provider,
		mime_type,
		output,
		G_PRIORITY_DEFAULT,
		NULL,
		on_write_mime_finish,
		&request
	);
	g_main_loop_run(loop);
	assert(request.ok);
	assert(request.error == NULL);

	data = g_memory_output_stream_get_data(G_MEMORY_OUTPUT_STREAM(output));
	length = g_memory_output_stream_get_data_size(G_MEMORY_OUTPUT_STREAM(output));
	bytes = g_byte_array_sized_new(length);
	g_byte_array_append(bytes, data, length);
	return bytes;
}

static void test_html_provider_exposes_rich_and_plain_formats(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t html[] = "<p>Hello <b>RDP</b></p>";
	g_autoptr(GdkContentProvider) provider = NULL;
	g_autoptr(GdkContentFormats) formats = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_text(&payload, "Hello RDP"));
	assert(rf_clipboard_rich_payload_set_html(&payload, html, strlen((const char *)html)));

	provider = rf_rdp_clipboard_content_provider_new(&payload);
	assert(provider != NULL);
	formats = gdk_content_provider_ref_formats(provider);
	assert(formats != NULL);
	assert(gdk_content_formats_contain_mime_type(formats, "text/html"));
	assert(gdk_content_formats_contain_mime_type(formats, "text/html;charset=utf-8"));
	assert(gdk_content_formats_contain_mime_type(formats, "text/_moz_htmlcontext"));
	assert(gdk_content_formats_contain_mime_type(formats, "text/_moz_htmlinfo"));
	assert(gdk_content_formats_contain_mime_type(formats, "text/plain"));
	assert(gdk_content_formats_contain_mime_type(formats, "text/plain;charset=utf-8"));
	assert(gdk_content_formats_contain_gtype(formats, G_TYPE_STRING));
}

static void test_html_provider_writes_rich_and_plain_data(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t html[] = "<p>Hello <b>RDP</b></p>";
	g_autoptr(GdkContentProvider) provider = NULL;
	g_autoptr(GByteArray) html_data = NULL;
	g_autoptr(GByteArray) plain_data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_text(&payload, "Hello RDP"));
	assert(rf_clipboard_rich_payload_set_html(&payload, html, strlen((const char *)html)));

	provider = rf_rdp_clipboard_content_provider_new(&payload);
	assert(provider != NULL);

	html_data = write_mime_type(provider, "text/html");
	assert(html_data->len == strlen((const char *)html));
	assert(memcmp(html_data->data, html, html_data->len) == 0);

	plain_data = write_mime_type(provider, "text/plain;charset=utf-8");
	assert(plain_data->len == strlen("Hello RDP"));
	assert(memcmp(plain_data->data, "Hello RDP", plain_data->len) == 0);
}

static void test_image_provider_exposes_png_and_writes_png(void)
{
	g_auto(RfClipboardRichPayload) payload;
	const uint8_t rgba[] = {
		0xff, 0x00, 0x00, 0xff,
		0x00, 0xff, 0x00, 0x80
	};
	const uint8_t png_signature[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
	};
	g_autoptr(GdkContentProvider) provider = NULL;
	g_autoptr(GdkContentFormats) formats = NULL;
	g_autoptr(GByteArray) png_data = NULL;
	g_autoptr(GByteArray) bmp_data = NULL;

	rf_clipboard_rich_payload_init(&payload);
	assert(rf_clipboard_rich_payload_set_image_rgba(
		&payload,
		rgba,
		sizeof(rgba),
		2,
		1,
		2 * 4
	));

	provider = rf_rdp_clipboard_content_provider_new(&payload);
	assert(provider != NULL);
	formats = gdk_content_provider_ref_formats(provider);
	assert(formats != NULL);
	assert(gdk_content_formats_contain_mime_type(formats, "image/png"));
	assert(gdk_content_formats_contain_mime_type(formats, "text/html"));
	assert(gdk_content_formats_contain_mime_type(formats, "image/tiff"));
	assert(gdk_content_formats_contain_mime_type(formats, "image/jpeg"));
	assert(gdk_content_formats_contain_mime_type(formats, "image/webp"));
	assert(gdk_content_formats_contain_mime_type(formats, "image/bmp"));
	assert(gdk_content_formats_contain_mime_type(formats, "image/x-bmp"));
	assert(gdk_content_formats_contain_mime_type(formats, "image/x-MS-bmp"));

	png_data = write_mime_type(provider, "image/png");
	assert(png_data->len > sizeof(png_signature));
	assert(memcmp(png_data->data, png_signature, sizeof(png_signature)) == 0);

	g_autoptr(GByteArray) html_data = write_mime_type(provider, "text/html");
	assert(g_strstr_len(
		(const char *)html_data->data,
		html_data->len,
		"src=\"data:image/png;base64,"
	) != NULL);

	g_autoptr(GByteArray) tiff_data = write_mime_type(provider, "image/tiff");
	assert(tiff_data->len > 2);
	assert(memcmp(tiff_data->data, "II", 2) == 0);

	g_autoptr(GByteArray) jpeg_data = write_mime_type(provider, "image/jpeg");
	assert(jpeg_data->len > 2);
	assert(jpeg_data->data[0] == 0xff);
	assert(jpeg_data->data[1] == 0xd8);

	g_autoptr(GByteArray) webp_data = write_mime_type(provider, "image/webp");
	assert(webp_data->len > 4);
	assert(memcmp(webp_data->data, "RIFF", 4) == 0);

	bmp_data = write_mime_type(provider, "image/bmp");
	assert(bmp_data->len == 14 + 40 + sizeof(rgba));
	assert(bmp_data->data[0] == 'B');
	assert(bmp_data->data[1] == 'M');
	assert(bmp_data->data[54] == 0x00);
	assert(bmp_data->data[55] == 0x00);
	assert(bmp_data->data[56] == 0xff);
	assert(bmp_data->data[57] == 0xff);
}

int main(void)
{
	test_html_provider_exposes_rich_and_plain_formats();
	test_html_provider_writes_rich_and_plain_data();
	test_image_provider_exposes_png_and_writes_png();
	return 0;
}
