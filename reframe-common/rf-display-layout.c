#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <glib.h>

#include "rf-display-layout.h"

bool rf_desktop_layout_equal(
	const struct rf_desktop_layout *a,
	const struct rf_desktop_layout *b
)
{
	return a != NULL && b != NULL &&
	       a->desktop_width == b->desktop_width &&
	       a->desktop_height == b->desktop_height &&
	       a->monitor_x == b->monitor_x && a->monitor_y == b->monitor_y;
}

struct output_state {
	char *name;
	bool have_position;
	bool have_size;
	int x;
	int y;
	unsigned int width;
	unsigned int height;
};

static void clear_output_state(struct output_state *state)
{
	g_clear_pointer(&state->name, g_free);
	state->have_position = false;
	state->have_size = false;
	state->x = 0;
	state->y = 0;
	state->width = 0;
	state->height = 0;
}

static bool parse_output_name(const char *line, char **name)
{
	const char *close = strrchr(line, ')');
	if (close == NULL)
		return false;
	const char *open = close;
	while (open > line && *open != '(')
		--open;
	if (*open != '(' || close <= open + 1)
		return false;

	g_clear_pointer(name, g_free);
	*name = g_strndup(open + 1, close - open - 1);
	return true;
}

static bool commit_output(
	const struct output_state *state,
	const char *connector_name,
	bool *have_any,
	int64_t *min_x,
	int64_t *min_y,
	int64_t *max_x,
	int64_t *max_y,
	bool *have_target,
	int64_t *target_x,
	int64_t *target_y
)
{
	if (state->name == NULL || !state->have_position ||
	    !state->have_size || state->width == 0 || state->height == 0)
		return true;

	const int64_t left = state->x;
	const int64_t top = state->y;
	const int64_t right = left + state->width;
	const int64_t bottom = top + state->height;
	if (!*have_any) {
		*min_x = left;
		*min_y = top;
		*max_x = right;
		*max_y = bottom;
		*have_any = true;
	} else {
		*min_x = MIN(*min_x, left);
		*min_y = MIN(*min_y, top);
		*max_x = MAX(*max_x, right);
		*max_y = MAX(*max_y, bottom);
	}

	if (!*have_target &&
	    (connector_name == NULL ||
	     g_strcmp0(state->name, connector_name) == 0)) {
		*target_x = left;
		*target_y = top;
		*have_target = true;
	}
	return true;
}

static bool extract_json_string(const char *object, const char *key, char **value)
{
	g_autofree char *pattern = g_strdup_printf(
		"\"%s\"[[:space:]]*:[[:space:]]*\"([^\"\\\\]*(?:\\\\.[^\"\\\\]*)*)\"",
		key
	);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (regex == NULL || !g_regex_match(regex, object, 0, &match))
		return false;

	g_autofree char *raw = g_match_info_fetch(match, 1);
	if (raw == NULL)
		return false;

	g_clear_pointer(value, g_free);
	*value = g_strcompress(raw);
	return *value != NULL;
}

static bool extract_json_int(const char *object, const char *key, int *value)
{
	g_autofree char *pattern = g_strdup_printf(
		"\"%s\"[[:space:]]*:[[:space:]]*(-?[0-9]+)",
		key
	);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (regex == NULL || !g_regex_match(regex, object, 0, &match))
		return false;

	g_autofree char *raw = g_match_info_fetch(match, 1);
	if (raw == NULL)
		return false;

	char *end = NULL;
	const long parsed = strtol(raw, &end, 10);
	if (end == raw || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX)
		return false;

	*value = (int)parsed;
	return true;
}

static bool extract_json_double(const char *object, const char *key, double *value)
{
	g_autofree char *pattern = g_strdup_printf(
		"\"%s\"[[:space:]]*:[[:space:]]*(-?[0-9]+(?:\\.[0-9]+)?)",
		key
	);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (regex == NULL || !g_regex_match(regex, object, 0, &match))
		return false;

	g_autofree char *raw = g_match_info_fetch(match, 1);
	if (raw == NULL)
		return false;

	char *end = NULL;
	const double parsed = g_ascii_strtod(raw, &end);
	if (end == raw || *end != '\0')
		return false;

	*value = parsed;
	return true;
}

static bool extract_json_bool(const char *object, const char *key, bool *value)
{
	g_autofree char *pattern = g_strdup_printf(
		"\"%s\"[[:space:]]*:[[:space:]]*(true|false)",
		key
	);
	g_autoptr(GRegex) regex = g_regex_new(pattern, 0, 0, NULL);
	g_autoptr(GMatchInfo) match = NULL;

	if (regex == NULL || !g_regex_match(regex, object, 0, &match))
		return false;

	g_autofree char *raw = g_match_info_fetch(match, 1);
	if (raw == NULL)
		return false;

	*value = g_strcmp0(raw, "true") == 0;
	return true;
}

