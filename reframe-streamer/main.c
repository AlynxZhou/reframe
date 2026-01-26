#include <stdint.h>
#include <stdbool.h>
#include <locale.h>
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
#	include <systemd/sd-daemon.h>
#endif

#define WAKEUP_MAX_EVENTS 4

// clang-format off
#define _ioctl_must(...)                                                         \
	G_STMT_START {                                                           \
		int e;                                                           \
		if ((e = ioctl(__VA_ARGS__)))                                    \
			g_error("Input: Failed to call ioctl() at line %d: %d.", \
				__LINE__,                                        \
				e);                                              \
	} G_STMT_END
#define _ioctl_may(...)                                                          \
	G_STMT_START {                                                           \
		int e;                                                           \
		if ((e = ioctl(__VA_ARGS__)))                                    \
			g_warning(                                               \
				"Input: Failed to call ioctl() at line %d: %d.", \
				__LINE__,                                        \
				e                                                \
			);                                                       \
	} G_STMT_END

#define _write_may(fd, buf, count)                                                                              \
	G_STMT_START {                                                                                          \
		ssize_t e = write((fd), (buf), (count));                                                        \
		if (e != (count))                                                                               \
			g_warning(                                                                              \
				"Input: Failed to write %ld bytes to %d at line %d, actually wrote %ld bytes.", \
				(count),                                                                        \
				(fd),                                                                           \
				__LINE__,                                                                       \
				e                                                                               \
			);                                                                                      \
	} G_STMT_END
// clang-format on

struct _this {
	RfConfig *config;
	GSocketConnection *connection;
	char *card_path;
	char *connector_name;
	int cfd;
	uint32_t crtc_id;
	uint32_t primary_id;
	bool cursor;
	uint32_t cursor_id;
	int ufd;
	bool wakeup;
	bool skip_auth;
};

static int _auth_pid(struct _this *this, pid_t pid, const char *target)
{
	if (this->skip_auth)
		return 0;

	g_autoptr(GError) error = NULL;
	g_autofree char *proc_exe = g_strdup_printf("/proc/%d/exe", pid);
	g_autofree char *bin = g_file_read_link(proc_exe, &error);
	if (bin == NULL) {
		g_warning(
			"Auth: Failed to read link of %s: %s.",
			proc_exe,
			error->message
		);
		return -1;
	}
	g_debug("Auth: Authenticating process executable binary %s.", bin);
	if (g_strcmp0(bin, target) != 0)
		return -2;
	return 0;
}

static ssize_t _send_auth_msg(struct _this *this, pid_t pid, bool ok)
{
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));

	ret = rf_send_header(this->connection, RF_MSG_TYPE_AUTH, 1, &error);
	if (ret <= 0)
		goto out;
	struct rf_auth auth;
	auth.pid = pid;
	auth.ok = ok;
	ret = g_output_stream_write(os, &auth, sizeof(auth), NULL, &error);

out:
	if (ret < 0)
		g_warning(
			"Auth: Failed to send auth message: %s.", error->message
		);
	else if (ret > 0)
		g_debug("Auth: Sent auth message for PID %d with result %s.",
			auth.pid,
			auth.ok ? "OK" : "not OK");
	return ret;
}

static ssize_t _on_auth_msg(struct _this *this)
{
	ssize_t ret = 0;
	size_t length = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0 || length != 1)
		goto out;
	pid_t pid = -1;
	ret = g_input_stream_read(is, &pid, sizeof(pid), NULL, &error);
	if (ret <= 0 || pid < 0)
		goto out;

	g_debug("Auth: Received auth message for PID %d.", pid);
	bool ok = _auth_pid(this, pid, BINDIR "/reframe-session") == 0;
	return _send_auth_msg(this, pid, ok);

out:
	if (ret < 0)
		g_warning(
			"Auth: Failed to receive auth message: %s.",
			error->message
		);
	return ret;
}

