#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>
#include <linux/uinput.h>

#include "config.h"
#include "rf-common.h"
#include "rf-config.h"
#include "rf-buffer.h"

#ifdef HAVE_LIBSYSTEMD
#include <systemd/sd-daemon.h>
#endif

#define _ioctl_must(...) \
	G_STMT_START { \
		int e; \
		if ((e = ioctl(__VA_ARGS__)))				\
			g_error("Failed to call ioctl() at line %d: %d.", __LINE__, e); \
	} G_STMT_END
#define _ioctl_may(...) \
	G_STMT_START { \
		int e; \
		if ((e = ioctl(__VA_ARGS__)))				\
			g_warning("Input: Failed to call ioctl() at line %d: %d.", __LINE__, e); \
	} G_STMT_END

#define _write_may(fd,buf,count) \
	G_STMT_START { \
		ssize_t e = write((fd), (buf), (count)); \
		if (e != (count)) \
			g_warning("Input: Failed to write %ld bytes to %d at line %d, actually wrote %ld bytes.", (count), (fd), __LINE__, e); \
	} G_STMT_END

struct _this {
	RfConfig *config;
	GSocketConnection *connection;
	int cfd;
	uint32_t connector_id;
	int ufd;
};

static uint32_t _get_connector_id(int cfd, drmModeRes *res,
				  const char *connector_name)
{
	uint32_t id = 0;
	bool found = false;
	for (int i = 0; i < res->count_connectors; ++i) {
		drmModeConnector *connector =
			drmModeGetConnector(cfd, res->connectors[i]);
		g_autofree char *full_name = g_strdup_printf(
			"%s-%d",
			drmModeGetConnectorTypeName(connector->connector_type),
			connector->connector_type_id);
		g_debug("Connector %s is %s.", full_name,
			connector->connection == DRM_MODE_CONNECTED ?
				"connected" :
				"disconnected");
		id = connector->connector_id;
		drmModeFreeConnector(connector);
		if (connector->connection == DRM_MODE_CONNECTED &&
		    g_strcmp0(full_name, connector_name) == 0) {
			found = true;
			break;
		}
	}
	return found ? id : 0;
}

static uint32_t _get_fb_id(int cfd, uint32_t connector_id)
{
	uint32_t id = 0;
	drmModeConnector *connector = NULL;
	drmModeEncoder *encoder = NULL;
	drmModeCrtc *crtc = NULL;

	connector = drmModeGetConnector(cfd, connector_id);
	if (connector == NULL)
		goto clean_connector;
	encoder = drmModeGetEncoder(cfd, connector->encoder_id);
	if (encoder == NULL)
		goto clean_encoder;
	crtc = drmModeGetCrtc(cfd, encoder->crtc_id);
	if (crtc == NULL)
		goto clean_crtc;
	id = crtc->buffer_id;

clean_crtc:
	drmModeFreeCrtc(crtc);
clean_encoder:
	drmModeFreeEncoder(encoder);
clean_connector:
	drmModeFreeConnector(connector);

	return id;
}

static int _export_fb2(struct _this *this, uint32_t fb_id, RfBuffer *b)
{
	drmModeFB2 *fb = drmModeGetFB2(this->cfd, fb_id);
	if (fb == NULL)
		return -1;
	g_debug("Frame: Got FB2 frame %u.", fb->fb_id);
	for (int i = 0; i < RF_MAX_PLANES; ++i) {
		if (fb->handles[i] == 0)
			break;
		drmPrimeHandleToFD(this->cfd, fb->handles[i],
				   DRM_CLOEXEC, &b->fds[i]);
		if (b->fds[i] < 0)
			break;
		++b->md.length;
	}
	b->md.width = fb->width;
	b->md.height = fb->height;
	b->md.fourcc = fb->pixel_format;
	b->md.modifier = fb->flags & DRM_MODE_FB_MODIFIERS ?
		fb->modifier :
		DRM_FORMAT_MOD_INVALID;
	for (int i = 0; i < b->md.length; ++i) {
		b->md.offsets[i] = fb->offsets[i];
		b->md.pitches[i] = fb->pitches[i];
	}
	drmModeFreeFB2(fb);
	return b->md.length;
}

