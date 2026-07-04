#include <locale.h>

#include <gdk/gdk.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <glib-unix.h>
#include <gtk/gtk.h>

#include "config.h"
#include "rf-clipboard-rich.h"
#include "rf-clipboard-text.h"
#include "rf-common.h"
#include "rf-rdp-clipboard-display.h"
#include "rf-rdp-clipboard-image.h"
#include "rf-rdp-clipboard-provider.h"
#include "rf-rdp-clipboard-stream.h"
#include "rf-rdp-clipboard-wayland.h"

struct this {
	GMainLoop *main_loop;
	GHashTable *sockets;
	GHashTable *socket_paths;
	GdkDisplay *display;
	GdkClipboard *clipboard;
	RfRdpClipboardWayland *wayland;
	GByteArray *last_wire;
};

struct read_request {
	struct this *this;
	struct rf_clipboard_rich_payload payload;
	unsigned int pending;
};

static void remember_payload(
	struct this *this,
	const struct rf_clipboard_rich_payload *payload
)
{
	g_autoptr(GByteArray) wire = rf_clipboard_rich_payload_serialize(payload);

	if (wire == NULL)
		return;
	g_clear_pointer(&this->last_wire, g_byte_array_unref);
	this->last_wire = g_steal_pointer(&wire);
}

static void send_payload_to_servers(
	struct this *this,
	const struct rf_clipboard_rich_payload *payload
)
{
	g_autoptr(GByteArray) wire = rf_clipboard_rich_payload_serialize(payload);
	unsigned int servers = 0;
	const bool duplicate =
		rf_clipboard_rich_wire_equal(this->last_wire, wire);
	GHashTableIter it;
	void *key;
	void *value;

	if (wire == NULL)
		return;
	if (duplicate) {
		g_message("RDP Clipboard: Skipping duplicate payload.");
		return;
	}

	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		GSocket *socket = key;
		GSource *source = value;
		g_autoptr(GError) error = NULL;
		g_autoptr(GSocketConnection) connection =
			g_socket_connection_factory_create_connection(socket);
		GOutputStream *output =
			g_io_stream_get_output_stream(G_IO_STREAM(connection));
		gsize written = 0;
		ssize_t ret = rf_send_header(
			connection,
			RF_MSG_TYPE_RDP_CLIPBOARD_RICH,
			wire->len,
			&error
		);

		if (ret <= 0 ||
		    !g_output_stream_write_all(
			    output,
			    wire->data,
			    wire->len,
			    &written,
			    NULL,
			    &error
		    ) || written != wire->len) {
			g_warning("RDP Clipboard: Failed to send payload: %s.",
				error != NULL ? error->message : "short write");
			g_source_destroy(source);
			g_hash_table_iter_remove(&it);
			continue;
		}
		servers++;
	}
	if (servers > 0) {
		g_clear_pointer(&this->last_wire, g_byte_array_unref);
		this->last_wire = g_steal_pointer(&wire);
	}
	g_message("RDP Clipboard: Sent payload to %u server(s).", servers);
}

static void read_request_done(struct read_request *request)
{
	if (request->pending > 0)
		return;
	if (rf_clipboard_rich_payload_has_data(&request->payload))
		send_payload_to_servers(request->this, &request->payload);
	rf_clipboard_rich_payload_clear(&request->payload);
	g_free(request);
}

static void on_wayland_payload(
	const struct rf_clipboard_rich_payload *payload,
	void *data
)
{
	struct this *this = data;

	send_payload_to_servers(this, payload);
}

static void on_read_html_stream_finish(
	GObject *source_object,
	GAsyncResult *res,
	void *data
)
{
	struct read_request *request = data;
	g_autoptr(GError) error = NULL;
	g_autoptr(GByteArray) html = rf_rdp_clipboard_read_stream_finish(
		G_INPUT_STREAM(source_object),
		res,
		&error
	);

	if (html == NULL) {
		g_warning("RDP Clipboard: Failed to read HTML stream: %s.", error->message);
		goto out;
	}
	rf_clipboard_rich_payload_set_html(
		&request->payload,
		html->data,
		html->len
	);
	g_message("RDP Clipboard: Read HTML length %u.", html->len);
out:
	request->pending--;
	read_request_done(request);
}

static void on_read_html_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct read_request *request = data;
	GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
	g_autoptr(GError) error = NULL;
	const char *mime_type = NULL;
	g_autoptr(GInputStream) stream =
		gdk_clipboard_read_finish(clipboard, res, &mime_type, &error);

	if (stream == NULL) {
		g_warning("RDP Clipboard: Failed to read HTML: %s.", error->message);
		request->pending--;
		read_request_done(request);
		return;
	}

	rf_rdp_clipboard_read_stream_async(
		stream,
		RF_CLIPBOARD_RICH_MAX_BYTES,
		NULL,
		on_read_html_stream_finish,
		request
	);
}

