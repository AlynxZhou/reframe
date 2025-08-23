#ifndef __RF_WIN_H__
#define __RF_WIN_H__

#include <gtk/gtk.h>

G_BEGIN_DECLS

#define RF_TYPE_WIN rf_win_get_type()
G_DECLARE_FINAL_TYPE(RfWin, rf_win, RF, WIN, GtkWindow)

RfWin *rf_win_new(void);
void rf_win_draw(RfWin *win, const unsigned char *buf, unsigned int width,
		 unsigned int height);

G_END_DECLS

#endif
