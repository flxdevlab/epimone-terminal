#pragma once
#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Rebindable keyboard shortcuts.
 *
 * Storage: one GSettings string key per action, named after the action,
 * holding a GTK accelerator string; the empty string means "no shortcut",
 * and the schema default is the factory binding, so resetting is
 * g_settings_reset().
 *
 * No libadwaita here: this is the model plus the GtkApplication plumbing. The
 * Keyboard page and its capture dialog live in epimone-settings.c. */

typedef struct
{
  const char *key;      /* GSettings key in org.felix.Epimone.shortcuts */
  const char *action;   /* detailed win.* action name the accel triggers */
  const char *label;    /* row title in the Keyboard page */
  const char *group;    /* Keyboard page group this row belongs to */
} EpimoneShortcut;

/* The full table, in page order. Length in @n_out. */
const EpimoneShortcut *epimone_shortcuts_table (guint *n_out);

/* Lookup by GSettings key (NULL if unknown). */
const EpimoneShortcut *epimone_shortcuts_lookup (const char *key);

/* Shared GSettings for the shortcuts schema. Borrowed, never unref'd. */
GSettings *epimone_shortcuts_get_settings (void);

/* Current and factory accelerator for @key. Caller frees. Either may be the
 * empty string, meaning the action is unbound. */
char *epimone_shortcuts_dup_accel   (const char *key);
char *epimone_shortcuts_dup_default (const char *key);

/* Accelerator text for DISPLAY, in the conventional modifier order (Ctrl,
 * Shift, Alt, Super) where gtk_accelerator_get_label puts Shift first.
 * Every user-visible accelerator goes through one of these; the stored accel
 * strings and the bindings themselves are untouched.
 *
 * format_label builds from a parsed accelerator; conventional_label reorders
 * text gtk_accelerator_get_label already produced (for labels GTK owns, like
 * GtkPopoverMenu's accel column). Caller frees. */
char *epimone_shortcuts_format_label       (guint keyval, GdkModifierType mods);
char *epimone_shortcuts_conventional_label (const char *gtk_label);

/* Rewrite, in place, every menu accelerator label under @root (labels GTK
 * renders itself and offers no API over). Menus call this once when built;
 * epimone_shortcuts_bind_application re-runs it over every window after a
 * rebind, because pushing accels makes GTK re-render those labels. */
void  epimone_shortcuts_fix_accel_labels   (GtkWidget *root);
/* Same, shaped as a signal callback: connect to a popover's "map" so lazily
 * built or re-rendered items are fixed just before each showing. */
void  epimone_shortcuts_fix_accel_labels_cb (GtkWidget *widget, gpointer user_data);

/* Store @accel (NULL or "" unbinds). Live-applies via the "changed" handler. */
void epimone_shortcuts_set_accel (const char *key, const char *accel);

void epimone_shortcuts_reset     (const char *key);
void epimone_shortcuts_reset_all (void);

/* Why an accelerator cannot be used. OK means it is safe to store. */
typedef enum
{
  EPIMONE_ACCEL_OK,
  EPIMONE_ACCEL_UNPARSABLE,   /* not an accelerator GTK can trigger on */
  EPIMONE_ACCEL_NO_MODIFIER,  /* plain key: would stop reaching the shell */
  EPIMONE_ACCEL_SHELL_KEY,    /* bare Ctrl+letter the shell owns */
  EPIMONE_ACCEL_CONFLICT      /* already bound to another action */
} EpimoneAccelVerdict;

/* Vet @accel for use by @key (which is excluded from the conflict search, so
 * re-confirming a row's existing binding is not a conflict with itself).
 *
 * On anything but OK, @message_out receives a sentence to show the user;
 * caller frees. @conflict_out, when non-NULL, receives the shortcut that
 * already owns @accel for the CONFLICT verdict (borrowed, static storage). */
EpimoneAccelVerdict epimone_shortcuts_check_accel (const char             *accel,
                                                   const char             *key,
                                                   char                  **message_out,
                                                   const EpimoneShortcut **conflict_out);

/* Push every stored accelerator onto @app, and keep doing so whenever a key
 * changes, so a rebind takes effect with no restart. Call once per process
 * with the application; later calls only re-apply. */
void epimone_shortcuts_bind_application (GtkApplication *app);

G_END_DECLS
