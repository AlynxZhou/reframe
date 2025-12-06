#ifndef __RF_BUFFER_H__
#define __RF_BUFFER_H__

#include <stdint.h>
#include <glib-object.h>

G_BEGIN_DECLS

#define RF_MAX_BUFS 2
#define RF_MAX_FDS 4

struct rf_buffer_metadata {
	unsigned int length;
	// DRM plane type.
	uint32_t type;
	int64_t crtc_x;
	int64_t crtc_y;
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

#define RF_TYPE_BUFFER rf_buffer_get_type()
typedef struct rf_buffer RfBuffer;
GType rf_buffer_get_type(void);

RfBuffer *rf_buffer_copy(RfBuffer *this);
void rf_buffer_debug(RfBuffer *this);
void rf_buffer_free(RfBuffer *this);

G_END_DECLS

#endif