// You need to explicitly cast the type of returned value.
static uint64_t _get_plane_prop(
	int cfd,
	uint32_t plane_id,
	const char *name,
	uint64_t default_value
)
{
	uint64_t value = default_value;
	drmModeObjectProperties *props = drmModeObjectGetProperties(
		cfd, plane_id, DRM_MODE_OBJECT_PLANE
	);
	if (props == NULL)
		return value;
	for (size_t i = 0; i < props->count_props; ++i) {
		drmModePropertyRes *prop =
			drmModeGetProperty(cfd, props->props[i]);
		if (prop == NULL)
			continue;
		if (g_strcmp0(prop->name, name) == 0)
			value = props->prop_values[i];
		drmModeFreeProperty(prop);
	}
	drmModeFreeObjectProperties(props);
	return value;
}

// We have to get plane via ID every frame to get the newest framebuffer.
static uint32_t _get_plane_id(int cfd, uint32_t crtc_id, uint32_t type)
{
	uint32_t plane_id = 0;
	drmModePlaneRes *pres = drmModeGetPlaneResources(cfd);
	if (pres == NULL) {
		g_warning("DRM: Failed to get plane resources.");
		return 0;
	}
	g_debug("DRM: Finding plane of type %s and CRTC ID %u.",
		rf_plane_type(type),
		crtc_id);
	for (size_t i = 0; i < pres->count_planes; ++i) {
		if (_get_plane_prop(
			    cfd, pres->planes[i], "type", DRM_PLANE_TYPE_OVERLAY
		    ) != type)
			continue;
		drmModePlane *plane = drmModeGetPlane(cfd, pres->planes[i]);
		if (plane == NULL)
			continue;
		// Ignore unrelated planes.
		g_debug("DRM: Plane ID %u is of type %s belongs to CRTC ID %u.",
			plane->plane_id,
			rf_plane_type(type),
			plane->crtc_id);
		if (plane->crtc_id == crtc_id)
			plane_id = plane->plane_id;
		drmModeFreePlane(plane);
		if (plane_id != 0)
			break;
	}
	drmModeFreePlaneResources(pres);
	return plane_id;
}

