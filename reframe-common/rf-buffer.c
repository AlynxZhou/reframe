#include <glib.h>

#include "rf-common.h"
#include "rf-buffer.h"

G_DEFINE_BOXED_TYPE(RfBuffer, rf_buffer, rf_buffer_copy, rf_buffer_free)

RfBuffer *rf_buffer_copy(RfBuffer *this)
{
	return g_memdup2(this, sizeof(*this));
}

void rf_buffer_debug(RfBuffer *this)
{
	g_debug("Frame: Got buffer metadata: length %u, type %s, "
		"crtc_x %d, crtc_y %d, crtc_w %u, crtc_h %u, "
		"src_x %u, src_y %u, src_w %u, src_h %u, "
		"width %u, height %u, fourcc %c%c%c%c, modifier %#lx.",
		this->md.length,
		rf_plane_type(this->md.type),
		this->md.crtc_x,
		this->md.crtc_y,
		this->md.crtc_w,
		this->md.crtc_h,
		this->md.src_x,
		this->md.src_y,
		this->md.src_w,
		this->md.src_h,
		this->md.width,
		this->md.height,
		(this->md.fourcc >> 0) & 0xff,
		(this->md.fourcc >> 8) & 0xff,
		(this->md.fourcc >> 16) & 0xff,
		(this->md.fourcc >> 24) & 0xff,
		this->md.modifier);
	g_debug("Frame: Got buffer fds: %d %d %d %d.",
		this->fds[0],
		this->fds[1],
		this->fds[2],
		this->fds[3]);
	g_debug("Frame: Got buffer offsets: %u %u %u %u.",
		this->md.offsets[0],
		this->md.offsets[1],
		this->md.offsets[2],
		this->md.offsets[3]);
	g_debug("Frame: Got buffer pitches: %u %u %u %u.",
		this->md.pitches[0],
		this->md.pitches[1],
		this->md.pitches[2],
		this->md.pitches[3]);
}

void rf_buffer_free(RfBuffer *this)
{
	g_free(this);
}
