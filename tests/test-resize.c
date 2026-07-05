#include <assert.h>

#include "rf-resize.h"

void rf_apply_default_size_if_unset(
	unsigned int default_width,
	unsigned int default_height,
	unsigned int *width,
	unsigned int *height
);

void rf_fit_size_to_reference(
	unsigned int requested_width,
	unsigned int requested_height,
	unsigned int reference_width,
	unsigned int reference_height,
	unsigned int *width,
	unsigned int *height
);

void rf_fit_viewport_to_reference(
	unsigned int canvas_width,
	unsigned int canvas_height,
	unsigned int reference_width,
	unsigned int reference_height,
	struct rf_viewport *viewport
);

void rf_map_point_from_viewport(
	double rx,
	double ry,
	unsigned int canvas_width,
	unsigned int canvas_height,
	const struct rf_viewport *viewport,
	double *out_rx,
	double *out_ry
);

int rf_map_point_to_absolute(
	double rx,
	double ry,
	unsigned int surface_width,
	unsigned int surface_height,
	unsigned int desktop_width,
	unsigned int desktop_height,
	int monitor_x,
	int monitor_y,
	unsigned int abs_max,
	int *abs_x,
	int *abs_y
);

static void test_unknown_aspect_uses_requested_size(void)
{
	unsigned int width = 0;
	unsigned int height = 0;

	rf_fit_size_to_aspect(1024, 768, 0.0, &width, &height);
	assert(width == 1024);
	assert(height == 768);
}

static void test_known_aspect_fits_inside_requested_size(void)
{
	unsigned int width = 0;
	unsigned int height = 0;

	rf_fit_size_to_aspect(1024, 768, 16.0 / 9.0, &width, &height);
	assert(width == 1024);
	assert(height == 576);

	rf_fit_size_to_aspect(1920, 1080, 16.0 / 9.0, &width, &height);
	assert(width == 1920);
	assert(height == 1080);
}

static void test_reference_aspect_fits_inside_requested_size(void)
{
	unsigned int width = 0;
	unsigned int height = 0;

	rf_fit_size_to_reference(1668, 758, 1920, 1080, &width, &height);
	assert(width == 1347);
	assert(height == 758);
}

static void test_reference_aspect_uses_requested_size_without_reference(void)
{
	unsigned int width = 0;
	unsigned int height = 0;

	rf_fit_size_to_reference(1668, 758, 0, 0, &width, &height);
	assert(width == 1668);
	assert(height == 758);
}

static void test_reference_aspect_centers_viewport_inside_canvas(void)
{
	struct rf_viewport viewport = { 0 };

	rf_fit_viewport_to_reference(1668, 758, 1920, 1080, &viewport);
	assert(viewport.x == 160);
	assert(viewport.y == 0);
	assert(viewport.w == 1347);
	assert(viewport.h == 758);
}

static void test_viewport_point_mapping_removes_letterbox_bars(void)
{
	const struct rf_viewport viewport = { 160, 0, 1347, 758 };
	double rx = 0.0;
	double ry = 0.0;

	rf_map_point_from_viewport(
		(160.0 + 673.5) / 1668.0,
		0.5,
		1668,
		758,
		&viewport,
		&rx,
		&ry
	);
	assert(rx > 0.499 && rx < 0.501);
	assert(ry > 0.499 && ry < 0.501);

	rf_map_point_from_viewport(0.0, 0.5, 1668, 758, &viewport, &rx, &ry);
	assert(rx == 0.0);
	assert(ry > 0.499 && ry < 0.501);

	rf_map_point_from_viewport(1.0, 0.5, 1668, 758, &viewport, &rx, &ry);
	assert(rx == 1.0);
	assert(ry > 0.499 && ry < 0.501);
}

static void test_absolute_mapping_uses_remote_surface_size(void)
{
	int abs_x = 0;
	int abs_y = 0;

	assert(rf_map_point_to_absolute(
		1560.0 / 2560.0,
		28.0 / 1440.0,
		2560,
		1440,
		4266,
		1440,
		0,
		0,
		32767,
		&abs_x,
		&abs_y
	) == 0);
	assert(abs_x > 11980 && abs_x < 11990);
	assert(abs_y > 635 && abs_y < 645);
}

static void test_absolute_mapping_falls_back_to_surface_desktop(void)
{
	int abs_x = 0;
	int abs_y = 0;

	assert(rf_map_point_to_absolute(
		0.5,
		0.5,
		2560,
		1440,
		0,
		0,
		0,
		0,
		32767,
		&abs_x,
		&abs_y
	) == 0);
	assert(abs_x > 16380 && abs_x < 16390);
	assert(abs_y > 16380 && abs_y < 16390);
}

static void test_default_size_does_not_override_client_size(void)
{
	unsigned int width = 1024;
	unsigned int height = 768;

	rf_apply_default_size_if_unset(1920, 1080, &width, &height);
	assert(width == 1024);
	assert(height == 768);
}

static void test_default_size_initializes_unset_size(void)
{
	unsigned int width = 0;
	unsigned int height = 0;

	rf_apply_default_size_if_unset(1920, 1080, &width, &height);
	assert(width == 1920);
	assert(height == 1080);
}

int main(void)
{
	test_unknown_aspect_uses_requested_size();
	test_known_aspect_fits_inside_requested_size();
	test_reference_aspect_fits_inside_requested_size();
	test_reference_aspect_uses_requested_size_without_reference();
	test_reference_aspect_centers_viewport_inside_canvas();
	test_viewport_point_mapping_removes_letterbox_bars();
	test_absolute_mapping_uses_remote_surface_size();
	test_absolute_mapping_falls_back_to_surface_desktop();
	test_default_size_does_not_override_client_size();
	test_default_size_initializes_unset_size();
	return 0;
}