static int _export_fb2(int cfd, RfBuffer *b, uint32_t fb_id)
{
	drmModeFB2 *fb = drmModeGetFB2(cfd, fb_id);
	if (fb == NULL)
		return 0;
	g_debug("Frame: Got FB2 framebuffer ID %u.", fb->fb_id);
	for (int i = 0; i < RF_MAX_FDS; ++i) {
		if (fb->handles[i] == 0)
			break;
		drmPrimeHandleToFD(cfd, fb->handles[i], DRM_CLOEXEC, &b->fds[i]);
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

static int _export_fb(int cfd, RfBuffer *b, uint32_t fb_id)
{
	drmModeFB *fb = drmModeGetFB(cfd, fb_id);
	if (fb == NULL)
		return 0;
	g_debug("Frame: Got FB framebuffer ID %u.", fb->fb_id);
	if (fb->handle == 0)
		return 0;
	drmPrimeHandleToFD(cfd, fb->handle, DRM_CLOEXEC, &b->fds[0]);
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

static int _make_buffer(int cfd, RfBuffer *b, uint32_t plane_id, uint32_t type)
{
	drmModePlane *plane = drmModeGetPlane(cfd, plane_id);
	if (plane == NULL)
		return 0;
	uint32_t fb_id = plane->fb_id;
	drmModeFreePlane(plane);
	if (fb_id == 0)
		return 0;
	g_debug("Frame: Got %s plane framebuffer ID %u.",
		rf_plane_type(type),
		fb_id);
	b->md.type = type;
	b->md.crtc_x = (int32_t)_get_plane_prop(cfd, plane_id, "CRTC_X", 0);
	b->md.crtc_y = (int32_t)_get_plane_prop(cfd, plane_id, "CRTC_Y", 0);
	b->md.crtc_w = (uint32_t)_get_plane_prop(cfd, plane_id, "CRTC_W", 0);
	b->md.crtc_h = (uint32_t)_get_plane_prop(cfd, plane_id, "CRTC_H", 0);
	// These are in 16.16 fixed point, we only need integer.
	b->md.src_x =
		(uint32_t)(_get_plane_prop(cfd, plane_id, "SRC_X", 0) >> 16);
	b->md.src_y =
		(uint32_t)(_get_plane_prop(cfd, plane_id, "SRC_Y", 0) >> 16);
	b->md.src_w =
		(uint32_t)(_get_plane_prop(cfd, plane_id, "SRC_W", 0) >> 16);
	b->md.src_h =
		(uint32_t)(_get_plane_prop(cfd, plane_id, "SRC_H", 0) >> 16);

	int ret = 0;
	// GUnixFDList refuses to send invalid fds like -1, so we need
	// to pass the number of fds. We assume valid planes are
	// continuous.
	b->md.length = 0;
	for (int i = 0; i < RF_MAX_FDS; ++i) {
		b->fds[i] = -1;
		b->md.offsets[i] = 0;
		b->md.pitches[i] = 0;
	}
	// Export DRM framebuffer to fds and metadata that EGL can import.
	ret = _export_fb2(cfd, b, fb_id);
	if (ret <= 0)
		ret = _export_fb(cfd, b, fb_id);
	if (ret <= 0)
		return ret;
	rf_buffer_debug(b);
	return ret;
}

static ssize_t
_send_buffer(GSocketConnection *connection, RfBuffer *b, GError **error)
{
	ssize_t ret = 0;
	GOutputVector iov = { &b->md, sizeof(b->md) };
	GUnixFDList *fds = g_unix_fd_list_new();
	// This won't take the ownership so we need to close fds.
	//
	// See <https://docs.gtk.org/gio/method.UnixFDList.append.html>.
	for (int i = 0; i < b->md.length; ++i)
		g_unix_fd_list_append(fds, b->fds[i], NULL);
	// This won't take the ownership so we need to free GUnixFDList.
	GSocketControlMessage *msg = g_unix_fd_message_new_with_fd_list(fds);
	GSocket *socket = g_socket_connection_get_socket(connection);
	ret = g_socket_send_message(
		socket, NULL, &iov, 1, &msg, 1, G_SOCKET_MSG_NONE, NULL, error
	);
	g_clear_object(&fds);
	g_clear_object(&msg);
	return ret;
}

static ssize_t
_send_frame_msg(struct _this *this, size_t length, RfBuffer *bufs)
{
	ssize_t ret = 0;
	g_autoptr(GError) error = NULL;

	ret = rf_send_header(
		this->connection, RF_MSG_TYPE_FRAME, length, &error
	);
	if (ret <= 0)
		goto out;
	for (size_t i = 0; i < length; ++i) {
		ret = _send_buffer(this->connection, &bufs[i], &error);
		if (ret <= 0)
			break;
	}

out:
	if (ret < 0)
		g_warning("Frame: Failed to send frame: %s.", error->message);
	return ret;
}

static ssize_t _on_frame_msg(struct _this *this)
{
	g_debug("Frame: Received frame message.");

	RfBuffer bufs[RF_MAX_BUFS];
	ssize_t ret = 0;
	size_t length = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0) {
		if (ret < 0)
			g_warning(
				"Frame: Failed to receive frame message: %s.",
				error->message
			);
		return ret;
	}

	length = 0;
	ret = _make_buffer(
		this->cfd,
		&bufs[length++],
		this->primary_id,
		DRM_PLANE_TYPE_PRIMARY
	);
	// Empty buffer, maybe locked screen and turned monitor off, skip it.
	if (ret <= 0) {
		g_debug("Frame: Got empty buffer for primary plane.");
		ret = _send_frame_msg(this, 0, NULL);
		return ret;
	}

	if (this->cursor && this->cursor_id == 0)
		this->cursor_id = _get_plane_id(
			this->cfd, this->crtc_id, DRM_PLANE_TYPE_CURSOR
		);
	if (this->cursor_id != 0) {
		ret = _make_buffer(
			this->cfd,
			&bufs[length++],
			this->cursor_id,
			DRM_PLANE_TYPE_CURSOR
		);
		// It is OK to ignore cursor plane if failed.
		if (ret <= 0)
			--length;
	}

	ret = _send_frame_msg(this, length, bufs);

	for (size_t i = 0; i < length; ++i)
		for (int j = 0; j < bufs[i].md.length; ++j)
			close(bufs[i].fds[j]);
	return ret;
}

static ssize_t _on_input_msg(struct _this *this)
{
	g_debug("Input: Received input message.");

	g_autofree struct input_event *ies = NULL;
	ssize_t ret = 0;
	size_t length = 0;
	g_autoptr(GError) error = NULL;
	GInputStream *is =
		g_io_stream_get_input_stream(G_IO_STREAM(this->connection));

	ret = g_input_stream_read(is, &length, sizeof(length), NULL, &error);
	if (ret <= 0)
		goto out;
	ies = g_malloc_n(length, sizeof(*ies));
	ret = g_input_stream_read(is, ies, length * sizeof(*ies), NULL, &error);
	if (ret <= 0)
		goto out;

	_write_may(this->ufd, ies, length * sizeof(*ies));

out:
	if (ret < 0)
		g_warning(
			"Input: Failed to receive input events: %s.",
			error->message
		);
	else if (ret > 0)
		g_debug("Input: Received %lu * %ld bytes input events.",
			length,
			sizeof(*ies));
	return ret;
}

static ssize_t _send_card_path_msg(struct _this *this, const char *card_path)
{
	ssize_t ret = 0;
	size_t length = strlen(card_path) + 1;
	g_autoptr(GError) error = NULL;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));

	ret = rf_send_header(
		this->connection, RF_MSG_TYPE_CARD_PATH, length, &error
	);
	if (ret <= 0)
		goto out;
	ret = g_output_stream_write(os, card_path, length, NULL, &error);

out:
	if (ret < 0)
		g_warning("DRM: Failed to send card path: %s.", error->message);
	return ret;
}

