#include <stdint.h>
#include <stdbool.h>
#include <glib.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdmessage.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <libdrm/drm_fourcc.h>

#include "config.h"
#include "rf-common.h"
#include "rf-buffer.h"
#include "rf-config.h"

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
		g_debug("%s: %s", full_name,
			connector->connection == DRM_MODE_CONNECTED ?
				"connected" :
				"disconnected");
		id = connector->connector_id;
		drmModeFreeConnector(connector);
		// TODO: Get connector by name.
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

int main(int argc, char *argv[])
{
	g_autofree char *config_path = NULL;
	g_autofree char *socket_path = g_strdup("/tmp/reframe.sock");
	bool keep_listen = false;
	bool version = false;
	g_autoptr(GError) error = NULL;

	GOptionEntry options[] = {
		{ "version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
		  &version, "Display version and exit.", NULL },
		{ "socket", 's', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
		  &socket_path, "Socket path of streamer.", "SOCKET" },
		{ "config", 'c', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING,
		  &config_path, "Configuration file path.", "PATH" },
		{ "keep-listen", 'k', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
		  &keep_listen,
		  "Keep listening to socket after disconnection (debug purpose).",
		  NULL },
		{ NULL, 0, G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, NULL, NULL,
		  NULL }
	};
	GOptionContext *context = g_option_context_new(" - ReFrame streamer");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_error("Failed to parse options.");
		return EXIT_FAILURE;
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	g_debug("Use configuration file %s", config_path);
	RfConfig *config = rf_config_new(config_path);
	g_autofree char *card_path = rf_config_get_card_path(config);
	g_remove(socket_path);
	int cfd = -1;
	cfd = open(card_path, O_RDONLY | O_CLOEXEC);
	if (cfd < 0) {
		g_error("Open card failed: %s", strerror(errno));
	}

	drmModeRes *res = NULL;
	uint32_t connector_id = 0;
	g_autofree char *connector_name = rf_config_get_connector(config);
	;
	res = drmModeGetResources(cfd);
	connector_id = _get_connector_id(cfd, res, connector_name);
	drmModeFreeResources(res);

	if (connector_id == 0) {
		g_error("Failed to find a connected connector %s.",
			connector_name);
		close(cfd);
		return EXIT_FAILURE;
	}
	g_debug("Find connected connector %s.", connector_name);

	g_autoptr(GSocketListener) listener = g_socket_listener_new();

	// Non-systemd socket.
	GSocketAddress *address = g_unix_socket_address_new(socket_path);
	g_socket_listener_add_address(listener, address, G_SOCKET_TYPE_STREAM,
				      G_SOCKET_PROTOCOL_DEFAULT, NULL, NULL,
				      NULL);
	// TODO: systemd socket.
	// int sfd = 0;
	// GSocket *socket = g_socket_new_from_fd(sfd, NULL);
	// g_socket_listener_add_socket(listener, socket, NULL, NULL);

	g_debug("Keep listening mode is %s.",
		keep_listen ? "enabled" : "disabled");
	while (keep_listen) {
		GSocketConnection *connection =
			g_socket_listener_accept(listener, NULL, NULL, NULL);
		if (connection == NULL) {
			g_error("Error listening to socket.");
			return EXIT_FAILURE;
		}
		GSocket *socket = g_socket_connection_get_socket(connection);

		while (true) {
			ssize_t ret;
			GInputStream *is = g_io_stream_get_input_stream(
				G_IO_STREAM(connection));
			char ready;
			// TODO: Read input line by line, and check whether type is input event or ready.
			ret = g_input_stream_read(is, &ready, 1, NULL, NULL);
			if (ret == 0) {
				g_debug("ReFrame server disconnected.");
				// g_application_release(app);
				break;
			} else if (ret < 0) {
				g_error("Read socket failed.");
				break;
			}

			g_debug("Received ready from ReFrame server.");

			RfBuffer b;
			uint32_t fb_id = _get_fb_id(cfd, connector_id);
			drmModeFB2 *fb = drmModeGetFB2(cfd, fb_id);
			if (fb == NULL)
				continue;
			g_debug("Get new framebuffer %u.", fb->fb_id);
			// GUnixFDList refuses to send invalid fds like -1, so we need
			// to pass the number of fds. We assume valid planes are
			// continuous.
			for (int i = 0; i < RF_MAX_PLANES; ++i)
				b.fds[i] = -1;
			b.md.length = 0;
			for (int i = 0; i < RF_MAX_PLANES; ++i) {
				if (fb->handles[i] == 0)
					break;
				drmPrimeHandleToFD(cfd, fb->handles[i],
						   DRM_CLOEXEC, &b.fds[i]);
				++b.md.length;
			}
			b.md.width = fb->width;
			b.md.height = fb->height;
			b.md.fourcc = fb->pixel_format;
			b.md.modifier = fb->flags & DRM_MODE_FB_MODIFIERS ?
						fb->modifier :
						DRM_FORMAT_MOD_INVALID;
			// b.md.modifier = fb->modifier;
			for (int i = 0; i < RF_MAX_PLANES; ++i) {
				if (fb->handles[i] != 0) {
					b.md.offsets[i] = fb->offsets[i];
					b.md.pitches[i] = fb->pitches[i];
				} else {
					b.md.offsets[i] = 0;
					b.md.pitches[i] = 0;
				}
			}

			g_debug("length: %u, width: %u, height: %u, fourcc: %c%c%c%c, modifier: %#lx",
				b.md.length, b.md.width, b.md.height,
				(b.md.fourcc >> 0) & 0xff,
				(b.md.fourcc >> 8) & 0xff,
				(b.md.fourcc >> 16) & 0xff,
				(b.md.fourcc >> 24) & 0xff, b.md.modifier);
			g_debug("fds: %d %d %d %d", b.fds[0], b.fds[1],
				b.fds[2], b.fds[3]);
			g_debug("offsets: %u %u %u %u", b.md.offsets[0],
				b.md.offsets[1], b.md.offsets[2],
				b.md.offsets[3]);
			g_debug("pitches: %u %u %u %u", b.md.pitches[0],
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
			ret = g_socket_send_message(socket, NULL, &iov, 1, &msg,
						    1, G_SOCKET_MSG_NONE, NULL,
						    NULL);
			if (ret < 0) {
				g_error("Failed to send fds via socket.");
			}
			g_object_unref(fds);
			g_object_unref(msg);
			for (int i = 0; i < b.md.length; ++i)
				close(b.fds[i]);

			drmModeFreeFB2(fb);
		}
	}

	g_socket_listener_close(listener);
	g_unlink(socket_path);
	close(cfd);

	return 0;
}
