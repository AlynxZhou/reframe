#include <assert.h>

#include "rf-display-layout.h"

static const char single_output[] =
	"Output \"Hisense Electric Co., Ltd. 27GX-Ultra 0000000000001\" (HDMI-A-1)\n"
	"  Current mode: 5120x2880 @ 165.000 Hz\n"
	"  Logical position: 0, 0\n"
	"  Logical size: 2560x1440\n"
	"  Scale: 2\n"
	"\n"
	"Output \"Tianma Microelectronics Ltd. TL140ADMP01 Unknown\" (eDP-1)\n"
	"  Disabled\n";

static const char dual_output[] =
	"Output \"Hisense Electric Co., Ltd. 27GX-Ultra 0000000000001\" (HDMI-A-1)\n"
	"  Current mode: 5120x2880 @ 165.000 Hz\n"
	"  Logical position: 0, 0\n"
	"  Logical size: 2560x1440\n"
	"  Scale: 2\n"
	"\n"
	"Output \"Tianma Microelectronics Ltd. TL140ADMP01 Unknown\" (eDP-1)\n"
	"  Current mode: 2560x1600 @ 165.000 Hz (preferred)\n"
	"  Logical position: 2560, 0\n"
	"  Logical size: 1706x1066\n"
	"  Scale: 1.5\n";

static const char hyprland_dual_output[] =
	"[{\n"
	"    \"id\": 2,\n"
	"    \"name\": \"HDMI-A-1\",\n"
	"    \"width\": 5120,\n"
	"    \"height\": 2880,\n"
	"    \"x\": 0,\n"
	"    \"y\": 0,\n"
	"    \"scale\": 2.00,\n"
	"    \"disabled\": false\n"
	"},{\n"
	"    \"id\": 1,\n"
	"    \"name\": \"eDP-1\",\n"
	"    \"width\": 2560,\n"
	"    \"height\": 1600,\n"
	"    \"x\": 2560,\n"
	"    \"y\": 0,\n"
	"    \"scale\": 1.60,\n"
	"    \"disabled\": false\n"
	"}]";

bool rf_parse_hyprland_monitors_layout(
	const char *monitors,
	const char *connector_name,
	struct rf_desktop_layout *layout
);

static void test_single_output_layout(void)
{
	struct rf_desktop_layout layout = { 0 };

	assert(rf_parse_niri_outputs_layout(
		single_output,
		"HDMI-A-1",
		&layout
	));
	assert(layout.desktop_width == 2560);
	assert(layout.desktop_height == 1440);
	assert(layout.monitor_x == 0);
	assert(layout.monitor_y == 0);
}

static void test_dual_output_layout_for_left_monitor(void)
{
	struct rf_desktop_layout layout = { 0 };

	assert(rf_parse_niri_outputs_layout(
		dual_output,
		"HDMI-A-1",
		&layout
	));
	assert(layout.desktop_width == 4266);
	assert(layout.desktop_height == 1440);
	assert(layout.monitor_x == 0);
	assert(layout.monitor_y == 0);
}

static void test_dual_output_layout_for_right_monitor(void)
{
	struct rf_desktop_layout layout = { 0 };

	assert(rf_parse_niri_outputs_layout(dual_output, "eDP-1", &layout));
	assert(layout.desktop_width == 4266);
	assert(layout.desktop_height == 1440);
	assert(layout.monitor_x == 2560);
	assert(layout.monitor_y == 0);
}

static void test_disabled_output_is_ignored(void)
{
	struct rf_desktop_layout layout = { 0 };

	assert(!rf_parse_niri_outputs_layout(single_output, "eDP-1", &layout));
}

static void test_hyprland_dual_output_layout_for_left_monitor(void)
{
	struct rf_desktop_layout layout = { 0 };

	assert(rf_parse_hyprland_monitors_layout(
		hyprland_dual_output,
		"HDMI-A-1",
		&layout
	));
	assert(layout.desktop_width == 4160);
	assert(layout.desktop_height == 1440);
	assert(layout.monitor_x == 0);
	assert(layout.monitor_y == 0);
}

static void test_hyprland_dual_output_layout_for_right_monitor(void)
{
	struct rf_desktop_layout layout = { 0 };

	assert(rf_parse_hyprland_monitors_layout(
		hyprland_dual_output,
		"eDP-1",
		&layout
	));
	assert(layout.desktop_width == 4160);
	assert(layout.desktop_height == 1440);
	assert(layout.monitor_x == 2560);
	assert(layout.monitor_y == 0);
}

static void test_layout_equality_detects_geometry_changes(void)
{
	const struct rf_desktop_layout dual = {
		.desktop_width = 4266,
		.desktop_height = 1440,
		.monitor_x = 0,
		.monitor_y = 0,
	};
	const struct rf_desktop_layout single = {
		.desktop_width = 2560,
		.desktop_height = 1440,
		.monitor_x = 0,
		.monitor_y = 0,
	};

	assert(rf_desktop_layout_equal(&dual, &dual));
	assert(!rf_desktop_layout_equal(&dual, &single));
	assert(!rf_desktop_layout_equal(&dual, NULL));
}

int main(void)
{
	test_single_output_layout();
	test_dual_output_layout_for_left_monitor();
	test_dual_output_layout_for_right_monitor();
	test_disabled_output_is_ignored();
	test_hyprland_dual_output_layout_for_left_monitor();
	test_hyprland_dual_output_layout_for_right_monitor();
	test_layout_equality_detects_geometry_changes();
	return 0;
}