static int _export_fb(struct _this *this, uint32_t fb_id, RfBuffer *b)
{
	drmModeFB *fb = drmModeGetFB(this->cfd, fb_id);
	if (fb == NULL)
		return -1;
	g_debug("Frame: Got FB frame %u.", fb->fb_id);
	if (fb->handle == 0)
		return 0;
	drmPrimeHandleToFD(this->cfd, fb->handle, DRM_CLOEXEC, &b->fds[0]);
	if (b->fds[0] < 0)
		return 0;
	b->md.length = 1;
	b->md.width = fb->width;
	b->md.height = fb->height;
	b->md.fourcc = DRM_FORMAT_XRGB8888;
	b->md.modifier = DRM_FORMAT_MOD_INVALID;
	b->md.offsets[0] = 0;
	b->md.pitches[0] = fb->pitch;
	drmModeFreeFB(fb);
	return b->md.length;
}

static ssize_t _on_frame_request(struct _this *this)
{
	g_debug("Frame: Received frame request.");

	uint32_t fb_id = _get_fb_id(this->cfd, this->connector_id);
	ssize_t ret = 0;
	RfBuffer b;
	// GUnixFDList refuses to send invalid fds like -1, so we need
	// to pass the number of fds. We assume valid planes are
	// continuous.
	for (int i = 0; i < RF_MAX_PLANES; ++i) {
		b.fds[i] = -1;
		b.md.offsets[i] = 0;
		b.md.pitches[i] = 0;
	}
	b.md.length = 0;
	// Export DRM framebuffer to fds and metadata that EGL can import.
	ret = _export_fb2(this, fb_id, &b);
	if (ret <= 0)
		ret = _export_fb(this, fb_id, &b);
	if (ret <= 0) {
		g_warning("Frame: Failed to get frame.");
		return ret;
	}

	g_debug("Frame: Got frame metadata: length %u, width %u, height %u, fourcc %c%c%c%c, modifier %#lx.",
		b.md.length, b.md.width, b.md.height,
		(b.md.fourcc >> 0) & 0xff,
		(b.md.fourcc >> 8) & 0xff,
		(b.md.fourcc >> 16) & 0xff,
		(b.md.fourcc >> 24) & 0xff, b.md.modifier);
	g_debug("Frame: Got frame fds: %d %d %d %d.", b.fds[0], b.fds[1],
		b.fds[2], b.fds[3]);
	g_debug("Frame: Got frame offsets: %u %u %u %u.", b.md.offsets[0],
		b.md.offsets[1], b.md.offsets[2],
		b.md.offsets[3]);
	g_debug("Frame: Got frame pitches: %u %u %u %u.", b.md.pitches[0],
		b.md.pitches[1], b.md.pitches[2],
		b.md.pitches[3]);

	GOutputVector iov = { &b.md, sizeof(b.md) };
	GUnixFDList *fds = g_unix_fd_list_new();
	// This won't take the ownership so we need to close fds.
	//
	// See <https://docs.gtk.org/gio/method.UnixFDList.append.html>.
	for (int i = 0; i < b.md.length; ++i)
		g_unix_fd_list_append(fds, b.fds[i], NULL);
	// This won't take the ownership so we need to free GUnixFDList.
	GSocketControlMessage *msg =
		g_unix_fd_message_new_with_fd_list(fds);
	GSocket *socket = g_socket_connection_get_socket(this->connection);
	ret = g_socket_send_message(socket, NULL, &iov, 1, &msg,
				    1, G_SOCKET_MSG_NONE, NULL,
				    NULL);
	g_object_unref(fds);
	g_object_unref(msg);

	for (int i = 0; i < b.md.length; ++i)
		close(b.fds[i]);

	if (ret < 0)
		g_warning("Frame: Failed to send frame to socket.");
	return ret;
}

