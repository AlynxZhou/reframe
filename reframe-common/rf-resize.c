#include "rf-resize.h"

static double clamp_unit(double value)
{
	if (value < 0.0)
		return 0.0;
	if (value > 1.0)
		return 1.0;
	return value;
}

void rf_fit_size_to_aspect(
	unsigned int requested_width,
	unsigned int requested_height,
	double aspect_ratio,
	unsigned int *width,
	unsigned int *height
)
{
	if (width == 0 || height == 0)
		return;

	if (requested_width == 0 || requested_height == 0) {
		*width = 0;
		*height = 0;
		return;
	}

	if (aspect_ratio <= 0.0) {
		*width = requested_width;
		*height = requested_height;
		return;
	}

	if ((double)requested_width / requested_height >= aspect_ratio) {
		*width = requested_height * aspect_ratio;
		*height = requested_height;
	} else {
		*width = requested_width;
		*height = requested_width / aspect_ratio;
	}
}

void rf_fit_size_to_reference(
	unsigned int requested_width,
	unsigned int requested_height,
	unsigned int reference_width,
	unsigned int reference_height,
	unsigned int *width,
	unsigned int *height
)
{
	double aspect_ratio = 0.0;

	if (reference_width > 0 && reference_height > 0)
		aspect_ratio = (double)reference_width / reference_height;

	rf_fit_size_to_aspect(
		requested_width,
		requested_height,
		aspect_ratio,
		width,
		height
	);
}

void rf_fit_viewport_to_reference(
	unsigned int canvas_width,
	unsigned int canvas_height,
	unsigned int reference_width,
	unsigned int reference_height,
	struct rf_viewport *viewport
)
{
	unsigned int width = 0;
	unsigned int height = 0;

	if (viewport == 0)
		return;

	rf_fit_size_to_reference(
		canvas_width,
		canvas_height,
		reference_width,
		reference_height,
		&width,
		&height
	);

	viewport->x = width < canvas_width ? (canvas_width - width) / 2 : 0;
	viewport->y = height < canvas_height ? (canvas_height - height) / 2 : 0;
	viewport->w = width;
	viewport->h = height;
}

void rf_map_point_from_viewport(
	double rx,
	double ry,
	unsigned int canvas_width,
	unsigned int canvas_height,
	const struct rf_viewport *viewport,
	double *out_rx,
	double *out_ry
)
{
	if (out_rx == 0 || out_ry == 0)
		return;

	if (viewport == 0 || viewport->w == 0 || viewport->h == 0 ||
	    canvas_width == 0 || canvas_height == 0) {
		*out_rx = clamp_unit(rx);
		*out_ry = clamp_unit(ry);
		return;
	}

	const double x = rx * canvas_width;
	const double y = ry * canvas_height;
	*out_rx = clamp_unit((x - viewport->x) / viewport->w);
	*out_ry = clamp_unit((y - viewport->y) / viewport->h);
}

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
)
{
	if (abs_x == 0 || abs_y == 0 || surface_width == 0 ||
	    surface_height == 0 || abs_max == 0)
		return -1;

	const double effective_desktop_width =
		desktop_width > 0 ? desktop_width :
				    (double)monitor_x + surface_width;
	const double effective_desktop_height =
		desktop_height > 0 ? desktop_height :
				     (double)monitor_y + surface_height;
	if (effective_desktop_width <= 0.0 || effective_desktop_height <= 0.0)
		return -1;

	const double x = clamp_unit(
		((double)monitor_x + clamp_unit(rx) * surface_width) /
		effective_desktop_width
	);
	const double y = clamp_unit(
		((double)monitor_y + clamp_unit(ry) * surface_height) /
		effective_desktop_height
	);
	*abs_x = abs_max * x;
	*abs_y = abs_max * y;
	return 0;
}

void rf_apply_default_size_if_unset(
	unsigned int default_width,
	unsigned int default_height,
	unsigned int *width,
	unsigned int *height
)
{
	if (width == 0 || height == 0)
		return;

	if (*width != 0 && *height != 0)
		return;

	*width = default_width;
	*height = default_height;
}