static bool commit_hyprland_monitor(
	const char *object,
	const char *connector_name,
	bool *have_any,
	int64_t *min_x,
	int64_t *min_y,
	int64_t *max_x,
	int64_t *max_y,
	bool *have_target,
	int64_t *target_x,
	int64_t *target_y
)
{
	g_autofree char *name = NULL;
	int width = 0;
	int height = 0;
	int x = 0;
	int y = 0;
	double scale = 1.0;
	bool disabled = false;

	if (!extract_json_string(object, "name", &name) ||
	    !extract_json_int(object, "width", &width) ||
	    !extract_json_int(object, "height", &height) ||
	    !extract_json_int(object, "x", &x) ||
	    !extract_json_int(object, "y", &y))
		return true;
	extract_json_double(object, "scale", &scale);
	extract_json_bool(object, "disabled", &disabled);

	if (disabled || width <= 0 || height <= 0 || scale <= 0.0)
		return true;

	const int64_t logical_width = (int64_t)((double)width / scale + 0.5);
	const int64_t logical_height = (int64_t)((double)height / scale + 0.5);
	if (logical_width <= 0 || logical_height <= 0)
		return true;

	const int64_t left = x;
	const int64_t top = y;
	const int64_t right = left + logical_width;
	const int64_t bottom = top + logical_height;
	if (!*have_any) {
		*min_x = left;
		*min_y = top;
		*max_x = right;
		*max_y = bottom;
		*have_any = true;
	} else {
		*min_x = MIN(*min_x, left);
		*min_y = MIN(*min_y, top);
		*max_x = MAX(*max_x, right);
		*max_y = MAX(*max_y, bottom);
	}

	if (!*have_target &&
	    (connector_name == NULL || g_strcmp0(name, connector_name) == 0)) {
		*target_x = left;
		*target_y = top;
		*have_target = true;
	}
	return true;
}

bool rf_parse_niri_outputs_layout(
	const char *outputs,
	const char *connector_name,
	struct rf_desktop_layout *layout
)
{
	if (outputs == NULL || layout == NULL)
		return false;

	g_auto(GStrv) lines = g_strsplit(outputs, "\n", -1);
	struct output_state state = { 0 };
	bool have_any = false;
	bool have_target = false;
	int64_t min_x = 0;
	int64_t min_y = 0;
	int64_t max_x = 0;
	int64_t max_y = 0;
	int64_t target_x = 0;
	int64_t target_y = 0;

	for (size_t i = 0; lines[i] != NULL; ++i) {
		char *line = g_strstrip(lines[i]);
		if (g_str_has_prefix(line, "Output ")) {
			commit_output(
				&state,
				connector_name,
				&have_any,
				&min_x,
				&min_y,
				&max_x,
				&max_y,
				&have_target,
				&target_x,
				&target_y
			);
			clear_output_state(&state);
			parse_output_name(line, &state.name);
			continue;
		}

		int x = 0;
		int y = 0;
		unsigned int width = 0;
		unsigned int height = 0;
		if (sscanf(line, "Logical position: %d, %d", &x, &y) == 2) {
			state.x = x;
			state.y = y;
			state.have_position = true;
		} else if (sscanf(line, "Logical size: %ux%u", &width, &height) == 2) {
			state.width = width;
			state.height = height;
			state.have_size = true;
		}
	}

	commit_output(
		&state,
		connector_name,
		&have_any,
		&min_x,
		&min_y,
		&max_x,
		&max_y,
		&have_target,
		&target_x,
		&target_y
	);
	clear_output_state(&state);

	const int64_t desktop_width = max_x - min_x;
	const int64_t desktop_height = max_y - min_y;
	const int64_t monitor_x = target_x - min_x;
	const int64_t monitor_y = target_y - min_y;
	if (!have_any || !have_target || desktop_width <= 0 ||
	    desktop_height <= 0 || desktop_width > UINT_MAX ||
	    desktop_height > UINT_MAX || monitor_x < INT_MIN ||
	    monitor_x > INT_MAX || monitor_y < INT_MIN || monitor_y > INT_MAX)
		return false;

	layout->desktop_width = (unsigned int)desktop_width;
	layout->desktop_height = (unsigned int)desktop_height;
	layout->monitor_x = (int)monitor_x;
	layout->monitor_y = (int)monitor_y;
	return true;
}

bool rf_parse_hyprland_monitors_layout(
	const char *monitors,
	const char *connector_name,
	struct rf_desktop_layout *layout
)
{
	if (monitors == NULL || layout == NULL)
		return false;

	bool have_any = false;
	bool have_target = false;
	int64_t min_x = 0;
	int64_t min_y = 0;
	int64_t max_x = 0;
	int64_t max_y = 0;
	int64_t target_x = 0;
	int64_t target_y = 0;
	const char *object_begin = NULL;
	unsigned int depth = 0;
	bool in_string = false;
	bool escaped = false;

	for (const char *p = monitors; *p != '\0'; ++p) {
		if (in_string) {
			if (escaped)
				escaped = false;
			else if (*p == '\\')
				escaped = true;
			else if (*p == '"')
				in_string = false;
			continue;
		}

		if (*p == '"') {
			in_string = true;
		} else if (*p == '{') {
			if (depth == 0)
				object_begin = p;
			++depth;
		} else if (*p == '}' && depth > 0) {
			--depth;
			if (depth == 0 && object_begin != NULL) {
				g_autofree char *object =
					g_strndup(object_begin, p - object_begin + 1);
				commit_hyprland_monitor(
					object,
					connector_name,
					&have_any,
					&min_x,
					&min_y,
					&max_x,
					&max_y,
					&have_target,
					&target_x,
					&target_y
				);
				object_begin = NULL;
			}
		}
	}

	const int64_t desktop_width = max_x - min_x;
	const int64_t desktop_height = max_y - min_y;
	const int64_t monitor_x = target_x - min_x;
	const int64_t monitor_y = target_y - min_y;
	if (!have_any || !have_target || desktop_width <= 0 ||
	    desktop_height <= 0 || desktop_width > UINT_MAX ||
	    desktop_height > UINT_MAX || monitor_x < INT_MIN ||
	    monitor_x > INT_MAX || monitor_y < INT_MIN || monitor_y > INT_MAX)
		return false;

	layout->desktop_width = (unsigned int)desktop_width;
	layout->desktop_height = (unsigned int)desktop_height;
	layout->monitor_x = (int)monitor_x;
	layout->monitor_y = (int)monitor_y;
	return true;
}
