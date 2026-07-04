#include "rf-rdp-clipboard-provider.h"

#include <string.h>

#include "rf-rdp-clipboard-image.h"

#define RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT "<html><body>"
#define RF_RDP_CLIPBOARD_MOZ_HTMLINFO "0,0"

static GdkContentProvider *bytes_provider_new(
	const char *mime_type,
	const uint8_t *data,
	size_t length
)
{
	g_autoptr(GBytes) bytes = NULL;

	if (data == NULL && length > 0)
		return NULL;
	bytes = g_bytes_new(data, length);
	return gdk_content_provider_new_for_bytes(mime_type, bytes);
}

static void add_provider(
	GPtrArray *providers,
	GdkContentProvider *provider
)
{
	if (provider != NULL)
		g_ptr_array_add(providers, provider);
}

GdkContentProvider *rf_rdp_clipboard_content_provider_new(
	const struct rf_clipboard_rich_payload *payload
)
{
	g_autoptr(GPtrArray) providers = NULL;
	GdkContentProvider *provider = NULL;

	if (payload == NULL)
		return NULL;

	providers = g_ptr_array_new();
	if (payload->html != NULL) {
		add_provider(
			providers,
			bytes_provider_new(
				"text/html",
				payload->html->data,
				payload->html->len
			)
		);
		add_provider(
			providers,
			bytes_provider_new(
				"text/html;charset=utf-8",
				payload->html->data,
				payload->html->len
			)
		);
		add_provider(
			providers,
			bytes_provider_new(
				"text/_moz_htmlcontext",
				(const uint8_t *)RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT,
				strlen(RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT)
			)
		);
		add_provider(
			providers,
			bytes_provider_new(
				"text/_moz_htmlinfo",
				(const uint8_t *)RF_RDP_CLIPBOARD_MOZ_HTMLINFO,
				strlen(RF_RDP_CLIPBOARD_MOZ_HTMLINFO)
			)
		);
	}
	if (payload->text != NULL) {
		add_provider(
			providers,
			bytes_provider_new(
				"text/plain",
				(const uint8_t *)payload->text,
				strlen(payload->text)
			)
		);
		add_provider(
			providers,
			bytes_provider_new(
				"text/plain;charset=utf-8",
				(const uint8_t *)payload->text,
				strlen(payload->text)
			)
		);
		add_provider(
			providers,
			gdk_content_provider_new_typed(G_TYPE_STRING, payload->text)
		);
	}
		if (payload->image_rgba != NULL) {
			g_autoptr(GBytes) html =
				rf_rdp_clipboard_image_html_bytes_from_payload(payload);
			g_autoptr(GBytes) png =
				rf_rdp_clipboard_image_png_bytes_from_payload(payload);
			g_autoptr(GBytes) tiff =
				rf_rdp_clipboard_image_tiff_bytes_from_payload(payload);
		g_autoptr(GBytes) jpeg =
			rf_rdp_clipboard_image_jpeg_bytes_from_payload(payload);
		g_autoptr(GBytes) webp =
			rf_rdp_clipboard_image_webp_bytes_from_payload(payload);
			g_autoptr(GBytes) bmp =
				rf_rdp_clipboard_image_bmp_bytes_from_payload(payload);
	
			if (payload->html == NULL && html != NULL) {
				add_provider(
					providers,
					gdk_content_provider_new_for_bytes("text/html", html)
				);
				add_provider(
					providers,
					gdk_content_provider_new_for_bytes(
						"text/html;charset=utf-8",
						html
					)
				);
				add_provider(
					providers,
					bytes_provider_new(
						"text/_moz_htmlcontext",
						(const uint8_t *)RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT,
						strlen(RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT)
					)
				);
				add_provider(
					providers,
					bytes_provider_new(
						"text/_moz_htmlinfo",
						(const uint8_t *)RF_RDP_CLIPBOARD_MOZ_HTMLINFO,
						strlen(RF_RDP_CLIPBOARD_MOZ_HTMLINFO)
					)
				);
			}
			if (png != NULL)
				add_provider(
					providers,
				gdk_content_provider_new_for_bytes("image/png", png)
			);
		if (tiff != NULL)
			add_provider(
				providers,
				gdk_content_provider_new_for_bytes("image/tiff", tiff)
			);
		if (jpeg != NULL)
			add_provider(
				providers,
				gdk_content_provider_new_for_bytes("image/jpeg", jpeg)
			);
		if (webp != NULL)
			add_provider(
				providers,
				gdk_content_provider_new_for_bytes("image/webp", webp)
			);
		if (bmp != NULL) {
			add_provider(
				providers,
				gdk_content_provider_new_for_bytes("image/bmp", bmp)
			);
			add_provider(
				providers,
				gdk_content_provider_new_for_bytes("image/x-bmp", bmp)
			);
			add_provider(
				providers,
				gdk_content_provider_new_for_bytes("image/x-MS-bmp", bmp)
			);
		}
	}
	if (providers->len == 0)
		return NULL;

	provider = gdk_content_provider_new_union(
		(GdkContentProvider **)providers->pdata,
		providers->len
	);
	return provider;
}