static void on_read_text_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct read_request *request = data;
	GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
	g_autoptr(GError) error = NULL;
	g_autofree char *text = gdk_clipboard_read_text_finish(
		clipboard,
		res,
		&error
	);
	g_autofree char *normalized = NULL;

	if (text != NULL) {
		normalized = rf_clipboard_text_normalize(text);
		if (g_strcmp0(text, normalized) != 0)
			g_message("RDP Clipboard: Normalized JSON Unicode escape text.");
		rf_clipboard_rich_payload_set_text(&request->payload, normalized);
	} else {
		g_warning("RDP Clipboard: Failed to read text: %s.", error->message);
	}
	request->pending--;
	read_request_done(request);
}

static void on_read_texture_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct read_request *request = data;
	GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
	g_autoptr(GError) error = NULL;
	g_autoptr(GdkTexture) texture = gdk_clipboard_read_texture_finish(
		clipboard,
		res,
		&error
	);

	if (texture != NULL) {
		rf_rdp_clipboard_image_payload_from_texture(
			texture,
			&request->payload
		);
	} else {
		g_warning("RDP Clipboard: Failed to read image: %s.", error->message);
	}
	request->pending--;
	read_request_done(request);
}

static void on_clipboard_changed(GdkClipboard *clipboard, void *data)
{
	struct this *this = data;
	const bool is_local = gdk_clipboard_is_local(clipboard);
	GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
	g_autofree char *formats_string = gdk_content_formats_to_string(formats);
	const bool has_text =
		gdk_content_formats_contain_mime_type(formats, "text/plain") ||
		gdk_content_formats_contain_mime_type(
			formats,
			"text/plain;charset=utf-8"
		) ||
		gdk_content_formats_contain_gtype(formats, G_TYPE_STRING);
	const bool has_html =
		gdk_content_formats_contain_mime_type(formats, "text/html");
	const bool has_image =
		gdk_content_formats_contain_gtype(formats, GDK_TYPE_TEXTURE);
	struct read_request *request = NULL;

	if (is_local) {
		g_message("RDP Clipboard: Ignoring local owner change.");
		return;
	}
	g_message(
		"RDP Clipboard: Changed text=%s html=%s image=%s formats=%s.",
		has_text ? "yes" : "no",
		has_html ? "yes" : "no",
		has_image ? "yes" : "no",
		formats_string != NULL ? formats_string : "(none)"
	);
	if (!has_text && !has_html && !has_image)
		return;

	request = g_new0(struct read_request, 1);
	request->this = this;
	rf_clipboard_rich_payload_init(&request->payload);
	if (has_text) {
		request->pending++;
		gdk_clipboard_read_text_async(
			clipboard,
			NULL,
			on_read_text_finish,
			request
		);
	}
	if (has_html) {
		static const char *html_mime_types[] = { "text/html", NULL };

		request->pending++;
		gdk_clipboard_read_async(
			clipboard,
			html_mime_types,
			G_PRIORITY_DEFAULT,
			NULL,
			on_read_html_finish,
			request
		);
	}
	if (has_image) {
		request->pending++;
		gdk_clipboard_read_texture_async(
			clipboard,
			NULL,
			on_read_texture_finish,
			request
		);
	}
	read_request_done(request);
}

static void set_clipboard_from_payload(
	struct this *this,
	const struct rf_clipboard_rich_payload *payload
)
{
	if (payload->image_rgba != NULL) {
		if (rf_rdp_clipboard_wayland_set_payload(this->wayland, payload)) {
			remember_payload(this, payload);
			g_message("RDP Clipboard: Set Wayland image %ux%u.",
				payload->image_width,
				payload->image_height);
			return;
		}

		g_autoptr(GBytes) bytes = g_bytes_new(
			payload->image_rgba->data,
			payload->image_rgba->len
		);
		g_autoptr(GdkTexture) texture = gdk_memory_texture_new(
			payload->image_width,
			payload->image_height,
			GDK_MEMORY_R8G8B8A8,
			bytes,
			payload->image_stride
		);

		gdk_clipboard_set_texture(this->clipboard, texture);
		g_message("RDP Clipboard: Set local image %ux%u.",
			payload->image_width,
			payload->image_height);
		return;
	}
	if (payload->html != NULL) {
		if (rf_rdp_clipboard_wayland_set_payload(this->wayland, payload)) {
			remember_payload(this, payload);
			g_message(
				"RDP Clipboard: Set Wayland HTML length %u text-length=%zu.",
				payload->html->len,
				payload->text != NULL ? strlen(payload->text) : 0
			);
			return;
		}

		g_autoptr(GdkContentProvider) provider =
			rf_rdp_clipboard_content_provider_new(payload);

		if (provider == NULL) {
			g_warning("RDP Clipboard: Failed to create HTML content provider.");
			return;
		}
		gdk_clipboard_set_content(this->clipboard, provider);
		g_message(
			"RDP Clipboard: Set local HTML length %u text-length=%zu.",
			payload->html->len,
			payload->text != NULL ? strlen(payload->text) : 0
		);
		return;
	}
	if (payload->text != NULL) {
		if (rf_rdp_clipboard_wayland_set_payload(this->wayland, payload)) {
			remember_payload(this, payload);
			g_message("RDP Clipboard: Set Wayland text length %zu.",
				strlen(payload->text));
			return;
		}

		gdk_clipboard_set_text(this->clipboard, payload->text);
		g_message("RDP Clipboard: Set local text length %zu.",
			strlen(payload->text));
	}
}

