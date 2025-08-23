#include <epoxy/egl.h>
#include <epoxy/gl.h>

#include "rf-common.h"
#include "rf-win.h"

struct _RfWin {
	GtkWindow parent_instance;
	GtkDrawingArea *area;
	unsigned char *buf;
	int width;
	int height;
};
G_DEFINE_TYPE(RfWin, rf_win, GTK_TYPE_WINDOW)

static void _draw(GtkDrawingArea *area, cairo_t *cr, int width, int height,
		  gpointer data)
{
	RfWin *this = data;
	int stride =
		cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, this->width);
	g_debug("Cairo image surface stride: %d.", stride);
	cairo_surface_t *s = cairo_image_surface_create_for_data(
		this->buf, CAIRO_FORMAT_ARGB32, this->width, this->height,
		stride);
	g_debug("Cairo image surface status: %d.", cairo_surface_status(s));
	cairo_set_source_surface(cr, s, 0, 0);
	cairo_paint(cr);
	cairo_surface_destroy(s);
}

static void _finalize(GObject *object)
{
	RfWin *this = RF_WIN(object);

	g_clear_pointer(&this->buf, g_free);

	G_OBJECT_CLASS(rf_win_parent_class)->finalize(object);
}

static void rf_win_init(RfWin *this)
{
	this->buf = NULL;
	this->area = GTK_DRAWING_AREA(gtk_drawing_area_new());
	gtk_drawing_area_set_draw_func(this->area, _draw, this, NULL);
	gtk_drawing_area_set_content_width(this->area, RF_DEFAULT_WIDTH);
	gtk_drawing_area_set_content_height(this->area, RF_DEFAULT_HEIGHT);
	gtk_window_set_child(GTK_WINDOW(this), GTK_WIDGET(this->area));
}

static void rf_win_class_init(RfWinClass *klass)
{
	GObjectClass *o_class = G_OBJECT_CLASS(klass);

	o_class->finalize = _finalize;
}

RfWin *rf_win_new(void)
{
	return g_object_new(RF_TYPE_WIN, NULL);
}

void rf_win_draw(RfWin *this, const unsigned char *buf, unsigned int width,
		 unsigned int height)
{
	this->width = width;
	this->height = height;
	gtk_drawing_area_set_content_width(this->area, width);
	gtk_drawing_area_set_content_height(this->area, height);
	g_clear_pointer(&this->buf, g_free);
	this->buf = g_memdup2(buf, width * height * RF_BYTES_PER_PIXEL);
	// OpenGL ES only supports RGBA, while cairo only supports BGRA.
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			int index = (y * width + x) * RF_BYTES_PER_PIXEL;
			int temp = this->buf[index];
			this->buf[index] = this->buf[index + 2];
			this->buf[index + 2] = temp;
		}
	}
	gtk_widget_queue_draw(GTK_WIDGET(this->area));
}