static ssize_t
_send_connector_name_msg(struct _this *this, const char *connector_name)
{
	ssize_t ret = 0;
	size_t length = strlen(connector_name) + 1;
	g_autoptr(GError) error = NULL;
	GOutputStream *os =
		g_io_stream_get_output_stream(G_IO_STREAM(this->connection));

	ret = rf_send_header(
		this->connection, RF_MSG_TYPE_CONNECTOR_NAME, length, &error
	);
	if (ret <= 0)
		goto out;
	ret = g_output_stream_write(os, connector_name, length, NULL, &error);
out:
	if (ret < 0)
		g_warning(
			"DRM: Failed to send connector name: %s.",
			error->message
		);
	return ret;
}

static inline char *_get_connector_name(drmModeConnector *connector)
{
	return g_strdup_printf(
		"%s-%d",
		drmModeGetConnectorTypeName(connector->connector_type),
		connector->connector_type_id
	);
}

static drmModeConnector *_get_connector(int cfd, const char *connector_name)
{
	drmModeConnector *connector = NULL;
	drmModeRes *res = drmModeGetResources(cfd);
	if (res == NULL) {
		g_warning("DRM: Failed to get resources.");
		return NULL;
	}
	if (connector_name != NULL)
		g_debug("DRM: Finding connector for connector %s.",
			connector_name);
	for (int i = 0; i < res->count_connectors; ++i) {
		connector = drmModeGetConnector(cfd, res->connectors[i]);
		if (connector == NULL)
			continue;
		g_autofree char *full_name = _get_connector_name(connector);
		g_debug("DRM: Connector %s is %s.",
			full_name,
			connector->connection == DRM_MODE_CONNECTED ?
				"connected" :
				"disconnected");
		if (connector->connection == DRM_MODE_CONNECTED &&
		    (connector_name == NULL ||
		     g_strcmp0(full_name, connector_name) == 0))
			break;
		drmModeFreeConnector(connector);
		connector = NULL;
	}
	drmModeFreeResources(res);
	return connector;
}

