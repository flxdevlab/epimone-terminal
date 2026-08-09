#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

#define EPIMONE_TYPE_SETTINGS (epimone_settings_get_type())
G_DECLARE_FINAL_TYPE (EpimoneSettings, epimone_settings, EPIMONE, SETTINGS, AdwWindow)

/* The preferences window: a sidebar list of categories on the left and a
 * content pane on the right (AdwNavigationSplitView). @parent is the main
 * window it was opened from (used as transient parent; may be NULL). */
GtkWidget *epimone_settings_new (GtkWindow *parent);

G_END_DECLS
