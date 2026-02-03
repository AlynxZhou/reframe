#include <grp.h>
#include <sys/types.h>
#include <xf86drmMode.h>

#include "config.h"
#include "rf-common.h"

void rf_buffer_debug(struct rf_buffer *b)
{
	g_debug("Frame: Got buffer metadata: length %u, type %s, "
		"crtc_x %d, crtc_y %d, crtc_w %u, crtc_h %u, "
		"src_x %u, src_y %u, src_w %u, src_h %u, "
		"width %u, height %u, fourcc %c%c%c%c, modifier %#lx.",
		b->md.length,
		rf_plane_type(b->md.type),
		b->md.crtc_x,
		b->md.crtc_y,
		b->md.crtc_w,
		b->md.crtc_h,
		b->md.src_x,
		b->md.src_y,
		b->md.src_w,
		b->md.src_h,
		b->md.width,
		b->md.height,
		(b->md.fourcc >> 0) & 0xff,
		(b->md.fourcc >> 8) & 0xff,
		(b->md.fourcc >> 16) & 0xff,
		(b->md.fourcc >> 24) & 0xff,
		b->md.modifier);
	g_debug("Frame: Got buffer fds: %d %d %d %d.",
		b->fds[0],
		b->fds[1],
		b->fds[2],
		b->fds[3]);
	g_debug("Frame: Got buffer offsets: %u %u %u %u.",
		b->md.offsets[0],
		b->md.offsets[1],
		b->md.offsets[2],
		b->md.offsets[3]);
	g_debug("Frame: Got buffer pitches: %u %u %u %u.",
		b->md.pitches[0],
		b->md.pitches[1],
		b->md.pitches[2],
		b->md.pitches[3]);
}

ssize_t rf_send_header(
	GSocketConnection *connection,
	char type,
	size_t length,
	GError **error
)
{
	ssize_t ret = 0;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(connection));
	ret = g_output_stream_write(os, &type, sizeof(type), NULL, error);
	if (ret <= 0)
		return ret;
	ret = g_output_stream_write(os, &length, sizeof(length), NULL, error);
	return ret;
}

const char *rf_plane_type(uint32_t type)
{
	switch (type) {
	case DRM_PLANE_TYPE_OVERLAY:
		return "overlay";
	case DRM_PLANE_TYPE_PRIMARY:
		return "primary";
	case DRM_PLANE_TYPE_CURSOR:
		return "cursor";
	default:
		g_assert_not_reached();
	}
}

void rf_set_group(const char *path)
{
	struct group *grp = getgrnam(USERNAME);
	if (grp != NULL)
		chown(path, -1, grp->gr_gid);
}

pid_t rf_get_socket_pid(GSocket *socket)
{
	g_autoptr(GError) error = NULL;
	g_autoptr(GCredentials) cred = g_socket_get_credentials(socket, &error);
	if (cred == NULL) {
		g_warning(
			"Failed to get credentials of socket client: %s.",
			error->message
		);
		return -1;
	}
	pid_t pid = g_credentials_get_unix_pid(cred, &error);
	if (pid < 0)
		g_warning(
			"Failed to get PID of socket client: %s.",
			error->message
		);
	return pid;
}