static ssize_t on_rdp_clipboard_msg(
	struct this *this,
	GSocketConnection *connection
)
{
	size_t length = 0;
	gsize read = 0;
	g_autofree uint8_t *data = NULL;
	g_autoptr(GError) error = NULL;
	GInputStream *input =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));
	g_auto(RfClipboardRichPayload) payload;

	if (!g_input_stream_read_all(
		    input,
		    &length,
		    sizeof(length),
		    &read,
		    NULL,
		    &error
	    ) || read != sizeof(length))
		goto fail;
	if (length == 0 || length > RF_CLIPBOARD_RICH_MAX_BYTES)
		return -1;

	data = g_malloc(length);
	if (!g_input_stream_read_all(input, data, length, &read, NULL, &error) ||
	    read != length)
		goto fail;

	rf_clipboard_rich_payload_init(&payload);
	if (!rf_clipboard_rich_payload_parse(data, length, &payload))
		return -1;
	set_clipboard_from_payload(this, &payload);
	return length;

fail:
	if (error != NULL)
		g_warning("RDP Clipboard: Failed to receive payload: %s.",
			error->message);
	return -1;
}

static int on_socket_in(GSocket *socket, GIOCondition condition, void *data)
{
	struct this *this = data;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	char type = 0;
	g_autoptr(GSocketConnection) connection =
		g_socket_connection_factory_create_connection(socket);
	GInputStream *input =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));

	if (!(condition & (G_IO_IN | G_IO_PRI)))
		return G_SOURCE_CONTINUE;
	ret = g_input_stream_read(input, &type, sizeof(type), NULL, &error);
	if (ret <= 0)
		goto out;

	switch (type) {
	case RF_MSG_TYPE_RDP_CLIPBOARD_RICH:
		ret = on_rdp_clipboard_msg(this, connection);
		break;
	default:
		g_message("RDP Clipboard: Ignoring unsupported message %c.", type);
		break;
	}

out:
	if (ret <= 0) {
		const char *socket_path =
			g_object_get_data(G_OBJECT(socket), "reframe-rdp-clipboard-path");

		if (error != NULL)
			g_warning("RDP Clipboard: Server disconnected: %s.",
				error->message);
		if (socket_path != NULL)
			g_hash_table_remove(this->socket_paths, socket_path);
		g_hash_table_remove(this->sockets, socket);
		if (g_hash_table_size(this->sockets) == 0)
			g_clear_pointer(&this->last_wire, g_byte_array_unref);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void connect_socket(
	struct this *this,
	const char *socket_path,
	bool socket_path_reserved
)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketAddress) address =
		g_unix_socket_address_new(socket_path);
	g_autoptr(GSocketClient) client = g_socket_client_new();

	if (!socket_path_reserved) {
		if (g_hash_table_contains(this->socket_paths, socket_path))
			return;
		g_hash_table_add(this->socket_paths, g_strdup(socket_path));
	}

	g_autoptr(GSocketConnection) connection = g_socket_client_connect(
		client,
		G_SOCKET_CONNECTABLE(address),
		NULL,
		&error
	);
	if (connection == NULL) {
		g_warning("RDP Clipboard: Failed to connect to server: %s.",
			error->message);
		g_hash_table_remove(this->socket_paths, socket_path);
		return;
	}

	GSocket *socket = g_socket_connection_get_socket(connection);
	GSource *source = g_socket_create_source(socket, G_IO_IN | G_IO_PRI, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(on_socket_in), this, NULL);
	g_source_attach(source, NULL);
	g_object_set_data_full(
		G_OBJECT(socket),
		"reframe-rdp-clipboard-path",
		g_strdup(socket_path),
		g_free
	);
	g_hash_table_insert(this->sockets, g_object_ref(socket), source);
	g_clear_pointer(&this->last_wire, g_byte_array_unref);
	g_message("RDP Clipboard: Connected to %s.", socket_path);
}

