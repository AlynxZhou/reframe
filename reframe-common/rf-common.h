#ifndef __RF_COMMON_H__
#define __RF_COMMON_H__

#include <stdint.h>
#include <stdbool.h>
#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

#define RF_BYTES_PER_PIXEL 4

/**
 * Socket IPC between Streamer and Server follows header and payload format:
 *
 * 1. Message type, which is 1 char.
 * 2. Payload length, which is 1 size_t.
 * 3. Payload.
 *
 * Some messages does not have payload, then length should be 0 and cannot be
 * omitted. If payload is a string, it should contain the `\0`.
 *
 * The payload length is the number of elements. For frame type, 0 is always the
 * primary plane, follows with an optional cursor plane. The payload length could
 * be 0 for frame type, which means the monitor is currently empty.
 */
#define RF_MSG_TYPE_FRAME 'F'
#define RF_MSG_TYPE_INPUT 'I'
#define RF_MSG_TYPE_CARD_PATH 'P'
#define RF_MSG_TYPE_CONNECTOR_NAME 'N'
#define RF_MSG_TYPE_CLIPBOARD_TEXT 'T'
#define RF_MSG_TYPE_AUTH 'A'

#define RF_KEYBOARD_MAX 256
#define RF_POINTER_MAX INT16_MAX

#define RF_MAX_BUFS 2
#define RF_MAX_FDS 4

#define RF_KEY_CODE_XKB_TO_EV(key_code) ((key_code) - 8)

struct rf_buffer_metadata {
	unsigned int length;
	// DRM plane type.
	uint32_t type;
	// See <https://events.static.linuxfound.org/sites/events/files/slides/brezillon-drm-kms.pdf>.
	int32_t crtc_x;
	int32_t crtc_y;
	uint32_t crtc_w;
	uint32_t crtc_h;
	uint32_t src_x;
	uint32_t src_y;
	uint32_t src_w;
	uint32_t src_h;
	uint32_t width;
	uint32_t height;
	uint32_t fourcc;
	uint64_t modifier;
	uint32_t offsets[RF_MAX_FDS];
	uint32_t pitches[RF_MAX_FDS];
};
struct rf_buffer {
	int fds[RF_MAX_FDS];
	struct rf_buffer_metadata md;
};

struct rf_rect {
	int x;
	int y;
	unsigned int w;
	unsigned int h;
};

struct rf_auth {
	pid_t pid;
	bool ok;
};

void rf_buffer_debug(struct rf_buffer *b);
ssize_t rf_send_header(
	GSocketConnection *connection,
	char type,
	size_t length,
	GError **error
);
const char *rf_plane_type(uint32_t type);
int rf_set_group(const char *path);
pid_t rf_get_socket_pid(GSocket *socket);

G_END_DECLS

#endif
