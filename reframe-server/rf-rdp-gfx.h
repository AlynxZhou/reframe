#ifndef __RF_RDP_GFX_H__
#define __RF_RDP_GFX_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_GFX_HEADER_SIZE 8u

#define RF_RDP_GFX_CMDID_WIRETOSURFACE_1 0x0001u
#define RF_RDP_GFX_CMDID_WIRETOSURFACE_2 0x0002u
#define RF_RDP_GFX_CMDID_CREATESURFACE 0x0009u
#define RF_RDP_GFX_CMDID_STARTFRAME 0x000bu
#define RF_RDP_GFX_CMDID_ENDFRAME 0x000cu
#define RF_RDP_GFX_CMDID_FRAMEACKNOWLEDGE 0x000du
#define RF_RDP_GFX_CMDID_RESETGRAPHICS 0x000eu
#define RF_RDP_GFX_CMDID_MAPSURFACETOOUTPUT 0x000fu
#define RF_RDP_GFX_CMDID_CAPSADVERTISE 0x0012u
#define RF_RDP_GFX_CMDID_CAPSCONFIRM 0x0013u
#define RF_RDP_GFX_CMDID_QOEFRAMEACKNOWLEDGE 0x0016u

#define RF_RDP_GFX_CAPVERSION_8 0x00080004u
#define RF_RDP_GFX_CAPVERSION_81 0x00080105u
#define RF_RDP_GFX_CAPVERSION_FRDP_1 0x00010000u
#define RF_RDP_GFX_CAPVERSION_10 0x000a0002u
#define RF_RDP_GFX_CAPVERSION_101 0x000a0100u
#define RF_RDP_GFX_CAPVERSION_102 0x000a0200u
#define RF_RDP_GFX_CAPVERSION_103 0x000a0301u
#define RF_RDP_GFX_CAPVERSION_104 0x000a0400u
#define RF_RDP_GFX_CAPVERSION_105 0x000a0502u
#define RF_RDP_GFX_CAPVERSION_106 0x000a0600u
#define RF_RDP_GFX_CAPVERSION_106_ERR 0x000a0601u
#define RF_RDP_GFX_CAPVERSION_107 0x000a0701u

#define RF_RDP_GFX_CAPS_FLAG_AVC420_ENABLED 0x00000010u
#define RF_RDP_GFX_CAPS_FLAG_AVC_DISABLED 0x00000020u
#define RF_RDP_GFX_CAPS_FLAG_AVC_THINCLIENT 0x00000040u
#define RF_RDP_GFX_CAPS_FLAG_SCALEDMAP_DISABLE 0x00000080u
#define RF_RDP_GFX_CAPS_FLAG_AV1_I444_SUPPORTED 0x10000000u
#define RF_RDP_GFX_CAPS_FLAG_AV1_I444_DISABLED 0x20000000u

#define RF_RDP_GFX_PIXEL_FORMAT_XRGB_8888 0x20u
#define RF_RDP_GFX_PIXEL_FORMAT_ARGB_8888 0x21u

#define RF_RDP_GFX_CODECID_UNCOMPRESSED 0x0000u
#define RF_RDP_GFX_CODECID_AV1 0x0001u
#define RF_RDP_GFX_CODECID_CAVIDEO 0x0003u
#define RF_RDP_GFX_CODECID_CAPROGRESSIVE 0x0009u
#define RF_RDP_GFX_CODECID_PLANAR 0x000au
#define RF_RDP_GFX_CODECID_AVC420 0x000bu
#define RF_RDP_GFX_CODECID_CAPROGRESSIVE_V2 0x000du
#define RF_RDP_GFX_CODECID_AVC444 0x000eu
#define RF_RDP_GFX_CODECID_AVC444V2 0x000fu

#define RF_RDP_GFX_AVC444_LC_BOTH 0u
#define RF_RDP_GFX_AVC444_LC_SINGLE 1u
#define RF_RDP_GFX_AVC444_LC_CHROMA 2u

#define RF_RDP_GFX_ZGFX_SEGMENTED_SINGLE 0xe0u
#define RF_RDP_GFX_ZGFX_SEGMENTED_MULTIPART 0xe1u
#define RF_RDP_GFX_ZGFX_PACKET_COMPR_TYPE_RDP8 0x04u
#define RF_RDP_GFX_ZGFX_PACKET_COMPRESSED 0x20u
#define RF_RDP_GFX_ZGFX_SEGMENTED_MAXSIZE 65535u

struct rf_rdp_gfx_caps {
	uint16_t count;
	uint32_t selected_version;
	uint32_t selected_flags;
	uint32_t av1_flags;
	bool avc420;
	bool avc444;
	bool avc444_v2;
	bool progressive;
	bool progressive_v2;
	bool remotefx;
	bool planar;
	bool av1;
	bool av1_i444;
};

struct rf_rdp_gfx_server_codecs {
	bool avc420;
	bool avc444;
	bool progressive;
	bool remotefx;
	bool planar;
	bool av1;
};