struct connect_request {
	struct this *this;
	char *socket_path;
};

static gboolean connect_later(void *data)
{
	struct connect_request *request = data;

	connect_socket(request->this, request->socket_path, true);
	g_free(request->socket_path);
	g_free(request);
	return G_SOURCE_REMOVE;
}

static void connect_deferred(struct this *this, const char *socket_path)
{
	struct connect_request *request = NULL;

	if (g_hash_table_contains(this->socket_paths, socket_path))
		return;
	g_hash_table_add(this->socket_paths, g_strdup(socket_path));

	request = g_new0(struct connect_request, 1);
	request->this = this;
	request->socket_path = g_strdup(socket_path);
	g_timeout_add(100, connect_later, request);
}

static void on_changed(
	GFileMonitor *monitor,
	GFile *file,
	GFile *other_file,
	GFileMonitorEvent event_type,
	void *data
)
{
	struct this *this = data;

	(void)monitor;
	(void)other_file;
	if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) !=
	    G_FILE_TYPE_SPECIAL)
		return;

	g_autofree char *socket_path = g_file_get_path(file);
	if (event_type == G_FILE_MONITOR_EVENT_CREATED)
		connect_deferred(this, socket_path);
}

static int on_sigint(void *data)
{
	struct this *this = data;

	g_main_loop_quit(this->main_loop);
	return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");

	g_autofree char *socket_dir = NULL;
	int version = FALSE;
	g_autoptr(GError) error = NULL;
	GOptionEntry options[] = {
		{ "version",
		  'v',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  &version,
		  "Display version and exit.",
		  NULL },
		{ "socket-dir",
		  'd',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_FILENAME,
		  &socket_dir,
		  "RDP clipboard socket dir to communicate.",
		  "DIR" },
		{ NULL, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, NULL, NULL }
	};
	g_autoptr(GOptionContext)
		context = g_option_context_new(" - ReFrame RDP Clipboard");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_warning("RDP Clipboard: Failed to parse options: %s.", error->message);
		g_clear_pointer(&error, g_error_free);
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	if (socket_dir == NULL)
		socket_dir = g_strdup("/tmp/reframe-rdp-clipboard");
	const size_t length = strlen(socket_dir);
	if (length > 0 && socket_dir[length - 1] == '/')
		socket_dir[length - 1] = '\0';

	g_autofree struct this *this = g_malloc0(sizeof(*this));
	this->socket_paths = g_hash_table_new_full(
		g_str_hash,
		g_str_equal,
		g_free,
		NULL
	);
	this->sockets = g_hash_table_new_full(
		g_direct_hash,
		g_direct_equal,
		g_object_unref,
		(GDestroyNotify)g_source_unref
	);

	this->wayland = rf_rdp_clipboard_wayland_new(on_wayland_payload, this);
	rf_rdp_clipboard_configure_gdk_backend();
	gtk_init();

	this->display = gdk_display_get_default();
	if (this->display == NULL)
		g_error("RDP Clipboard: Failed to get the default GDK display.");
	this->clipboard = gdk_display_get_clipboard(this->display);
	if (this->clipboard == NULL)
		g_error("RDP Clipboard: Failed to get clipboard.");
	g_signal_connect(
		this->clipboard,
		"changed",
		G_CALLBACK(on_clipboard_changed),
		this
	);

	g_autoptr(GDir) dir = g_dir_open(socket_dir, 0, NULL);
	if (dir != NULL) {
		const char *name = NULL;
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree char *socket_path =
				g_build_filename(socket_dir, name, NULL);
			connect_socket(this, socket_path, false);
		}
	}

	g_autoptr(GFile) file = g_file_new_for_path(socket_dir);
	g_autoptr(GFileMonitor) monitor = g_file_monitor_directory(
		file,
		G_FILE_MONITOR_NONE,
		NULL,
		&error
	);
	if (monitor == NULL)
		g_warning("RDP Clipboard: Failed to monitor socket dir: %s.",
			error->message);
	else
		g_signal_connect(monitor, "changed", G_CALLBACK(on_changed), this);

	this->main_loop = g_main_loop_new(NULL, false);
	g_unix_signal_add(SIGINT, on_sigint, this);
	g_main_loop_run(this->main_loop);
	g_main_loop_unref(this->main_loop);

	GHashTableIter it;
	void *value;
	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, NULL, &value))
		g_source_destroy(value);
	g_hash_table_unref(this->sockets);
	g_hash_table_unref(this->socket_paths);
	g_clear_pointer(&this->wayland, rf_rdp_clipboard_wayland_free);
	g_clear_pointer(&this->last_wire, g_byte_array_unref);
	g_clear_object(&this->clipboard);
	if (this->display != NULL)
		gdk_display_close(this->display);
	g_clear_object(&this->display);
	return 0;
}
