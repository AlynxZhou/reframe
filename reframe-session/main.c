#include <locale.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixsocketaddress.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include "config.h"
#include "rf-common.h"
#include "rf-clipboard-text.h"

struct this {
	GMainLoop *main_loop;
	GHashTable *sockets;
	GHashTable *socket_paths;
	GIOCondition io_flags;
	GdkDisplay *display;
	GdkClipboard *clipboard;
};

static void
send_clipboard_text_msg(struct this *this, const char *clipboard_text)
{
	size_t length = strlen(clipboard_text) + 1;
	unsigned int servers = 0;
	GHashTableIter it;
	void *key;
	void *value;
	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		ssize_t ret = 0;
		gsize written = 0;
		g_autoptr(GError) error = NULL;
		GSocketConnection *connection =
			g_socket_connection_factory_create_connection(key);
		GOutputStream *os =
			g_io_stream_get_output_stream(G_IO_STREAM(connection));
		ret = rf_send_header(
			connection, RF_MSG_TYPE_CLIPBOARD_TEXT, length, &error
		);
		if (ret <= 0)
			goto next;
		if (!g_output_stream_write_all(
			    os,
			    clipboard_text,
			    length,
			    &written,
			    NULL,
			    &error
		    ) || written != length) {
			ret = error != NULL ? -1 : 0;
			goto next;
		}
		ret = written;
	next:
		if (ret <= 0) {
			if (ret < 0)
				g_warning(
					"Failed to send clipboard text: %s.",
					error->message
				);
			else
				g_message("ReFrame Server disconnected.");
			g_source_destroy(value);
			g_hash_table_iter_remove(&it);
		} else {
			servers++;
		}
	}
	g_message(
		"Clipboard: Sent local clipboard text length %zu to %u server(s).",
		length,
		servers
	);
}

static void
on_read_text_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct this *this = data;
	GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
	g_autoptr(GError) error = NULL;
	g_autofree char *text = NULL;
	g_autofree char *normalized = NULL;

	text = gdk_clipboard_read_text_finish(clipboard, res, &error);
	if (text == NULL) {
		g_warning("Failed to read clipboard text: %s.", error->message);
		return;
	}
	normalized = rf_clipboard_text_normalize(text);
	if (g_strcmp0(text, normalized) != 0)
		g_message("Clipboard: Normalized JSON Unicode escape text.");
	g_debug("Clipboard: Got new text %s.", normalized);
	send_clipboard_text_msg(this, normalized);
}

static void on_clipboard_changed(GdkClipboard *clipboard, void *data)
{
	struct this *this = data;
	const bool is_local = gdk_clipboard_is_local(clipboard);

	if (is_local) {
		g_message("Clipboard: Ignoring local clipboard owner change.");
		return;
	}

	GdkContentFormats *formats = gdk_clipboard_get_formats(clipboard);
	g_autofree char *formats_string = gdk_content_formats_to_string(formats);
	const bool has_text =
		gdk_content_formats_contain_mime_type(formats, "text/plain") ||
		gdk_content_formats_contain_mime_type(
			formats,
			"text/plain;charset=utf-8"
		) ||
		gdk_content_formats_contain_gtype(formats, G_TYPE_STRING);

	g_message(
		"Clipboard: Changed local=%s text=%s formats=%s.",
		is_local ? "yes" : "no",
		has_text ? "yes" : "no",
		formats_string != NULL ? formats_string : "(none)"
	);
	if (!has_text)
		return;

	gdk_clipboard_read_text_async(
		clipboard, NULL, on_read_text_finish, this
	);
}

static ssize_t
on_clipboard_text_msg(struct this *this, GSocketConnection *connection)
{
	g_autofree char *msg = NULL;
	size_t length = 0;
	ssize_t ret = 0;
	gsize read = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));

	if (!g_input_stream_read_all(
		    is,
		    &length,
		    sizeof(length),
		    &read,
		    NULL,
		    &error
	    ) || read != sizeof(length)) {
		ret = error != NULL ? -1 : 0;
		goto out;
	}

	msg = g_malloc0(length);
	if (!g_input_stream_read_all(is, msg, length, &read, NULL, &error) ||
	    read != length) {
		ret = error != NULL ? -1 : 0;
		goto out;
	}
	ret = read;

	gdk_clipboard_set_text(this->clipboard, msg);

out:
	if (ret < 0)
		g_warning(
			"Failed to receive clipboard text: %s.", error->message
		);
	else if (ret > 0)
		g_message(
			"Clipboard: Set local clipboard text length %zu.",
			length
		);
	return ret;
}

static int on_socket_in(GSocket *socket, GIOCondition condition, void *data)
{
	struct this *this = data;

	if (!(condition & this->io_flags))
		return G_SOURCE_CONTINUE;

	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	char type;
	g_autoptr(GSocketConnection) connection =
		g_socket_connection_factory_create_connection(socket);
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));
	ret = g_input_stream_read(is, &type, sizeof(type), NULL, &error);
	if (ret <= 0) {
		if (ret < 0)
			g_warning(
				"Failed to read message type: %s.",
				error->message
			);
		goto out;
	}

	switch (type) {
	case RF_MSG_TYPE_CLIPBOARD_TEXT:
		ret = on_clipboard_text_msg(this, connection);
		break;
	default:
		break;
	}

