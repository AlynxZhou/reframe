#include "rf-rdp-clipboard-image.h"

#include <limits.h>
#include <string.h>

static void write_u16_le(uint8_t *data, uint16_t value)
{
	data[0] = value & 0xff;
	data[1] = value >> 8;
}

static void write_u32_le(uint8_t *data, uint32_t value)
{
	data[0] = value & 0xff;
	data[1] = (value >> 8) & 0xff;
	data[2] = (value >> 16) & 0xff;
	data[3] = (value >> 24) & 0xff;
}

GBytes *rf_rdp_clipboard_image_png_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
)
{
	g_autoptr(GBytes) pixels = NULL;
	g_autoptr(GdkTexture) texture = NULL;

	if (payload == NULL || payload->image_rgba == NULL ||
	    payload->image_width == 0 || payload->image_height == 0 ||
	    payload->image_stride < (size_t)payload->image_width * 4)
		return NULL;

	pixels = g_bytes_new(payload->image_rgba->data, payload->image_rgba->len);
	texture = gdk_memory_texture_new(
		payload->image_width,
		payload->image_height,
		GDK_MEMORY_R8G8B8A8,
		pixels,
		payload->image_stride
	);
	if (texture == NULL)
		return NULL;
	return gdk_texture_save_to_png_bytes(texture);
}

static GBytes *image_bytes_from_payload_with_format(
	const struct rf_clipboard_rich_payload *payload,
	const char *format
)
{
	const bool alpha = g_strcmp0(format, "png") == 0;
	g_autoptr(GBytes) pixels = NULL;
	g_autoptr(GdkPixbuf) pixbuf = NULL;
	g_autofree gchar *buffer = NULL;
	g_autofree uint8_t *rgb = NULL;
	gsize buffer_length = 0;
	g_autoptr(GError) error = NULL;

	if (payload == NULL || payload->image_rgba == NULL ||
	    payload->image_width == 0 || payload->image_height == 0 ||
	    payload->image_width > INT_MAX || payload->image_height > INT_MAX ||
	    payload->image_stride > INT_MAX ||
	    payload->image_stride < (size_t)payload->image_width * 4)
		return NULL;
	if (payload->image_rgba->len <
	    (size_t)payload->image_height * payload->image_stride)
		return NULL;

	if (!alpha) {
		const size_t rgb_stride = (size_t)payload->image_width * 3;
		const size_t rgb_length = rgb_stride * payload->image_height;

		if ((size_t)payload->image_height > SIZE_MAX / rgb_stride ||
		    rgb_stride > INT_MAX)
			return NULL;
		rgb = g_malloc0(rgb_length);
		for (uint32_t y = 0; y < payload->image_height; ++y) {
			const uint8_t *src =
				payload->image_rgba->data + (size_t)y * payload->image_stride;
			uint8_t *dst = rgb + (size_t)y * rgb_stride;

			for (uint32_t x = 0; x < payload->image_width; ++x) {
				dst[x * 3 + 0] = src[x * 4 + 0];
				dst[x * 3 + 1] = src[x * 4 + 1];
				dst[x * 3 + 2] = src[x * 4 + 2];
			}
		}
		pixels = g_bytes_new_take(g_steal_pointer(&rgb), rgb_length);
		pixbuf = gdk_pixbuf_new_from_bytes(
			pixels,
			GDK_COLORSPACE_RGB,
			FALSE,
			8,
			(int)payload->image_width,
			(int)payload->image_height,
			(int)rgb_stride
		);
	} else {
		pixels = g_bytes_new(
			payload->image_rgba->data,
			payload->image_rgba->len
		);
		pixbuf = gdk_pixbuf_new_from_bytes(
			pixels,
			GDK_COLORSPACE_RGB,
			TRUE,
			8,
			(int)payload->image_width,
			(int)payload->image_height,
			(int)payload->image_stride
		);
	}
	if (pixbuf == NULL ||
	    !gdk_pixbuf_save_to_buffer(
		    pixbuf,
		    &buffer,
		    &buffer_length,
		    format,
		    &error,
		    NULL
	    ) ||
	    buffer == NULL || buffer_length == 0)
		return NULL;
	return g_bytes_new_take(g_steal_pointer(&buffer), buffer_length);
}

GBytes *rf_rdp_clipboard_image_tiff_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
)
{
	return image_bytes_from_payload_with_format(payload, "tiff");
}

GBytes *rf_rdp_clipboard_image_jpeg_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
)
{
	return image_bytes_from_payload_with_format(payload, "jpeg");
}

GBytes *rf_rdp_clipboard_image_webp_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
)
{
	return image_bytes_from_payload_with_format(payload, "webp");
}

GBytes *rf_rdp_clipboard_image_html_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
)
{
	g_autoptr(GBytes) png = NULL;
	g_autofree char *base64 = NULL;
	g_autofree char *html = NULL;
	gsize png_length = 0;
	const uint8_t *png_data = NULL;

	png = rf_rdp_clipboard_image_png_bytes_from_payload(payload);
	if (png == NULL)
		return NULL;
	png_data = g_bytes_get_data(png, &png_length);
	if (png_data == NULL || png_length == 0)
		return NULL;

	base64 = g_base64_encode(png_data, png_length);
	if (base64 == NULL)
		return NULL;
	html = g_strdup_printf(
		"<img src=\"data:image/png;base64,%s\" width=\"%u\" height=\"%u\">",
		base64,
		payload->image_width,
		payload->image_height
	);
	return g_bytes_new_take(g_steal_pointer(&html), strlen(html));
}

