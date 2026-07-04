#include "rf-rdp-clipboard-wayland.h"

#include "config.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <glib-unix.h>
#include <glib-unix.h>

#include "rf-clipboard-text.h"
#include "rf-rdp-clipboard-image.h"
#include "rf-rdp-clipboard-stream.h"

#define RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT "<html><body>"
#define RF_RDP_CLIPBOARD_MOZ_HTMLINFO "0,0"

static bool string_is_one_of(const char *value, const char * const *candidates)
{
	if (value == NULL)
		return false;
	for (size_t i = 0; candidates[i] != NULL; ++i) {
		if (g_strcmp0(value, candidates[i]) == 0)
			return true;
	}
	return false;
}

const char *rf_rdp_clipboard_wayland_pick_html_mime(
	const char * const *mime_types
)
{
	static const char *preferred[] = {
		"text/html",
		"text/html;charset=utf-8",
		NULL
	};

	if (mime_types == NULL)
		return NULL;
	for (size_t i = 0; preferred[i] != NULL; ++i) {
		for (size_t j = 0; mime_types[j] != NULL; ++j) {
			if (g_strcmp0(mime_types[j], preferred[i]) == 0)
				return mime_types[j];
		}
	}
	return NULL;
}

const char *rf_rdp_clipboard_wayland_pick_text_mime(
	const char * const *mime_types
)
{
	static const char *preferred[] = {
		"text/plain;charset=utf-8",
		"text/plain",
		"UTF8_STRING",
		"TEXT",
		"STRING",
		NULL
	};

	if (mime_types == NULL)
		return NULL;
	for (size_t i = 0; preferred[i] != NULL; ++i) {
		for (size_t j = 0; mime_types[j] != NULL; ++j) {
			if (g_strcmp0(mime_types[j], preferred[i]) == 0)
				return mime_types[j];
		}
	}
	return NULL;
}

const char *rf_rdp_clipboard_wayland_pick_image_mime(
	const char * const *mime_types
)
{
	static const char *preferred[] = {
		"image/png",
		"image/webp",
		"image/jpeg",
		"image/tiff",
		"image/bmp",
		"image/x-bmp",
		"image/x-MS-bmp",
		NULL
	};

	if (mime_types == NULL)
		return NULL;
	for (size_t i = 0; preferred[i] != NULL; ++i) {
		for (size_t j = 0; mime_types[j] != NULL; ++j) {
			if (g_strcmp0(mime_types[j], preferred[i]) == 0)
				return mime_types[j];
		}
	}
	return NULL;
}

GBytes *rf_rdp_clipboard_wayland_payload_bytes_for_mime(
	const struct rf_clipboard_rich_payload *payload,
	const char *mime_type
)
{
	static const char *html_mimes[] = {
		"text/html",
		"text/html;charset=utf-8",
		NULL
	};
	static const char *text_mimes[] = {
		"text/plain",
		"text/plain;charset=utf-8",
		"UTF8_STRING",
		"TEXT",
		"STRING",
		NULL
	};
	static const char *bmp_mimes[] = {
		"image/bmp",
		"image/x-bmp",
		"image/x-MS-bmp",
		NULL
	};

	if (payload == NULL || mime_type == NULL)
		return NULL;
	if (string_is_one_of(mime_type, html_mimes) && payload->html != NULL)
		return g_bytes_new(payload->html->data, payload->html->len);
	if (string_is_one_of(mime_type, html_mimes) &&
	    payload->image_rgba != NULL)
		return rf_rdp_clipboard_image_html_bytes_from_payload(payload);
	if (g_strcmp0(mime_type, "text/_moz_htmlcontext") == 0 &&
	    (payload->html != NULL || payload->image_rgba != NULL))
		return g_bytes_new_static(
			RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT,
			strlen(RF_RDP_CLIPBOARD_MOZ_HTMLCONTEXT)
		);
	if (g_strcmp0(mime_type, "text/_moz_htmlinfo") == 0 &&
	    (payload->html != NULL || payload->image_rgba != NULL))
		return g_bytes_new_static(
			RF_RDP_CLIPBOARD_MOZ_HTMLINFO,
			strlen(RF_RDP_CLIPBOARD_MOZ_HTMLINFO)
		);
	if (string_is_one_of(mime_type, text_mimes) && payload->text != NULL)
		return g_bytes_new(payload->text, strlen(payload->text));
	if (g_strcmp0(mime_type, "image/png") == 0 &&
	    payload->image_rgba != NULL)
		return rf_rdp_clipboard_image_png_bytes_from_payload(payload);
	if (g_strcmp0(mime_type, "image/tiff") == 0 &&
	    payload->image_rgba != NULL)
		return rf_rdp_clipboard_image_tiff_bytes_from_payload(payload);
	if ((g_strcmp0(mime_type, "image/jpeg") == 0 ||
	     g_strcmp0(mime_type, "image/jpg") == 0) &&
	    payload->image_rgba != NULL)
		return rf_rdp_clipboard_image_jpeg_bytes_from_payload(payload);
	if (g_strcmp0(mime_type, "image/webp") == 0 &&
	    payload->image_rgba != NULL)
		return rf_rdp_clipboard_image_webp_bytes_from_payload(payload);
	if (string_is_one_of(mime_type, bmp_mimes) && payload->image_rgba != NULL)
		return rf_rdp_clipboard_image_bmp_bytes_from_payload(payload);
	return NULL;
}

