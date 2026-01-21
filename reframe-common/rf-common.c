#include <grp.h>
#include <sys/types.h>
#include <xf86drmMode.h>

#include "config.h"
#include "rf-common.h"

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
