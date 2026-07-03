#ifndef __RF_RDP_CORE_H__
#define __RF_RDP_CORE_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "rf-rdp-mcs.h"

struct rf_rect;

#define RF_RDP_SEC_EXCHANGE_PKT 0x0001u
#define RF_RDP_SEC_INFO_PKT 0x0040u
#define RF_RDP_SEC_LICENSE_PKT 0x0080u

#define RF_RDP_PDU_TYPE_DEMAND_ACTIVE 0x01u
#define RF_RDP_PDU_TYPE_CONFIRM_ACTIVE 0x03u
#define RF_RDP_PDU_TYPE_DATA 0x07u

#define RF_RDP_DATA_PDU_TYPE_UPDATE 0x02u
#define RF_RDP_DATA_PDU_TYPE_CONTROL 0x14u
#define RF_RDP_DATA_PDU_TYPE_POINTER 0x1bu
#define RF_RDP_DATA_PDU_TYPE_INPUT 0x1cu
#define RF_RDP_DATA_PDU_TYPE_SYNCHRONIZE 0x1fu
#define RF_RDP_DATA_PDU_TYPE_FONT_LIST 0x27u
#define RF_RDP_DATA_PDU_TYPE_FONT_MAP 0x28u

#define RF_RDP_UPDATE_TYPE_BITMAP 0x0001u
#define RF_RDP_BITMAP_COMPRESSION 0x0001u

#define RF_RDP_CAPSET_TYPE_SURFACE_COMMANDS 0x001cu
#define RF_RDP_CAPSET_TYPE_BITMAP_CODECS 0x001du

#define RF_RDP_SURFCMDS_SET_SURFACE_BITS 0x00000002u
#define RF_RDP_SURFCMDS_STREAM_SURFACE_BITS 0x00000040u

#define RF_RDP_CODEC_ID_NSCODEC 0x01u
#define RF_RDP_CODEC_ID_REMOTEFX 0x03u

#define RF_RDP_SURFCMD_SET_SURFACE_BITS 0x0001u
#define RF_RDP_SURFCMD_STREAM_SURFACE_BITS 0x0006u
#define RF_RDP_FASTPATH_UPDATETYPE_SURFCMDS 0x04u

#define RF_RDP_DEFAULT_SHARE_ID (0x10000u + RF_RDP_MCS_BASE_CHANNEL_ID)

#define RF_RDP_SYNC_MESSAGE_TYPE_SYNC 0x0001u
#define RF_RDP_CONTROL_ACTION_REQUEST_CONTROL 0x0001u
#define RF_RDP_CONTROL_ACTION_GRANTED_CONTROL 0x0002u
#define RF_RDP_CONTROL_ACTION_COOPERATE 0x0004u
#define RF_RDP_FONTLIST_FIRST 0x0001u
#define RF_RDP_FONTLIST_LAST 0x0002u

struct rf_rdp_core_pdu {
	size_t payload_offset;
	size_t payload_length;
	uint16_t channel_id;
	uint16_t security_flags;
	uint16_t share_type;
	uint16_t share_source;
	uint8_t data_type;
};

struct rf_rdp_core_control_pdu {
	uint16_t action;
	uint16_t grant_id;
	uint32_t control_id;
};

struct rf_rdp_core_capabilities {
	bool surface_set_bits;
	bool surface_stream_bits;
	bool nscodec;
	bool remotefx;
	uint8_t nscodec_id;
	uint8_t remotefx_id;
};

enum rf_rdp_graphics_mode {
	RF_RDP_GRAPHICS_MODE_BITMAP,
	RF_RDP_GRAPHICS_MODE_SURFACE_NSC
};

