#ifndef __RF_DISPLAY_LAYOUT_H__
#define __RF_DISPLAY_LAYOUT_H__

#include <stdbool.h>

#include "rf-common.h"

bool rf_parse_niri_outputs_layout(
	const char *outputs,
	const char *connector_name,
	struct rf_desktop_layout *layout
);

bool rf_parse_hyprland_monitors_layout(
	const char *monitors,
	const char *connector_name,
	struct rf_desktop_layout *layout
);

bool rf_desktop_layout_equal(
	const struct rf_desktop_layout *a,
	const struct rf_desktop_layout *b
);

#endif
