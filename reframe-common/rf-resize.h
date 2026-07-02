#ifndef __RF_RESIZE_H__
#define __RF_RESIZE_H__

struct rf_viewport {
	unsigned int x;
	unsigned int y;
	unsigned int w;
	unsigned int h;
};

void rf_fit_size_to_aspect(
	unsigned int requested_width,
	unsigned int requested_height,
	double aspect_ratio,
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

void rf_apply_default_size_if_unset(
	unsigned int default_width,
	unsigned int default_height,
	unsigned int *width,
	unsigned int *height
);

#endif
