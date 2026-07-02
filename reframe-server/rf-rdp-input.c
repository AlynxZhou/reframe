#include "rf-rdp-input.h"

#define RDP_INPUT_BODY_HEADER_LENGTH 4
#define RDP_INPUT_EVENT_LENGTH 12

#define PTR_FLAGS_HWHEEL 0x0400
#define PTR_FLAGS_WHEEL 0x0200
#define PTR_FLAGS_WHEEL_NEGATIVE 0x0100
#define PTR_FLAGS_MOVE 0x0800
#define PTR_FLAGS_DOWN 0x8000
#define PTR_FLAGS_BUTTON1 0x1000
#define PTR_FLAGS_BUTTON2 0x2000
#define PTR_FLAGS_BUTTON3 0x4000
#define PTR_XFLAGS_BUTTON1 0x0001
#define PTR_XFLAGS_BUTTON2 0x0002

static uint16_t read_u16_le(const uint8_t *data)
{
	return data[0] | ((uint16_t)data[1] << 8);
}

static const uint16_t scancode_to_ev_key[128] = {
	[0x01] = 1,   [0x02] = 2,   [0x03] = 3,   [0x04] = 4,
	[0x05] = 5,   [0x06] = 6,   [0x07] = 7,   [0x08] = 8,
	[0x09] = 9,   [0x0a] = 10,  [0x0b] = 11,  [0x0c] = 12,
	[0x0d] = 13,  [0x0e] = 14,  [0x0f] = 15,  [0x10] = 16,
	[0x11] = 17,  [0x12] = 18,  [0x13] = 19,  [0x14] = 20,
	[0x15] = 21,  [0x16] = 22,  [0x17] = 23,  [0x18] = 24,
	[0x19] = 25,  [0x1a] = 26,  [0x1b] = 27,  [0x1c] = 28,
	[0x1d] = 29,  [0x1e] = 30,  [0x1f] = 31,  [0x20] = 32,
	[0x21] = 33,  [0x22] = 34,  [0x23] = 35,  [0x24] = 36,
	[0x25] = 37,  [0x26] = 38,  [0x27] = 39,  [0x28] = 40,
	[0x29] = 41,  [0x2a] = 42,  [0x2b] = 43,  [0x2c] = 44,
	[0x2d] = 45,  [0x2e] = 46,  [0x2f] = 47,  [0x30] = 48,
	[0x31] = 49,  [0x32] = 50,  [0x33] = 51,  [0x34] = 52,
	[0x35] = 53,  [0x36] = 54,  [0x37] = 55,  [0x38] = 56,
	[0x39] = 57,  [0x3a] = 58,  [0x3b] = 59,  [0x3c] = 60,
	[0x3d] = 61,  [0x3e] = 62,  [0x3f] = 63,  [0x40] = 64,
	[0x41] = 65,  [0x42] = 66,  [0x43] = 67,  [0x44] = 68,
	[0x45] = 69,  [0x46] = 70,  [0x47] = 71,  [0x48] = 72,
	[0x49] = 73,  [0x4a] = 74,  [0x4b] = 75,  [0x4c] = 76,
	[0x4d] = 77,  [0x4e] = 78,  [0x4f] = 79,  [0x50] = 80,
	[0x51] = 81,  [0x52] = 82,  [0x53] = 83,  [0x57] = 87,
	[0x58] = 88,
};

uint32_t rf_rdp_scancode_to_ev_key(uint8_t code, bool extended)
{
	if (extended) {
		switch (code) {
		case 0x1c:
			return 96;  /* KEY_KPENTER */
		case 0x1d:
			return 97;  /* KEY_RIGHTCTRL */
		case 0x35:
			return 98;  /* KEY_KPSLASH */
		case 0x38:
			return 100; /* KEY_RIGHTALT */
		case 0x47:
			return 102; /* KEY_HOME */
		case 0x48:
			return 103; /* KEY_UP */
		case 0x49:
			return 104; /* KEY_PAGEUP */
		case 0x4b:
			return 105; /* KEY_LEFT */
		case 0x4d:
			return 106; /* KEY_RIGHT */
		case 0x4f:
			return 107; /* KEY_END */
		case 0x50:
			return 108; /* KEY_DOWN */
		case 0x51:
			return 109; /* KEY_PAGEDOWN */
		case 0x52:
			return 110; /* KEY_INSERT */
		case 0x53:
			return 111; /* KEY_DELETE */
		default:
			break;
		}
	}

	if (code >= 128)
		return 0;

	return scancode_to_ev_key[code];
}

bool rf_rdp_pointer_update(
	struct rf_rdp_pointer_state *state,
	uint16_t flags,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
)
{
	if (state == 0 || width == 0 || height == 0)
		return false;

	state->wup = false;
	state->wdown = false;
	state->wleft = false;
	state->wright = false;

	if (flags & PTR_FLAGS_MOVE) {
		state->rx = (double)x / width;
		state->ry = (double)y / height;
		if (state->rx < 0.0)
			state->rx = 0.0;
		if (state->rx > 1.0)
			state->rx = 1.0;
		if (state->ry < 0.0)
			state->ry = 0.0;
		if (state->ry > 1.0)
			state->ry = 1.0;
	}

	if (flags & PTR_FLAGS_BUTTON1)
		state->left = flags & PTR_FLAGS_DOWN;
	if (flags & PTR_FLAGS_BUTTON2)
		state->right = flags & PTR_FLAGS_DOWN;
	if (flags & PTR_FLAGS_BUTTON3)
		state->middle = flags & PTR_FLAGS_DOWN;

	if (flags & PTR_XFLAGS_BUTTON1)
		state->back = flags & PTR_FLAGS_DOWN;
	if (flags & PTR_XFLAGS_BUTTON2)
		state->forward = flags & PTR_FLAGS_DOWN;

	if (flags & PTR_FLAGS_WHEEL) {
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			state->wdown = true;
		else
			state->wup = true;
	}

	if (flags & PTR_FLAGS_HWHEEL) {
		if (flags & PTR_FLAGS_WHEEL_NEGATIVE)
			state->wright = true;
		else
			state->wleft = true;
	}

	return true;
}

bool rf_rdp_input_event_count(
	const uint8_t *data,
	size_t length,
	uint16_t *count
)
{
	uint16_t events = 0;

	if (data == NULL || count == NULL || length < RDP_INPUT_BODY_HEADER_LENGTH)
		return false;

	events = read_u16_le(data);
	if ((size_t)events >
	    (length - RDP_INPUT_BODY_HEADER_LENGTH) / RDP_INPUT_EVENT_LENGTH)
		return false;

	*count = events;
	return true;
}

bool rf_rdp_input_parse_event(
	const uint8_t *data,
	size_t length,
	uint16_t index,
	struct rf_rdp_input_event *event
)
{
	uint16_t count = 0;
	size_t offset = 0;

	if (event == NULL || !rf_rdp_input_event_count(data, length, &count) ||
	    index >= count)
		return false;

	offset = RDP_INPUT_BODY_HEADER_LENGTH +
		(size_t)index * RDP_INPUT_EVENT_LENGTH;
	event->message_type = read_u16_le(data + offset + 4);
	event->flags = read_u16_le(data + offset + 6);
	event->param1 = read_u16_le(data + offset + 8);
	event->param2 = read_u16_le(data + offset + 10);
	return true;
}