static ssize_t _on_input_request(struct _this *this)
{
	g_autofree struct input_event *ies = NULL;
	size_t length = 0;
	ssize_t ret = 0;
	GInputStream *is = g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, NULL);
	if (ret <= 0) {
		if (ret < 0)
			g_warning("Input: Failed to receive input events length from socket.");
		return ret;
	}
	ies = g_malloc_n(length, sizeof(*ies));
	ret = g_input_stream_read(is, ies, length * sizeof(*ies), NULL, NULL);
	if (ret < 0) {
		if (ret < 0)
			g_warning("Input: Failed to receive %lu * %ld bytes input events length from socket.", length, sizeof(*ies));
		return ret;
	}
	g_debug("Input: Received %lu * %ld bytes input events.", length, sizeof(*ies));
	_write_may(this->ufd, ies, length * sizeof(*ies));

	return ret;
}

static void _init_drm(struct _this *this)
{
	g_autofree char *card_path = rf_config_get_card_path(this->config);
	this->cfd = open(card_path, O_RDONLY | O_CLOEXEC);
	if (this->cfd <= 0)
		g_error("Failed to open card %s: %s.", card_path, strerror(errno));

	drmModeRes *res = NULL;
	g_autofree char *connector_name = rf_config_get_connector(this->config);
	res = drmModeGetResources(this->cfd);
	this->connector_id = _get_connector_id(this->cfd, res, connector_name);
	drmModeFreeResources(res);
	if (this->connector_id == 0)
		g_error("Failed to find a connected connector %s.",
			connector_name);
	g_message("Found connected connector %s.", connector_name);
}

static void _init_uinput(struct _this *this)
{
	this->ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (this->ufd <= 0)
		g_error("Failed to open uinput: %s.", strerror(errno));

	_ioctl_must(this->ufd, UI_SET_EVBIT, EV_KEY);
	_ioctl_must(this->ufd, UI_SET_EVBIT, EV_SYN);
	for (int i = 0; i < RF_KEYBOARD_MAX; ++i)
		_ioctl_must(this->ufd, UI_SET_KEYBIT, i);

	_ioctl_must(this->ufd, UI_SET_EVBIT, EV_ABS);
	_ioctl_must(this->ufd, UI_SET_ABSBIT, ABS_X);
	_ioctl_must(this->ufd, UI_SET_ABSBIT, ABS_Y);

	_ioctl_must(this->ufd, UI_SET_EVBIT, EV_REL);
	_ioctl_must(this->ufd, UI_SET_RELBIT, REL_X);
	_ioctl_must(this->ufd, UI_SET_RELBIT, REL_Y);

	_ioctl_must(this->ufd, UI_SET_KEYBIT, BTN_LEFT);
	_ioctl_must(this->ufd, UI_SET_KEYBIT, BTN_MIDDLE);
	_ioctl_must(this->ufd, UI_SET_KEYBIT, BTN_RIGHT);

	_ioctl_must(this->ufd, UI_SET_EVBIT, EV_REL);
	_ioctl_must(this->ufd, UI_SET_RELBIT, REL_WHEEL);

	struct uinput_abs_setup abs = {0};
	abs.absinfo.maximum = RF_POINTER_MAX;
	abs.absinfo.minimum = 0;
	abs.code = ABS_X;
	_ioctl_must(this->ufd, UI_ABS_SETUP, &abs);
	abs.code = ABS_Y;
	_ioctl_must(this->ufd, UI_ABS_SETUP, &abs);

	struct uinput_setup dev = {0};
	dev.id.bustype = BUS_USB;
	dev.id.vendor = 0xa3a7;
	dev.id.product = 0x0003;
	strcpy(dev.name, "reframe");
	_ioctl_must(this->ufd, UI_DEV_SETUP, &dev);
	_ioctl_must(this->ufd, UI_DEV_CREATE);
}