out:
	if (ret <= 0) {
		const char *socket_path =
			g_object_get_data(G_OBJECT(socket), "reframe-session-path");

		if (ret == 0)
			g_message("ReFrame Server disconnected.");
		if (socket_path != NULL)
			g_hash_table_remove(this->socket_paths, socket_path);
		g_hash_table_remove(this->sockets, socket);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void connect(
	struct this *this,
	const char *socket_path,
	bool socket_path_reserved
)
{
	g_debug("Socket: Connect to path %s.", socket_path);
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketAddress)
		address = g_unix_socket_address_new(socket_path);
	g_autoptr(GSocketClient) client = g_socket_client_new();

	if (!socket_path_reserved) {
		if (g_hash_table_contains(this->socket_paths, socket_path)) {
			g_debug(
				"Socket: Already connected or connecting to path %s.",
				socket_path
			);
			return;
		}
		g_hash_table_add(this->socket_paths, g_strdup(socket_path));
	}

	g_autoptr(GSocketConnection) connection = g_socket_client_connect(
		client, G_SOCKET_CONNECTABLE(address), NULL, &error
	);
	if (connection == NULL) {
		g_warning(
			"Failed to connect to ReFrame Server: %s",
			error->message
		);
		g_hash_table_remove(this->socket_paths, socket_path);
		return;
	}

	GSocket *socket = g_socket_connection_get_socket(connection);
	GSource *source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(on_socket_in), this, NULL);
	g_source_attach(source, NULL);
	g_object_set_data_full(
		G_OBJECT(socket),
		"reframe-session-path",
		g_strdup(socket_path),
		g_free
	);
	g_hash_table_insert(this->sockets, g_object_ref(socket), source);
}

struct connect_request {
	struct this *this;
	char *socket_path;
};

static gboolean connect_later(void *data)
{
	struct connect_request *request = data;

	connect(request->this, request->socket_path, true);
	g_free(request->socket_path);
	g_free(request);
	return G_SOURCE_REMOVE;
}

static void connect_deferred(struct this *this, const char *socket_path)
{
	struct connect_request *request = g_new0(struct connect_request, 1);

	if (g_hash_table_contains(this->socket_paths, socket_path)) {
		g_debug("Socket: Already connected or connecting to path %s.", socket_path);
		g_free(request);
		return;
	}
	g_hash_table_add(this->socket_paths, g_strdup(socket_path));

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

	if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) !=
	    G_FILE_TYPE_SPECIAL)
		return;

	g_autofree char *socket_path = g_file_get_path(file);
	g_debug("Socket: Got changed type %d for path %s",
		event_type,
		socket_path);
	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CREATED:
		connect_deferred(this, socket_path);
		break;
	// It should be enough to handle disconnect on transfer.
	// case G_FILE_MONITOR_EVENT_DELETED:
	// 	disconnect(this);
	// 	break;
	default:
		break;
	}
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
	// `gboolean` is `int`, but `bool` may be `char`! Passing `bool` pointer
	// to `GOptionContext` leads into overflow!
	int version = FALSE;
	g_autoptr(GError) error = NULL;
	GOptionEntry options[] = { { "version",
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
				     "Session socket dir to communicate.",
				     "DIR" },
				   { NULL,
				     0,
				     G_OPTION_FLAG_NONE,
				     G_OPTION_ARG_NONE,
				     NULL,
				     NULL,
				     NULL } };
	g_autoptr(GOptionContext)
		context = g_option_context_new(" - ReFrame Session");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_warning("Failed to parse options: %s.", error->message);
		g_clear_pointer(&error, g_error_free);
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	if (socket_dir == NULL)
		socket_dir = g_strdup("/tmp/reframe-session");

	const size_t length = strlen(socket_dir);
	if (socket_dir[length - 1] == '/')
		socket_dir[length - 1] = '\0';

	g_autofree struct this *this = g_malloc0(sizeof(*this));
	this->io_flags = G_IO_IN | G_IO_PRI;
	this->socket_paths = g_hash_table_new_full(
		g_str_hash,
		g_str_equal,
		g_free,
		NULL
	);

	// See <https://gitlab.gnome.org/GNOME/gtk/-/issues/1874>.
	//
	// Monitoring clipboard for unfocused window is not allowed by Wayland.
	// That's disappointing, we may add Wayland data-control implementation
	// and mutter specific implementation in future. But currently living
	// with X11 or Xwayland is enough.
	g_setenv("GDK_BACKEND", "x11", true);
	gtk_init();

	this->display = gdk_display_get_default();
	if (this->display == NULL)
		g_error("Failed to get the default GDK display.");
	this->clipboard = gdk_display_get_clipboard(this->display);
	if (this->clipboard == NULL)
		g_error("Failed to get clipboard.");
	g_signal_connect(
		this->clipboard,
		"changed",
		G_CALLBACK(on_clipboard_changed),
		this
	);

	this->sockets = g_hash_table_new_full(
		g_direct_hash,
		g_direct_equal,
		g_object_unref,
		(GDestroyNotify)g_source_unref
	);

	g_autoptr(GDir) dir = g_dir_open(socket_dir, 0, NULL);
	if (dir != NULL) {
		const char *name = NULL;
		while ((name = g_dir_read_name(dir)) != NULL) {
			g_autofree char *socket_path =
				g_build_filename(socket_dir, name, NULL);
			connect(this, socket_path, false);
		}
	}

	g_autoptr(GFile) file = g_file_new_for_path(socket_dir);
	g_autoptr(GFileMonitor) monitor = g_file_monitor_directory(
		file, G_FILE_MONITOR_NONE, NULL, &error
	);
	if (monitor == NULL)
		g_warning("Failed to monitor socket dir: %s", error->message);
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
	g_hash_table_remove_all(this->sockets);
	g_hash_table_unref(this->socket_paths);

	g_clear_object(&this->clipboard);
	if (this->display != NULL)
		gdk_display_close(this->display);
	g_clear_object(&this->display);

	return 0;
}