#ifdef HAVE_WAYLAND_CLIPBOARD

#include <wayland-client.h>

#include "ext-data-control-v1-client-protocol.h"

struct wayland_offer {
	struct ext_data_control_offer_v1 *offer;
	GPtrArray *mime_types;
};

struct wayland_source {
	RfRdpClipboardWayland *clipboard;
	struct ext_data_control_source_v1 *source;
	struct rf_clipboard_rich_payload payload;
};

struct rf_rdp_clipboard_wayland {
	struct wl_display *display;
	struct wl_registry *registry;
	struct wl_seat *seat;
	struct ext_data_control_manager_v1 *manager;
	struct ext_data_control_device_v1 *device;
	GSource *source;
	struct wayland_offer *pending_offer;
	struct wayland_source *current_source;
	bool ignore_next_selection;
	RfRdpClipboardWaylandPayloadFunc on_payload;
	void *data;
};

struct wayland_read_request {
	RfRdpClipboardWayland *clipboard;
	struct wayland_offer *offer;
	struct rf_clipboard_rich_payload payload;
	unsigned int pending;
};

struct wayland_read_slot {
	struct wayland_read_request *request;
	bool html;
	bool image;
};

static void wayland_offer_free(struct wayland_offer *offer)
{
	if (offer == NULL)
		return;
	if (offer->offer != NULL)
		ext_data_control_offer_v1_destroy(offer->offer);
	g_clear_pointer(&offer->mime_types, g_ptr_array_unref);
	g_free(offer);
}

static void wayland_source_free(struct wayland_source *source)
{
	if (source == NULL)
		return;
	if (source->source != NULL)
		ext_data_control_source_v1_destroy(source->source);
	rf_clipboard_rich_payload_clear(&source->payload);
	g_free(source);
}

static char **wayland_offer_mime_types_strv(const struct wayland_offer *offer)
{
	char **types = NULL;

	if (offer == NULL || offer->mime_types == NULL)
		return NULL;
	types = g_new0(char *, offer->mime_types->len + 1);
	for (size_t i = 0; i < offer->mime_types->len; ++i)
		types[i] = g_ptr_array_index(offer->mime_types, i);
	return types;
}

static bool write_all_fd(int fd, const uint8_t *data, size_t length)
{
	size_t written = 0;

	while (written < length) {
		const ssize_t ret = write(fd, data + written, length - written);

		if (ret < 0 && errno == EINTR)
			continue;
		if (ret <= 0)
			return false;
		written += ret;
	}
	return true;
}