static drmModeConnector *
_get_connected_card_and_connector(struct _this *this, const char *connector_name)
{
	g_autoptr(GDir) dir = g_dir_open("/dev/dri", 0, NULL);
	if (dir == NULL)
		return NULL;
	const char *name = NULL;
	while ((name = g_dir_read_name(dir)) != NULL) {
		if (!g_str_has_prefix(name, "card"))
			continue;
		g_autofree char *card_path =
			g_strdup_printf("/dev/dri/%s", name);
		int cfd = open(card_path, O_RDONLY | O_CLOEXEC);
		if (cfd < 0)
			continue;
		g_debug("DRM: Finding the first connected connector on card %s.",
			card_path);
		drmModeConnector *connector =
			_get_connector(cfd, connector_name);
		if (connector != NULL) {
			this->cfd = cfd;
			this->card_path = g_strdup(card_path);
			g_message(
				"DRM: Found the first connected connector on card %s.",
				card_path
			);
			return connector;
		}
		close(cfd);
	}
	return NULL;
}

static drmModeConnector *
_get_card_and_connector(struct _this *this, const char *connector_name)
{
	if (this->card_path == NULL)
		return _get_connected_card_and_connector(this, connector_name);
	this->cfd = open(this->card_path, O_RDONLY | O_CLOEXEC);
	if (this->cfd < 0) {
		g_warning(
			"DRM: Failed to open card %s: %s.",
			this->card_path,
			strerror(errno)
		);
		return NULL;
	}
	g_message("DRM: Opened card %s.", this->card_path);
	return _get_connector(this->cfd, connector_name);
}

static drmModeCrtc *_get_crtc(int cfd, drmModeConnector *connector)
{
	drmModeEncoder *encoder = NULL;
	drmModeCrtc *crtc = NULL;
	encoder = drmModeGetEncoder(cfd, connector->encoder_id);
	if (encoder == NULL)
		return NULL;
	crtc = drmModeGetCrtc(cfd, encoder->crtc_id);
	drmModeFreeEncoder(encoder);
	return crtc;
}

static void _setup_drm(struct _this *this)
{
	this->crtc_id = 0;
	this->primary_id = 0;
	this->cursor_id = 0;
	this->cursor = true;

	this->card_path = rf_config_get_card_path(this->config);
	this->connector_name = rf_config_get_connector(this->config);
	drmModeConnector *connector =
		_get_card_and_connector(this, this->connector_name);
	if (connector == NULL)
		g_error("DRM: Failed to find a connected connector.");

	// We may become DRM master if we are the first process that opens DRM
	// card, then drop DRM master so we could start compositor after ReFrame.
	drmDropMaster(this->cfd);

	if (this->connector_name == NULL)
		this->connector_name = _get_connector_name(connector);
	g_message("DRM: Found connected connector %s.", this->connector_name);

	drmModeCrtc *crtc = _get_crtc(this->cfd, connector);
	drmModeFreeConnector(connector);
	if (crtc == NULL)
		g_error("DRM: Failed to find a CRTC for connector.");
	this->crtc_id = crtc->crtc_id;
	drmModeFreeCrtc(crtc);

	// This is needed to get primary and cursor planes.
	if (drmSetClientCap(this->cfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1) < 0)
		g_warning("DRM: Failed to set universal planes capability.");
	// This is needed to get `CRTC_X/Y` properties of planes.
	if (drmSetClientCap(this->cfd, DRM_CLIENT_CAP_ATOMIC, 1) < 0)
		g_warning("DRM: Failed to set atomic capability.");

	this->primary_id =
		_get_plane_id(this->cfd, this->crtc_id, DRM_PLANE_TYPE_PRIMARY);
	if (this->primary_id == 0)
		g_error("DRM: Failed to find a primary plane for CRTC.");
	this->cursor = rf_config_get_cursor(this->config);
	g_message(
		"DRM: Cursor plane is %s.",
		this->cursor ? "enabled" : "disabled"
	);

	_send_card_path_msg(this, this->card_path);
	_send_connector_name_msg(this, this->connector_name);
}

static void _clean_drm(struct _this *this)
{
	if (this->cfd >= 0) {
		close(this->cfd);
		this->cfd = -1;
	}
	g_clear_pointer(&this->card_path, g_free);
	g_clear_pointer(&this->connector_name, g_free);
}