enum rf_rdp_gfx_codec {
	RF_RDP_GFX_CODEC_UNCOMPRESSED,
	RF_RDP_GFX_CODEC_PLANAR,
	RF_RDP_GFX_CODEC_REMOTEFX,
	RF_RDP_GFX_CODEC_PROGRESSIVE,
	RF_RDP_GFX_CODEC_PROGRESSIVE_V2,
	RF_RDP_GFX_CODEC_AVC420,
	RF_RDP_GFX_CODEC_AVC444,
	RF_RDP_GFX_CODEC_AVC444_V2,
	RF_RDP_GFX_CODEC_AV1
};

struct rf_rdp_gfx_frame_ack {
	uint32_t queue_depth;
	uint32_t frame_id;
	uint32_t total_frames_decoded;
};

struct rf_rdp_gfx_qoe_frame_ack {
	uint32_t frame_id;
	uint32_t timestamp;
	uint16_t time_diff_se;
	uint16_t time_diff_edr;
};

bool rf_rdp_gfx_parse_caps_advertise(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_gfx_caps *caps
);
enum rf_rdp_gfx_codec rf_rdp_gfx_select_codec(
	const struct rf_rdp_gfx_caps *caps,
	const struct rf_rdp_gfx_server_codecs *server,
	bool prefer_avc444
);
enum rf_rdp_gfx_codec rf_rdp_gfx_select_codec_policy(
	const struct rf_rdp_gfx_caps *caps,
	const struct rf_rdp_gfx_server_codecs *server,
	bool under_pressure
);
const char *rf_rdp_gfx_codec_name(enum rf_rdp_gfx_codec codec);
bool rf_rdp_gfx_codec_is_video(enum rf_rdp_gfx_codec codec);
bool rf_rdp_gfx_codec_payload_allows_zgfx(uint16_t codec_id);
bool rf_rdp_gfx_parse_frame_acknowledge(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_gfx_frame_ack *ack
);
bool rf_rdp_gfx_parse_qoe_frame_acknowledge(
	const uint8_t *data,
	size_t length,
	struct rf_rdp_gfx_qoe_frame_ack *ack
);
size_t rf_rdp_gfx_write_caps_confirm(
	uint8_t *data,
	size_t capacity,
	uint32_t version,
	uint32_t flags
);
size_t rf_rdp_gfx_write_create_surface(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint16_t width,
	uint16_t height,
	uint8_t pixel_format
);
size_t rf_rdp_gfx_write_map_surface_to_output(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint32_t output_origin_x,
	uint32_t output_origin_y
);
size_t rf_rdp_gfx_write_reset_graphics(
	uint8_t *data,
	size_t capacity,
	uint32_t width,
	uint32_t height
);
size_t rf_rdp_gfx_write_start_frame(
	uint8_t *data,
	size_t capacity,
	uint32_t frame_id,
	uint32_t timestamp
);
size_t rf_rdp_gfx_write_end_frame(
	uint8_t *data,
	size_t capacity,
	uint32_t frame_id
);
size_t rf_rdp_gfx_write_wire_to_surface_1(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint16_t codec_id,
	uint8_t pixel_format,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	const uint8_t *bitmap_data,
	size_t bitmap_data_length
);
size_t rf_rdp_gfx_write_wire_to_surface_2(
	uint8_t *data,
	size_t capacity,
	uint16_t surface_id,
	uint16_t codec_id,
	uint32_t codec_context_id,
	uint8_t pixel_format,
	const uint8_t *bitmap_data,
	size_t bitmap_data_length
);
size_t rf_rdp_gfx_write_avc420_bitmap_stream(
	uint8_t *data,
	size_t capacity,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	uint8_t qp,
	uint8_t quality,
	const uint8_t *h264_data,
	size_t h264_data_length
);
size_t rf_rdp_gfx_write_avc444_bitmap_stream(
	uint8_t *data,
	size_t capacity,
	uint8_t lc,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	uint8_t qp,
	uint8_t quality,
	const uint8_t *h264_data,
	size_t h264_data_length
);
size_t rf_rdp_gfx_write_avc444_bitmap_stream_pair(
	uint8_t *data,
	size_t capacity,
	uint16_t left,
	uint16_t top,
	uint16_t right,
	uint16_t bottom,
	uint8_t qp,
	uint8_t quality,
	const uint8_t *first_h264_data,
	size_t first_h264_data_length,
	const uint8_t *second_h264_data,
	size_t second_h264_data_length
);
size_t rf_rdp_gfx_write_zgfx_uncompressed(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length
);
size_t rf_rdp_gfx_write_zgfx(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length,
	bool *compressed
);
size_t rf_rdp_gfx_write_zgfx_payload(
	uint8_t *data,
	size_t capacity,
	const uint8_t *payload,
	size_t payload_length,
	bool allow_compression,
	bool *compressed
);

#endif