static void on_data_source_send(
	void *data,
	struct ext_data_control_source_v1 *source,
	const char *mime_type,
	int32_t fd
)
{
	struct wayland_source *wayland_source = data;
	g_autoptr(GBytes) bytes = NULL;
	gsize length = 0;
	const uint8_t *payload_data = NULL;

	(void)source;
	bytes = rf_rdp_clipboard_wayland_payload_bytes_for_mime(
		&wayland_source->payload,
		mime_type
	);
	if (bytes != NULL) {
		payload_data = g_bytes_get_data(bytes, &length);
		if (!write_all_fd(fd, payload_data, length))
			g_warning("RDP Clipboard: Failed to write Wayland MIME %s.",
				mime_type);
		else
			g_message(
				"RDP Clipboard: Served Wayland MIME %s length %zu.",
				mime_type,
				length
			);
	} else {
		g_message("RDP Clipboard: No payload for Wayland MIME %s.",
			mime_type);
	}
	close(fd);
}

static void on_data_source_cancelled(
	void *data,
	struct ext_data_control_source_v1 *source
)
{
	struct wayland_source *wayland_source = data;
	RfRdpClipboardWayland *clipboard = wayland_source->clipboard;

	(void)source;
	if (clipboard->current_source == wayland_source)
		clipboard->current_source = NULL;
	wayland_source_free(wayland_source);
}

static const struct ext_data_control_source_v1_listener data_source_listener = {
	.send = on_data_source_send,
	.cancelled = on_data_source_cancelled
};

static void on_offer_mime(
	void *data,
	struct ext_data_control_offer_v1 *offer,
	const char *mime_type
)
{
	struct wayland_offer *wayland_offer = data;

	(void)offer;
	g_ptr_array_add(wayland_offer->mime_types, g_strdup(mime_type));
}

static const struct ext_data_control_offer_v1_listener offer_listener = {
	.offer = on_offer_mime
};

static void wayland_read_request_done(struct wayland_read_request *request)
{
	if (request->pending > 0)
		return;
	if (rf_clipboard_rich_payload_has_data(&request->payload) &&
	    request->clipboard->on_payload != NULL)
		request->clipboard->on_payload(
			&request->payload,
			request->clipboard->data
		);
	rf_clipboard_rich_payload_clear(&request->payload);
	wayland_offer_free(request->offer);
	g_free(request);
}

static void on_offer_read_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct wayland_read_slot *slot = data;
	struct wayland_read_request *request = slot->request;
	g_autoptr(GError) error = NULL;
	g_autoptr(GByteArray) bytes = rf_rdp_clipboard_read_stream_finish(
		G_INPUT_STREAM(source_object),
		res,
		&error
	);

	if (bytes == NULL) {
		g_warning("RDP Clipboard: Failed to read Wayland clipboard: %s.",
			error != NULL ? error->message : "unknown error");
		goto out;
	}
	if (slot->image) {
		if (rf_rdp_clipboard_image_payload_from_bytes(
			    bytes->data,
			    bytes->len,
			    &request->payload
		    ))
			g_message("RDP Clipboard: Read Wayland image length %u.",
				bytes->len);
		else
			g_warning("RDP Clipboard: Failed to decode Wayland image.");
	} else if (slot->html) {
		rf_clipboard_rich_payload_set_html(
			&request->payload,
			bytes->data,
			bytes->len
		);
		g_message("RDP Clipboard: Read Wayland HTML length %u.", bytes->len);
	} else if (g_utf8_validate((const char *)bytes->data, bytes->len, NULL)) {
		g_autofree char *text =
			g_strndup((const char *)bytes->data, bytes->len);
		g_autofree char *normalized = rf_clipboard_text_normalize(text);

		rf_clipboard_rich_payload_set_text(&request->payload, normalized);
		g_message("RDP Clipboard: Read Wayland text length %u.", bytes->len);
	}
out:
	request->pending--;
	g_free(slot);
	wayland_read_request_done(request);
}