static void _wakeup_uinput(struct _this *this)
{
	struct input_event ies[WAKEUP_MAX_EVENTS];
	memset(ies, 0, WAKEUP_MAX_EVENTS * sizeof(*ies));

	ies[0].type = EV_REL;
	ies[0].code = REL_X;
	ies[0].value = 1;

	ies[1].type = EV_SYN;
	ies[1].code = SYN_REPORT;
	ies[1].value = 0;

	ies[2].type = EV_REL;
	ies[2].code = REL_Y;
	ies[2].value = 1;

	ies[3].type = EV_SYN;
	ies[3].code = SYN_REPORT;
	ies[3].value = 0;

	_write_may(this->ufd, ies, WAKEUP_MAX_EVENTS * sizeof(*ies));
}

static void _setup_uinput(struct _this *this)
{
	this->ufd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
	if (this->ufd < 0)
		this->ufd = open("/dev/input/uinput", O_WRONLY | O_NONBLOCK);
	if (this->ufd < 0)
		g_error("Input: Failed to open uinput: %s.", strerror(errno));

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

	struct uinput_abs_setup abs = { 0 };
	abs.absinfo.maximum = RF_POINTER_MAX;
	abs.absinfo.minimum = 0;
	abs.code = ABS_X;
	_ioctl_must(this->ufd, UI_ABS_SETUP, &abs);
	abs.code = ABS_Y;
	_ioctl_must(this->ufd, UI_ABS_SETUP, &abs);

	struct uinput_setup dev = { 0 };
	dev.id.bustype = BUS_USB;
	dev.id.vendor = 0xa3a7;
	dev.id.product = 0x0003;
	strcpy(dev.name, "reframe");
	_ioctl_must(this->ufd, UI_DEV_SETUP, &dev);
	_ioctl_must(this->ufd, UI_DEV_CREATE);

	// If screen is turned off, we cannot get CRTC, so we have to wake it up.
	this->wakeup = rf_config_get_wakeup(this->config);
	if (this->wakeup) {
		g_message(
			"Input: Waiting for 1 second to let userspace detect the uinput device before wakeup."
		);
		g_usleep(G_USEC_PER_SEC);
		_wakeup_uinput(this);
		g_message(
			"Input: Waiting for 1 second to let userspace process wakeup."
		);
		g_usleep(G_USEC_PER_SEC);
	}
}

static void _clean_uinput(struct _this *this)
{
	if (this->ufd >= 0) {
		_ioctl_may(this->ufd, UI_DEV_DESTROY);
		close(this->ufd);
		this->ufd = -1;
	}
}

