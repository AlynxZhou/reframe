#ifndef __RF_RDP_INPUT_H__
#define __RF_RDP_INPUT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RF_RDP_KBD_FLAGS_EXTENDED 0x0100u
#define RF_RDP_KBD_FLAGS_RELEASE 0x8000u

#define RF_RDP_INPUT_EVENT_SCANCODE 0x0004u
#define RF_RDP_INPUT_EVENT_MOUSE 0x8001u
#define RF_RDP_INPUT_EVENT_MOUSEX 0x8002u

struct rf_rdp_pointer_state {
	double rx;
	double ry;
	bool left;
	bool middle;
	bool right;
	bool back;
	bool forward;
	bool wup;
	bool wdown;
	bool wleft;
	bool wright;
};

struct rf_rdp_input_event {
	uint16_t message_type;
	uint16_t flags;
	uint16_t param1;
	uint16_t param2;
};

uint32_t rf_rdp_scancode_to_ev_key(uint8_t code, bool extended);
bool rf_rdp_pointer_update(
	struct rf_rdp_pointer_state *state,
	uint16_t flags,
	uint16_t x,
	uint16_t y,
	uint16_t width,
	uint16_t height
);
bool rf_rdp_input_event_count(
	const uint8_t *data,
	size_t length,
	uint16_t *count
);
bool rf_rdp_input_parse_event(
	const uint8_t *data,
	size_t length,
	uint16_t index,
	struct rf_rdp_input_event *event
);

#endif