static bool start_offer_read(
	struct wayland_read_request *request,
	const char *mime_type,
	bool html,
	bool image
)
{
	int fds[2] = { -1, -1 };
	struct wayland_read_slot *slot = NULL;
	GInputStream *stream = NULL;

	if (mime_type == NULL)
		return false;
	if (pipe(fds) < 0)
		return false;
	ext_data_control_offer_v1_receive(request->offer->offer, mime_type, fds[1]);
	close(fds[1]);
	fds[1] = -1;

	slot = g_new0(struct wayland_read_slot, 1);
	slot->request = request;
	slot->html = html;
	slot->image = image;
	request->pending++;

	stream = g_unix_input_stream_new(fds[0], true);
	rf_rdp_clipboard_read_stream_async(
		stream,
		RF_CLIPBOARD_RICH_MAX_BYTES,
		NULL,
		on_offer_read_finish,
		slot
	);
	g_object_unref(stream);
	return true;
}

static void start_offer_reads(
	RfRdpClipboardWayland *clipboard,
	struct wayland_offer *offer
)
{
	g_autofree char **types = wayland_offer_mime_types_strv(offer);
	const char *html_mime =
		rf_rdp_clipboard_wayland_pick_html_mime((const char * const *)types);
	const char *text_mime =
		rf_rdp_clipboard_wayland_pick_text_mime((const char * const *)types);
	const char *image_mime =
		rf_rdp_clipboard_wayland_pick_image_mime((const char * const *)types);
	struct wayland_read_request *request = NULL;

	if (html_mime == NULL && text_mime == NULL && image_mime == NULL) {
		wayland_offer_free(offer);
		return;
	}

	request = g_new0(struct wayland_read_request, 1);
	request->clipboard = clipboard;
	request->offer = offer;
	rf_clipboard_rich_payload_init(&request->payload);
	start_offer_read(request, html_mime, true, false);
	start_offer_read(request, text_mime, false, false);
	start_offer_read(request, image_mime, false, true);
	wl_display_flush(clipboard->display);
	wayland_read_request_done(request);
}

static void on_data_device_data_offer(
	void *data,
	struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer
)
{
	RfRdpClipboardWayland *clipboard = data;

	(void)device;
	g_clear_pointer(&clipboard->pending_offer, wayland_offer_free);
	clipboard->pending_offer = g_new0(struct wayland_offer, 1);
	clipboard->pending_offer->offer = offer;
	clipboard->pending_offer->mime_types =
		g_ptr_array_new_with_free_func(g_free);
	ext_data_control_offer_v1_add_listener(
		offer,
		&offer_listener,
		clipboard->pending_offer
	);
}

static void on_data_device_selection(
	void *data,
	struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer
)
{
	RfRdpClipboardWayland *clipboard = data;
	struct wayland_offer *wayland_offer =
		g_steal_pointer(&clipboard->pending_offer);

	(void)device;
	if (wayland_offer == NULL || wayland_offer->offer != offer) {
		g_clear_pointer(&wayland_offer, wayland_offer_free);
		return;
	}
	if (clipboard->ignore_next_selection) {
		clipboard->ignore_next_selection = false;
		wayland_offer_free(wayland_offer);
		return;
	}
	start_offer_reads(clipboard, wayland_offer);
}

static void on_data_device_finished(
	void *data,
	struct ext_data_control_device_v1 *device
)
{
	RfRdpClipboardWayland *clipboard = data;

	(void)device;
	g_warning("RDP Clipboard: Wayland data-control device finished.");
	if (clipboard->source != NULL)
		g_source_destroy(clipboard->source);
}

static void on_data_device_primary_selection(
	void *data,
	struct ext_data_control_device_v1 *device,
	struct ext_data_control_offer_v1 *offer
)
{
	RfRdpClipboardWayland *clipboard = data;

	(void)device;
	if (clipboard->pending_offer != NULL &&
	    clipboard->pending_offer->offer == offer) {
		wayland_offer_free(g_steal_pointer(&clipboard->pending_offer));
	} else if (offer != NULL) {
		ext_data_control_offer_v1_destroy(offer);
	}
}

static const struct ext_data_control_device_v1_listener data_device_listener = {
	.data_offer = on_data_device_data_offer,
	.selection = on_data_device_selection,
	.finished = on_data_device_finished,
	.primary_selection = on_data_device_primary_selection
};