int main(int argc, char *argv[])
{
	setlocale(LC_ALL, "");

	g_autofree char *config_path = NULL;
	g_autofree char *socket_path = NULL;
	// `gboolean` is `int`, but `bool` may be `char`! Passing `bool` pointer
	// to `GOptionContext` leads into overflow!
	int keep_listen = false;
	int skip_auth = false;
	int version = false;
	g_autoptr(GError) error = NULL;

	GOptionEntry options[] = {
		{ "version",
		  'v',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  &version,
		  "Display version and exit.",
		  NULL },
		{ "socket",
		  's',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_FILENAME,
		  &socket_path,
		  "Socket path to communiate.",
		  "SOCKET" },
		{ "config",
		  'c',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_FILENAME,
		  &config_path,
		  "Configuration file path.",
		  "PATH" },
		{ "keep-listen",
		  'k',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  &keep_listen,
		  "Keep listening to socket after disconnection (debug purpose).",
		  NULL },
		{ "skip-auth",
		  'A',
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  &skip_auth,
		  "Skip socket client authenticating by always OK (debug purpose).",
		  NULL },
		{ NULL,
		  0,
		  G_OPTION_FLAG_NONE,
		  G_OPTION_ARG_NONE,
		  NULL,
		  NULL,
		  NULL }
	};
	g_autoptr(GOptionContext)
		context = g_option_context_new(" - ReFrame Streamer");
	g_option_context_add_main_entries(context, options, NULL);
	if (!g_option_context_parse(context, &argc, &argv, &error)) {
		g_warning("Failed to parse options: %s.", error->message);
		g_clear_pointer(&error, g_error_free);
	}

	if (version) {
		g_print(PROJECT_VERSION "\n");
		return 0;
	}

	// We ensure the default dir, user ensure the argument dir.
	if (socket_path == NULL) {
		g_mkdir("/tmp/reframe", 0755);
		rf_set_group("/tmp/reframe");
		socket_path = g_strdup("/tmp/reframe/reframe.sock");
	}

	g_autofree struct _this *this = g_malloc0(sizeof(*this));
	g_message("Using configuration file %s.", config_path);
	this->config = rf_config_new(config_path);
	this->cfd = -1;
	this->ufd = -1;
	this->skip_auth = skip_auth;

	g_autoptr(GSocketListener) listener = g_socket_listener_new();
	g_message("Using socket %s.", socket_path);

#ifdef HAVE_LIBSYSTEMD
	if (sd_listen_fds(0) != 0) {
		// systemd socket.
		// We only handle 1 socket.
		int sfd = SD_LISTEN_FDS_START;
		g_autoptr(GSocket) socket = g_socket_new_from_fd(sfd, &error);
		if (error != NULL)
			g_error("Failed to create socket from systemd fd: %s.",
				error->message);
		g_socket_listener_add_socket(listener, socket, NULL, &error);
	} else {
#endif
		// Non-systemd socket.
		g_autoptr(GSocketAddress)
			address = g_unix_socket_address_new(socket_path);
		g_remove(socket_path);
		g_socket_listener_add_address(
			listener,
			address,
			G_SOCKET_TYPE_STREAM,
			G_SOCKET_PROTOCOL_DEFAULT,
			NULL,
			NULL,
			&error
		);
		rf_set_group(socket_path);
		g_chmod(socket_path, 0660);
#ifdef HAVE_LIBSYSTEMD
	}
#endif
	if (error != NULL)
		g_error("Failed to listen to socket: %s.", error->message);

	g_message(
		"Keep listening mode is %s.",
		keep_listen ? "enabled" : "disabled"
	);
	g_message(
		"Skip authenticating mode is %s.",
		skip_auth ? "enabled" : "disabled"
	);
	do {
		this->connection =
			g_socket_listener_accept(listener, NULL, NULL, &error);
		if (this->connection == NULL)
			g_error("Failed to accept connection: %s.",
				error->message);

		GSocket *socket =
			g_socket_connection_get_socket(this->connection);
		pid_t pid = rf_get_socket_pid(socket);
		if (_auth_pid(this, pid, BINDIR "/reframe-server") != 0) {
			g_warning("Got disallowed socket client PID %d.", pid);
			goto close;
		}

		g_message("ReFrame Server connected.");

		_setup_uinput(this);
		_setup_drm(this);

		while (true) {
			ssize_t ret = 0;
			GInputStream *is = g_io_stream_get_input_stream(
				G_IO_STREAM(this->connection)
			);
			char type;
			ret = g_input_stream_read(
				is, &type, sizeof(type), NULL, &error
			);
			if (ret <= 0) {
				if (ret < 0)
					g_warning(
						"Failed to read message type: %s.",
						error->message
					);
				break;
			}

			switch (type) {
			case RF_MSG_TYPE_FRAME:
				ret = _on_frame_msg(this);
				break;
			case RF_MSG_TYPE_INPUT:
				ret = _on_input_msg(this);
				break;
			case RF_MSG_TYPE_AUTH:
				ret = _on_auth_msg(this);
				break;
			default:
				break;
			}
			if (ret <= 0)
				break;
		}

		g_message("ReFrame Server disconnected.");

		_clean_drm(this);
		_clean_uinput(this);

	close:
		// Dropping the last reference of it will automatically close IO
		// streams and socket.
		g_clear_object(&this->connection);
	} while (keep_listen);

	g_socket_listener_close(listener);
	g_clear_object(&this->config);

	return 0;
}
