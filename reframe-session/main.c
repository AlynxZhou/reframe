#include <locale.h>
#include <glib.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <gdk/gdk.h>

#include "config.h"
#include "rf-common.h"

struct _this {
	GMainLoop *main_loop;
	GHashTable *sockets;
	GIOCondition io_flags;
	GdkDisplay *display;
	GdkClipboard *clipboard;
};

static void
_send_clipboard_text_msg(struct _this *this, const char *clipboard_text)
{
	size_t length = strlen(clipboard_text) + 1;
	GHashTableIter it;
	void *key;
	void *value;
	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, &key, &value)) {
		ssize_t ret = 0;
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
		ret = g_output_stream_write(
			os, clipboard_text, length, NULL, &error
		);
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
		}
	}
}

static void
_on_read_text_finish(GObject *source_object, GAsyncResult *res, void *data)
{
	struct _this *this = data;
	GdkClipboard *clipboard = GDK_CLIPBOARD(source_object);
	g_autoptr(GError) error = NULL;
	g_autofree char *text = NULL;

	text = gdk_clipboard_read_text_finish(clipboard, res, &error);
	if (text == NULL) {
		g_warning("Failed to read clipboard text: %s.", error->message);
		return;
	}
	g_debug("Clipboard: Got new text %s.", text);
	_send_clipboard_text_msg(this, text);
}

static void _on_clipboard_changed(GdkClipboard *clipboard, void *data)
{
	struct _this *this = data;

	if (gdk_clipboard_is_local(clipboard))
		return;

	gdk_clipboard_read_text_async(
		clipboard, NULL, _on_read_text_finish, this
	);
}

static ssize_t
_on_clipboard_text_msg(struct _this *this, GSocketConnection *connection)
{
	g_autofree char *msg = NULL;
	size_t length = 0;
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0)
		goto out;

	msg = g_malloc0(length);
	ret = g_input_stream_read(is, msg, length, NULL, &error);
	if (ret <= 0)
		goto out;

	gdk_clipboard_set_text(this->clipboard, msg);

out:
	if (ret < 0)
		g_warning(
			"Failed to receive clipboard text: %s.", error->message
		);
	else if (ret > 0)
		g_debug("Clipboard: Received text %s.", msg);
	return ret;
}

static int _on_socket_in(GSocket *socket, GIOCondition condition, void *data)
{
	struct _this *this = data;

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
		ret = _on_clipboard_text_msg(this, connection);
		break;
	default:
		break;
	}

out:
	if (ret <= 0) {
		if (ret == 0)
			g_message("ReFrame Server disconnected.");
		g_hash_table_remove(this->sockets, socket);
		return G_SOURCE_REMOVE;
	}
	return G_SOURCE_CONTINUE;
}

static void _connect(struct _this *this, const char *socket_path)
{
	g_debug("Socket: Connect to path %s.", socket_path);
	g_autoptr(GError) error = NULL;
	g_autoptr(GSocketAddress)
		address = g_unix_socket_address_new(socket_path);
	g_autoptr(GSocketClient) client = g_socket_client_new();
	g_autoptr(GSocketConnection) connection = g_socket_client_connect(
		client, G_SOCKET_CONNECTABLE(address), NULL, &error
	);
	if (connection == NULL) {
		g_warning(
			"Failed to connect to ReFrame Server: %s",
			error->message
		);
		return;
	}

	GSocket *socket = g_socket_connection_get_socket(connection);
	GSource *source = g_socket_create_source(socket, this->io_flags, NULL);
	g_source_set_callback(source, G_SOURCE_FUNC(_on_socket_in), this, NULL);
	g_source_attach(source, NULL);
	g_hash_table_insert(this->sockets, g_object_ref(socket), source);
	// _send_clipboard_text_msg(this, "收到！");
}

static void _on_changed(
	GFileMonitor *monitor,
	GFile *file,
	GFile *other_file,
	GFileMonitorEvent event_type,
	void *data
)
{
	struct _this *this = data;

	if (g_file_query_file_type(file, G_FILE_QUERY_INFO_NONE, NULL) !=
	    G_FILE_TYPE_SPECIAL)
		return;

	g_autofree char *socket_path = g_file_get_path(file);
	g_debug("Socket: Got changed type %d for path %s",
		event_type,
		socket_path);
	switch (event_type) {
	case G_FILE_MONITOR_EVENT_CREATED:
		_connect(this, socket_path);
		break;
	// It should be enough to handle disconnect on transfer.
	// case G_FILE_MONITOR_EVENT_DELETED:
	// 	_disconnect(this);
	// 	break;
	default:
		break;
	}
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

	g_autofree struct _this *this = g_malloc0(sizeof(*this));
	this->io_flags = G_IO_IN | G_IO_PRI;

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
		G_CALLBACK(_on_clipboard_changed),
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
				g_strdup_printf("%s/%s", socket_dir, name);
			_connect(this, socket_path);
		}
	}

	g_autoptr(GFile) file = g_file_new_for_path(socket_dir);
	g_autoptr(GFileMonitor) monitor = g_file_monitor_directory(
		file, G_FILE_MONITOR_NONE, NULL, &error
	);
	if (monitor == NULL)
		g_warning("Failed to monitor socket dir: %s", error->message);
	g_signal_connect(monitor, "changed", G_CALLBACK(_on_changed), this);

	this->main_loop = g_main_loop_new(NULL, false);
	g_main_loop_run(this->main_loop);
	g_main_loop_unref(this->main_loop);

	GHashTableIter it;
	void *value;
	g_hash_table_iter_init(&it, this->sockets);
	while (g_hash_table_iter_next(&it, NULL, &value))
		g_source_destroy(value);
	g_hash_table_remove_all(this->sockets);

	g_clear_object(&this->clipboard);
	if (this->display != NULL)
		gdk_display_close(this->display);
	g_clear_object(&this->display);

	return 0;
}