static void on_registry_global(
	void *data,
	struct wl_registry *registry,
	uint32_t name,
	const char *interface,
	uint32_t version
)
{
	RfRdpClipboardWayland *clipboard = data;

	if (g_strcmp0(interface, ext_data_control_manager_v1_interface.name) == 0) {
		clipboard->manager = wl_registry_bind(
			registry,
			name,
			&ext_data_control_manager_v1_interface,
			MIN(version, 1)
		);
	} else if (g_strcmp0(interface, wl_seat_interface.name) == 0) {
		clipboard->seat = wl_registry_bind(
			registry,
			name,
			&wl_seat_interface,
			MIN(version, 1)
		);
	}
}

static void on_registry_global_remove(
	void *data,
	struct wl_registry *registry,
	uint32_t name
)
{
	(void)data;
	(void)registry;
	(void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = on_registry_global,
	.global_remove = on_registry_global_remove
};

static gboolean on_wayland_fd(int fd, GIOCondition condition, void *data)
{
	RfRdpClipboardWayland *clipboard = data;

	(void)fd;
	if (condition & (G_IO_ERR | G_IO_HUP | G_IO_NVAL))
		return G_SOURCE_REMOVE;
	if (wl_display_dispatch(clipboard->display) < 0)
		return G_SOURCE_REMOVE;
	while (wl_display_dispatch_pending(clipboard->display) > 0)
		;
	wl_display_flush(clipboard->display);
	return G_SOURCE_CONTINUE;
}

RfRdpClipboardWayland *rf_rdp_clipboard_wayland_new(
	RfRdpClipboardWaylandPayloadFunc on_payload,
	void *data
)
{
	RfRdpClipboardWayland *clipboard = NULL;
	int fd = -1;

	if (g_getenv("WAYLAND_DISPLAY") == NULL)
		return NULL;

	clipboard = g_new0(RfRdpClipboardWayland, 1);
	clipboard->on_payload = on_payload;
	clipboard->data = data;
	clipboard->display = wl_display_connect(NULL);
	if (clipboard->display == NULL) {
		g_message("RDP Clipboard: Wayland display is not available.");
		goto fail;
	}
	clipboard->registry = wl_display_get_registry(clipboard->display);
	wl_registry_add_listener(
		clipboard->registry,
		&registry_listener,
		clipboard
	);
	if (wl_display_roundtrip(clipboard->display) < 0) {
		g_message("RDP Clipboard: Wayland registry roundtrip failed.");
		goto fail;
	}
	if (clipboard->manager == NULL || clipboard->seat == NULL) {
		g_message(
			"RDP Clipboard: Wayland data-control unavailable manager=%s seat=%s.",
			clipboard->manager != NULL ? "yes" : "no",
			clipboard->seat != NULL ? "yes" : "no"
		);
		goto fail;
	}

	clipboard->device = ext_data_control_manager_v1_get_data_device(
		clipboard->manager,
		clipboard->seat
	);
	ext_data_control_device_v1_add_listener(
		clipboard->device,
		&data_device_listener,
		clipboard
	);
	fd = wl_display_get_fd(clipboard->display);
	clipboard->source = g_unix_fd_source_new(
		fd,
		G_IO_IN | G_IO_ERR | G_IO_HUP | G_IO_NVAL
	);
	g_source_set_callback(
		clipboard->source,
		G_SOURCE_FUNC(on_wayland_fd),
		clipboard,
		NULL
	);
	g_source_attach(clipboard->source, NULL);
	if (wl_display_roundtrip(clipboard->display) < 0) {
		g_message("RDP Clipboard: Wayland data-control roundtrip failed.");
		goto fail;
	}
	g_message("RDP Clipboard: Wayland data-control backend is active.");
	return clipboard;

fail:
	rf_rdp_clipboard_wayland_free(clipboard);
	return NULL;
}

bool rf_rdp_clipboard_wayland_set_payload(
	RfRdpClipboardWayland *clipboard,
	const struct rf_clipboard_rich_payload *payload
)
{
	struct wayland_source *source = NULL;

	if (clipboard == NULL || clipboard->manager == NULL ||
	    clipboard->device == NULL || payload == NULL)
		return false;
	wayland_source_free(g_steal_pointer(&clipboard->current_source));

	source = g_new0(struct wayland_source, 1);
	source->clipboard = clipboard;
	rf_clipboard_rich_payload_init(&source->payload);
	if (payload->text != NULL)
		rf_clipboard_rich_payload_set_text(&source->payload, payload->text);
	if (payload->html != NULL)
		rf_clipboard_rich_payload_set_html(
			&source->payload,
			payload->html->data,
			payload->html->len
		);
	if (payload->image_rgba != NULL)
		rf_clipboard_rich_payload_set_image_rgba(
			&source->payload,
			payload->image_rgba->data,
			payload->image_rgba->len,
			payload->image_width,
			payload->image_height,
			payload->image_stride
		);

	source->source = ext_data_control_manager_v1_create_data_source(
		clipboard->manager
	);
	ext_data_control_source_v1_add_listener(
		source->source,
		&data_source_listener,
		source
	);
	if (source->payload.html != NULL || source->payload.image_rgba != NULL) {
		ext_data_control_source_v1_offer(source->source, "text/html");
		ext_data_control_source_v1_offer(
			source->source,
			"text/html;charset=utf-8"
		);
		ext_data_control_source_v1_offer(
			source->source,
			"text/_moz_htmlcontext"
		);
		ext_data_control_source_v1_offer(source->source, "text/_moz_htmlinfo");
	}
	if (source->payload.text != NULL) {
		ext_data_control_source_v1_offer(source->source, "text/plain");
		ext_data_control_source_v1_offer(
			source->source,
			"text/plain;charset=utf-8"
		);
		ext_data_control_source_v1_offer(source->source, "UTF8_STRING");
		ext_data_control_source_v1_offer(source->source, "TEXT");
		ext_data_control_source_v1_offer(source->source, "STRING");
	}
	if (source->payload.image_rgba != NULL) {
		ext_data_control_source_v1_offer(source->source, "image/png");
		ext_data_control_source_v1_offer(source->source, "image/tiff");
		ext_data_control_source_v1_offer(source->source, "image/jpeg");
		ext_data_control_source_v1_offer(source->source, "image/webp");
		ext_data_control_source_v1_offer(source->source, "image/bmp");
		ext_data_control_source_v1_offer(source->source, "image/x-bmp");
		ext_data_control_source_v1_offer(source->source, "image/x-MS-bmp");
	}
	clipboard->current_source = source;
	clipboard->ignore_next_selection = true;
	ext_data_control_device_v1_set_selection(clipboard->device, source->source);
	wl_display_flush(clipboard->display);
	return true;
}

void rf_rdp_clipboard_wayland_free(RfRdpClipboardWayland *clipboard)
{
	if (clipboard == NULL)
		return;
	if (clipboard->source != NULL) {
		g_source_destroy(clipboard->source);
		g_source_unref(clipboard->source);
	}
	wayland_source_free(clipboard->current_source);
	wayland_offer_free(clipboard->pending_offer);
	if (clipboard->device != NULL)
		ext_data_control_device_v1_destroy(clipboard->device);
	if (clipboard->manager != NULL)
		ext_data_control_manager_v1_destroy(clipboard->manager);
	if (clipboard->seat != NULL)
		wl_seat_destroy(clipboard->seat);
	if (clipboard->registry != NULL)
		wl_registry_destroy(clipboard->registry);
	if (clipboard->display != NULL)
		wl_display_disconnect(clipboard->display);
	g_free(clipboard);
}

#else

RfRdpClipboardWayland *rf_rdp_clipboard_wayland_new(
	RfRdpClipboardWaylandPayloadFunc on_payload,
	void *data
)
{
	(void)on_payload;
	(void)data;
	return NULL;
}

void rf_rdp_clipboard_wayland_free(RfRdpClipboardWayland *clipboard)
{
	(void)clipboard;
}

bool rf_rdp_clipboard_wayland_set_payload(
	RfRdpClipboardWayland *clipboard,
	const struct rf_clipboard_rich_payload *payload
)
{
	(void)clipboard;
	(void)payload;
	return false;
}

#endif
