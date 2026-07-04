#ifndef __RF_RDP_CLIPBOARD_PROVIDER_H__
#define __RF_RDP_CLIPBOARD_PROVIDER_H__

#include <gdk/gdk.h>

#include "rf-clipboard-rich.h"

G_BEGIN_DECLS

GdkContentProvider *rf_rdp_clipboard_content_provider_new(
	const struct rf_clipboard_rich_payload *payload
);

G_END_DECLS

#endif