GBytes *rf_rdp_clipboard_image_bmp_bytes_from_payload(
	const struct rf_clipboard_rich_payload *payload
)
{
	const size_t file_header_length = 14;
	const size_t dib_header_length = 40;
	const size_t row_bytes = payload != NULL ? (size_t)payload->image_width * 4 : 0;
	size_t pixels_length = 0;
	size_t length = 0;
	g_autofree uint8_t *bmp = NULL;

	if (payload == NULL || payload->image_rgba == NULL ||
	    payload->image_width == 0 || payload->image_height == 0 ||
	    payload->image_width > INT32_MAX || payload->image_height > INT32_MAX ||
	    payload->image_stride < row_bytes || row_bytes == 0)
		return NULL;
	if ((size_t)payload->image_height > SIZE_MAX / row_bytes)
		return NULL;
	pixels_length = row_bytes * payload->image_height;
	if (payload->image_rgba->len < (size_t)payload->image_height *
	    payload->image_stride)
		return NULL;
	if (file_header_length + dib_header_length > SIZE_MAX - pixels_length ||
	    file_header_length + dib_header_length + pixels_length > G_MAXUINT32)
		return NULL;
	length = file_header_length + dib_header_length + pixels_length;
	bmp = g_malloc0(length);

	bmp[0] = 'B';
	bmp[1] = 'M';
	write_u32_le(bmp + 2, (uint32_t)length);
	write_u32_le(bmp + 10, file_header_length + dib_header_length);
	write_u32_le(bmp + 14, dib_header_length);
	write_u32_le(bmp + 18, payload->image_width);
	write_u32_le(bmp + 22, (uint32_t)(-(int32_t)payload->image_height));
	write_u16_le(bmp + 26, 1);
	write_u16_le(bmp + 28, 32);
	write_u32_le(bmp + 34, (uint32_t)pixels_length);

	for (uint32_t y = 0; y < payload->image_height; ++y) {
		const uint8_t *src =
			payload->image_rgba->data + (size_t)y * payload->image_stride;
		uint8_t *dst = bmp + file_header_length + dib_header_length +
			(size_t)y * row_bytes;

		for (uint32_t x = 0; x < payload->image_width; ++x) {
			dst[x * 4 + 0] = src[x * 4 + 2];
			dst[x * 4 + 1] = src[x * 4 + 1];
			dst[x * 4 + 2] = src[x * 4 + 0];
			dst[x * 4 + 3] = src[x * 4 + 3];
		}
	}

	return g_bytes_new_take(g_steal_pointer(&bmp), length);
}

static uint8_t unpremultiply_channel(uint8_t channel, uint8_t alpha)
{
	if (alpha == 0)
		return 0;
	if (alpha == 255)
		return channel;

	const unsigned int value = ((unsigned int)channel * 255u + alpha / 2u) /
		alpha;
	return value > 255u ? 255u : (uint8_t)value;
}

bool rf_rdp_clipboard_image_payload_from_texture(
	GdkTexture *texture,
	struct rf_clipboard_rich_payload *payload
)
{
	int width = 0;
	int height = 0;
	size_t stride = 0;
	size_t length = 0;
	g_autofree uint8_t *bgra = NULL;
	g_autofree uint8_t *rgba = NULL;

	if (texture == NULL || payload == NULL)
		return false;

	width = gdk_texture_get_width(texture);
	height = gdk_texture_get_height(texture);
	if (width <= 0 || height <= 0)
		return false;
	stride = (size_t)width * 4;
	if ((size_t)height > SIZE_MAX / stride)
		return false;
	length = stride * (size_t)height;

	bgra = g_malloc0(length);
	rgba = g_malloc0(length);
	gdk_texture_download(texture, bgra, stride);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const size_t offset = (size_t)y * stride + (size_t)x * 4;
			const uint8_t b = bgra[offset + 0];
			const uint8_t g = bgra[offset + 1];
			const uint8_t r = bgra[offset + 2];
			const uint8_t a = bgra[offset + 3];

			rgba[offset + 0] = unpremultiply_channel(r, a);
			rgba[offset + 1] = unpremultiply_channel(g, a);
			rgba[offset + 2] = unpremultiply_channel(b, a);
			rgba[offset + 3] = a;
		}
	}

	return rf_clipboard_rich_payload_set_image_rgba(
		payload,
		rgba,
		length,
		(uint32_t)width,
		(uint32_t)height,
		stride
	);
}

bool rf_rdp_clipboard_image_payload_from_png(
	const uint8_t *png,
	size_t png_length,
	struct rf_clipboard_rich_payload *payload
)
{
	return rf_rdp_clipboard_image_payload_from_bytes(
		png,
		png_length,
		payload
	);
}

bool rf_rdp_clipboard_image_payload_from_bytes(
	const uint8_t *image,
	size_t image_length,
	struct rf_clipboard_rich_payload *payload
)
{
	g_autoptr(GBytes) bytes = NULL;
	g_autoptr(GError) error = NULL;
	g_autoptr(GdkTexture) texture = NULL;

	if (image == NULL || image_length == 0 || payload == NULL ||
	    image_length > G_MAXSSIZE)
		return false;

	bytes = g_bytes_new(image, image_length);
	texture = gdk_texture_new_from_bytes(bytes, &error);
	if (texture == NULL)
		return false;
	return rf_rdp_clipboard_image_payload_from_texture(texture, payload);
}