int main(int argc, char *argv[])
{
	g_autofree char *config_path = NULL;
	g_autofree char *socket_path = NULL;
	// `gboolean` is `int`, but `bool` may be `char`! Passing `bool` pointer
	// to `GOptionContext` leads into overflow!
	gboolean keep_listen = FALSE;
	gboolean version = FALSE;
	g_autofree char **args = g_strdupv(argv);
	g_autoptr(GError) error = NULL;

	GOptionEntry options[] = {
		{ "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
		  &version, "Display version and exit.", NULL },
		{ "socket", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
		  &socket_path, "Socket path to communiate.", "SOCKET" },
		{ "config", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_FILENAME,
		  &config_path, "Configuration file path.", "PATH" },
		{ "keep-listen", 'k', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
		  &keep_listen,
		  "Keep listening to socket after disconnection (debug purpose).",
		  NULL },
		{ NULL, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, NULL,
		  NULL }
	};
	g_autoptr(GOptionContext) context = g_option_context_new(" - ReFrame Streamer");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse_strv(context, &args, &error)) {
		g_warning("Failed to parse options: %s.", error->message);
		g_clear_pointer(&error, g_error_free);
	}
	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}
	// Use `g_strdup()` here to make `g_autofree` happy.
	if (socket_path == NULL)
		socket_path = g_strdup("/tmp/reframe.sock");

	g_autofree struct _this *this = g_malloc0(sizeof(*this));
	g_message("Using configuration file %s.", config_path);
	this->config = rf_config_new(config_path);

	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_message("Using socket %s.", socket_path);

#ifdef HAVE_LIBSYSTEMD
	if (sd_listen_fds(0) != 0) {
		// systemd socket.
		// We only handle 1 socket.
		int sfd = SD_LISTEN_FDS_START;
		g_autoptr(GSocket) socket = g_socket_new_from_fd(sfd, &error);
		if (error != NULL)
			g_error("Failed to create socket from systemd fd: %s.", error->message);
		g_socket_listener_add_socket(listener, socket, NULL, &error);
	} else {
#endif
		g_remove(socket_path);
		// Non-systemd socket.
		g_autoptr(GSocketAddress) address = g_unix_socket_address_new(socket_path);
		g_socket_listener_add_address(listener, address, G_SOCKET_TYPE_STREAM,
				      G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
				      &error);
#ifdef HAVE_LIBSYSTEMD
	}
#endif
	if (error != NULL)
		g_error("Failed to listen to socket: %s.", error->message);

	g_message("Keep listening mode is %s.",
		keep_listen ? "enabled" : "disabled");
	do {
		this->connection =
			g_socket_listener_accept(listener, NULL, NULL, &error);
		if (this->connection == NULL)
			g_error("Failed to accept connection: %s.", error->message);

		g_message("ReFrame Server connected.");

		_init_drm(this);
		_init_uinput(this);

		while (true) {
			ssize_t ret = 0;
			GInputStream *is = g_io_stream_get_input_stream(
				G_IO_STREAM(this->connection));
			char request;
			ret = g_input_stream_read(is, &request, sizeof(request), NULL, NULL);
			if (ret == 0) {
				g_message("ReFrame Server disconnected.");
				break;
			} else if (ret < 0) {
				g_warning("Failed to receive request from socket.");
				break;
			}

			switch (request) {
			case RF_REQUEST_TYPE_FRAME:
				ret = _on_frame_request(this);
				break;
			case RF_REQUEST_TYPE_INPUT:
				ret = _on_input_request(this);
				break;
			default:
				break;
			}
			if (ret <= 0) {
				if (ret == 0)
					g_message("ReFrame Server disconnected.");
				break;
			}
		}

		_ioctl_may(this->ufd, UI_DEV_DESTROY);
		close(this->ufd);
		this->ufd = 0;

		close(this->cfd);
		this->cfd = 0;

		g_io_stream_close(G_IO_STREAM(this->connection), NULL, &error);
		if (error != NULL)
			g_error("Failed to close socket connection: %s.", error->message);
		g_clear_object(&this->connection);
	} while (keep_listen);

	g_socket_listener_close(listener);
	g_object_unref(this->config);

	return 0;
}