bool rf_rdp_core_parse_client_pdu(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_pdu *pdu
);
bool rf_rdp_core_parse_synchronize_body(
	const uint8_t *data,
	size_t length,
	uint16_t *target_user
);
bool rf_rdp_core_parse_control_body(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_control_pdu *control
);
bool rf_rdp_core_parse_font_list_body(
	const uint8_t *data,
	size_t length,
	uint16_t *flags
);
bool rf_rdp_core_parse_confirm_active_capabilities(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_core_capabilities *caps
);
bool rf_rdp_core_convert_rgba_rect_to_bgrx_bottom_up(
	uint8_t *dst,
	size_t dst_length,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
);
bool rf_rdp_core_convert_rgba_rect_to_bgrx_top_down(
	uint8_t *dst,
	size_t dst_length,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
);
bool rf_rdp_core_convert_rgba_rect_to_bgr_bottom_up(
	uint8_t *dst,
	size_t dst_length,
	const uint8_t *rgba,
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
);
bool rf_rdp_core_clip_update_rect(
	unsigned int frame_width,
	unsigned int frame_height,
	const struct rf_rect *damage,
	bool full_frame,
	uint16_t *x,
	uint16_t *y,
	uint16_t *width,
	uint16_t *height
);
bool rf_rdp_core_get_rgba_rect_source(
	size_t rgba_length,
	size_t rgba_stride,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	size_t *offset,
	size_t *available_length
);
bool rf_rdp_core_expand_update_rect(
	unsigned int frame_width,
	unsigned int frame_height,
	unsigned int min_width,
	unsigned int min_height,
	uint16_t *x,
	uint16_t *y,
	uint16_t *width,
	uint16_t *height
);
bool rf_rdp_core_make_full_surface_rect(
	unsigned int frame_width,
	unsigned int frame_height,
	uint16_t *x,
	uint16_t *y,
	uint16_t *width,
	uint16_t *height
);
bool rf_rdp_core_frame_should_render(
	int64_t last_frame_time_us,
	int64_t now_us,
	unsigned int max_fps,
	bool needs_full_frame
);
bool rf_rdp_core_frame_scheduler_should_render(
	int64_t *next_frame_time_us,
	int64_t now_us,
	unsigned int max_fps,
	bool needs_full_frame
);
bool rf_rdp_core_should_defer_graphics_for_rdpgfx(
	bool drdynvc_advertised,
	bool rdpgfx_disabled,
	bool rdpgfx_caps_confirmed
);
unsigned int rf_rdp_core_update_adaptive_fps(
	unsigned int current_fps,
	unsigned int configured_max_fps,
	unsigned int min_fps,
	uint64_t bytes_sent,
	int64_t interval_us,
	uint64_t target_bytes_per_second,
	uint64_t avg_send_time_us,
	bool bandwidth_limited_clients
);
bool rf_rdp_core_should_limit_fallback_fps_for_quality_state(
	bool limited_clients,
	bool quality_auto_clients,
	unsigned int current_quality_level,
	unsigned int max_quality_level
);
unsigned int rf_rdp_core_update_video_quality_level(
	unsigned int current_level,
	unsigned int max_level,
	uint64_t bytes_sent,
	int64_t interval_us,
	uint64_t target_bytes_per_second,
	unsigned int target_fps,
	uint64_t avg_send_time_us,
	uint64_t frames_sent,
	uint64_t frames_skipped,
	uint32_t max_inflight_frames,
	bool video_clients
);
unsigned int rf_rdp_core_update_video_quality_level_with_qoe(
	unsigned int current_level,
	unsigned int max_level,
	uint64_t bytes_sent,
	int64_t interval_us,
	uint64_t target_bytes_per_second,
	unsigned int target_fps,
	uint64_t avg_send_time_us,
	uint64_t frames_sent,
	uint64_t frames_skipped,
	uint32_t max_inflight_frames,
	uint16_t max_qoe_time_diff_se,
	uint16_t max_qoe_time_diff_edr,
	bool video_clients
);
bool rf_rdp_core_should_use_avc444(
	bool avc444_available,
	bool avc420_available,
	unsigned int video_quality_level
);
bool rf_rdp_core_should_skip_avc444_delta(
	unsigned int frame_width,
	unsigned int frame_height,
	unsigned int damage_width,
	unsigned int damage_height
);
bool rf_rdp_core_should_skip_avc444_delta_for_quality(
	unsigned int frame_width,
	unsigned int frame_height,
	unsigned int damage_width,
	unsigned int damage_height,
	unsigned int quality_level
);
bool rf_rdp_core_rdpgfx_avc444_lc_index(uint8_t lc, unsigned int *index);
bool rf_rdp_core_should_defer_avc444_chroma(
	uint32_t frame_id,
	unsigned int quality_level,
	bool full_frame,
	bool luma_changed,
	bool chroma_changed
);
int64_t rf_rdp_core_rdpgfx_avc_bit_rate(
	unsigned int width,
	unsigned int height,
	unsigned int fps,
	unsigned int quality_level,
	bool avc444
);
uint8_t rf_rdp_core_rdpgfx_avc_qp(
	unsigned int quality_level,
	bool avc444
);
uint8_t rf_rdp_core_rdpgfx_avc_quality(
	unsigned int quality_level,
	bool avc444
);
unsigned int rf_rdp_core_rdpgfx_avc_gop_size(
	unsigned int fps,
	unsigned int quality_level,
	bool avc444
);
unsigned int rf_rdp_core_rdpgfx_ack_limited_fps(
	unsigned int base_fps,
	uint32_t ack_queue_depth,
	bool ack_queue_depth_valid,
	uint32_t inflight_frames
);
enum rf_rdp_graphics_mode rf_rdp_core_select_graphics_mode(
	const char *requested,
	const struct rf_rdp_core_capabilities *caps,
	bool nscodec_encoder_available
);
size_t rf_rdp_core_write_license_valid_client(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id
);
size_t rf_rdp_core_write_demand_active(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint16_t width,
	uint16_t height
);
size_t rf_rdp_core_write_bitmap_update(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	const uint8_t *bgrx,
	size_t bgrx_stride
);
size_t rf_rdp_core_compress_bgr24_bitmap(
	uint8_t *dst,
	size_t dst_capacity,
	const uint8_t *bgr,
	uint16_t width,
	uint16_t height,
	size_t bgr_stride
);
size_t rf_rdp_core_write_compressed_bitmap_update(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	const uint8_t *compressed,
	size_t compressed_length,
	uint16_t scan_width,
	uint16_t uncompressed_size
);
size_t rf_rdp_core_write_surface_bits(
	uint8_t *data,
	size_t capacity,
	uint16_t command_type,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height,
	uint8_t bits_per_pixel,
	uint8_t codec_id,
	const uint8_t *bitmap_data,
	size_t bitmap_data_length
);
size_t rf_rdp_core_write_server_synchronize(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
);
size_t rf_rdp_core_write_server_control_cooperate(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
);
size_t rf_rdp_core_write_server_control_granted(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
);
size_t rf_rdp_core_write_server_font_map(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
);
size_t rf_rdp_core_write_server_default_pointer(
	uint8_t *data,
	size_t capacity,
	uint16_t user_id,
	uint32_t share_id
);

#endif
