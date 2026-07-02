#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "rf-rdp-input.h"

static void test_keyboard_scancodes(void)
{
	assert(rf_rdp_scancode_to_ev_key(0x1e, false) == 30);  /* A */
	assert(rf_rdp_scancode_to_ev_key(0x30, false) == 48);  /* B */
	assert(rf_rdp_scancode_to_ev_key(0x1c, false) == 28);  /* Enter */
	assert(rf_rdp_scancode_to_ev_key(0x1d, false) == 29);  /* Left Ctrl */
	assert(rf_rdp_scancode_to_ev_key(0x1d, true) == 97);   /* Right Ctrl */
	assert(rf_rdp_scancode_to_ev_key(0x48, true) == 103);  /* Up */
	assert(rf_rdp_scancode_to_ev_key(0x4b, true) == 105);  /* Left */
	assert(rf_rdp_scancode_to_ev_key(0xff, false) == 0);
}

static void test_pointer_buttons_and_position(void)
{
	struct rf_rdp_pointer_state state = { 0 };

	assert(rf_rdp_pointer_update(&state, 0x0800 | 0x8000 | 0x1000, 960, 540, 1920, 1080));
	assert(fabs(state.rx - 0.5) < 0.00001);
	assert(fabs(state.ry - 0.5) < 0.00001);
	assert(state.left);
	assert(!state.middle);
	assert(!state.right);

	assert(rf_rdp_pointer_update(&state, 0x1000, 960, 540, 1920, 1080));
	assert(!state.left);
}

static void test_pointer_wheel_and_extended_buttons(void)
{
	struct rf_rdp_pointer_state state = { 0 };

	assert(rf_rdp_pointer_update(&state, 0x0200 | 120, 0, 0, 100, 100));
	assert(state.wup);
	assert(!state.wdown);

	assert(rf_rdp_pointer_update(&state, 0x0200 | 0x0100 | 120, 0, 0, 100, 100));
	assert(!state.wup);
	assert(state.wdown);

	assert(rf_rdp_pointer_update(&state, 0x8000 | 0x0001, 0, 0, 100, 100));
	assert(state.back);
	assert(!state.forward);

	assert(rf_rdp_pointer_update(&state, 0x0002, 0, 0, 100, 100));
	assert(state.back);
	assert(!state.forward);
}

static void test_parse_slow_path_input_events(void)
{
	const uint8_t body[] = {
		0x02, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x04, 0x00,
		0x00, 0x01, 0x1e, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00,
		0x01, 0x80,
		0x00, 0x98, 0x80, 0x02, 0x58, 0x01
	};
	struct rf_rdp_input_event event = { 0 };
	uint16_t count = 0;

	assert(rf_rdp_input_event_count(body, sizeof(body), &count));
	assert(count == 2);
	assert(rf_rdp_input_parse_event(body, sizeof(body), 0, &event));
	assert(event.message_type == RF_RDP_INPUT_EVENT_SCANCODE);
	assert(event.flags == RF_RDP_KBD_FLAGS_EXTENDED);
	assert(event.param1 == 0x1e);
	assert(event.param2 == 0);
	assert(rf_rdp_input_parse_event(body, sizeof(body), 1, &event));
	assert(event.message_type == RF_RDP_INPUT_EVENT_MOUSE);
	assert(event.flags == (0x8000 | 0x1000 | 0x0800));
	assert(event.param1 == 640);
	assert(event.param2 == 344);
	assert(!rf_rdp_input_parse_event(body, sizeof(body), 2, &event));
	assert(!rf_rdp_input_event_count(body, 8, &count));
}

int main(void)
{
	test_keyboard_scancodes();
	test_pointer_buttons_and_position();
	test_pointer_wheel_and_extended_buttons();
	test_parse_slow_path_input_events();
	return 0;
}
