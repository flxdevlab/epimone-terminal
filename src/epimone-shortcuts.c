#include "epimone-shortcuts.h"

#include <gdk/gdkkeysyms.h>

#define EPIMONE_SHORTCUTS_SCHEMA "org.felix.Epimone.shortcuts"

/* Every accelerator Epimone binds, and the win.* action it drives.
 *
 * These stay WINDOW-level actions on purpose. GtkApplication accelerators are
 * dispatched from the window, whose action muxer reaches window and app actions
 * but not widget-class actions installed on a descendant; accelerators bound
 * to descendant-installed actions silently do nothing (see epimone_page_copy).
 * The action strings here are detailed names, so the directional ones carry
 * their target ("win.focus::left") exactly as
 * gtk_application_set_accels_for_action wants.
 *
 * The `key` column is also the GSettings key, and the schema default for that
 * key is the accelerator listed in the schema; the two must stay in step. */
static const EpimoneShortcut epimone_shortcut_table[] = {
  { "new-tab",     "win.new-tab",      "New Tab",          "Tabs & Windows" },
  { "new-window",  "win.new-window",   "New Window",       "Tabs & Windows" },
  { "set-title",   "win.set-title",    "Set Title",        "Tabs & Windows" },
  { "preferences", "win.preferences",  "Preferences",      "Tabs & Windows" },
  { "overview",    "win.overview",     "All Sessions",     "Tabs & Windows" },
  { "tab-move-left",  "win.tab-move-left",  "Move Tab Left",  "Tabs & Windows" },
  { "tab-move-right", "win.tab-move-right", "Move Tab Right", "Tabs & Windows" },

  { "split-right", "win.split-right",  "Split Right",      "Panes" },
  { "split-down",  "win.split-down",   "Split Down",       "Panes" },
  /* Label says "Detach" (it only detaches; see the pane menu's note in
   * epimone-page.c); the key and action keep their close-pane spelling so
   * stored bindings stay valid. */
  { "close-pane",  "win.close-pane",   "Detach Pane",      "Panes" },
  { "kill-pane",   "win.kill-pane",    "Kill Pane",        "Panes" },
  { "zoom",        "win.zoom",         "Zoom Pane",        "Panes" },
  { "open-in-new-tab", "win.open-in-new-tab", "Open in New Tab", "Panes" },
  { "read-only",   "win.read-only",    "Read-Only",        "Panes" },
  { "reset",       "win.reset",        "Reset",            "Panes" },
  { "reset-and-clear", "win.reset-and-clear", "Reset and Clear", "Panes" },
  { "focus-left",  "win.focus::left",  "Focus Pane Left",  "Panes" },
  { "focus-right", "win.focus::right", "Focus Pane Right", "Panes" },
  { "focus-up",    "win.focus::up",    "Focus Pane Up",    "Panes" },
  { "focus-down",  "win.focus::down",  "Focus Pane Down",  "Panes" },

  { "copy",        "win.copy",         "Copy",             "Clipboard" },
  { "paste",       "win.paste",        "Paste",            "Clipboard" },
  { "select-all",  "win.select-all",   "Select All",       "Clipboard" },
  { "select-none", "win.select-none",  "Select None",      "Clipboard" },
};

/* Bare Ctrl+<letter> combinations the shell and readline own. Binding any of
 * these from a preferences dialog would take away SIGINT, EOF, suspend, clear,
 * line editing or reverse-search inside every terminal, so they are refused
 * outright rather than merely warned about. */
static const guint epimone_reserved_ctrl_keys[] = {
  GDK_KEY_c,   /* SIGINT */
  GDK_KEY_d,   /* EOF */
  GDK_KEY_z,   /* SIGTSTP */
  GDK_KEY_l,   /* clear */
  GDK_KEY_a,   /* beginning of line */
  GDK_KEY_e,   /* end of line */
  GDK_KEY_k,   /* kill to end of line */
  GDK_KEY_u,   /* kill line */
  GDK_KEY_w,   /* kill word */
  GDK_KEY_r,   /* reverse search */
};

/* ------------------------------------------------------------------ *
 * Conventional accelerator labels
 *
 * gtk_accelerator_get_label emits modifiers in GTK's own canonical order,
 * Shift before Ctrl, so "<Control><Shift>t" renders as "Shift+Ctrl+T". That
 * matches almost nothing else the user runs: GNOME Settings, VS Code and the
 * rest all write "Ctrl+Shift+T". (Verified against GTK 4.22; libadwaita
 * 1.9's AdwShortcutLabel renders Shift-first too, so there is no upstream
 * helper to reuse.)
 *
 * The fix is a text transform on GTK's own label rather than a from-scratch
 * formatter: the modifier prefix is re-emitted in the conventional order
 * (Ctrl, Shift, Alt, Super) and everything else, the key part in particular
 * ("T", ",", "Page Up"), stays exactly as GTK printed it. Under a locale
 * that translates the modifier words the prefixes stop matching and the
 * label passes through in GTK's order unchanged; a safe fallback, and this
 * application is not localized.
 * ------------------------------------------------------------------ */

/* Conventional emit order. Meta/Hyper close the list for completeness; GTK
 * names them last as well. */
static const char * const epimone_modifier_names[] = {
  "Ctrl", "Shift", "Alt", "Super", "Meta", "Hyper",
};

char *
epimone_shortcuts_conventional_label (const char *gtk_label)
{
  gboolean have[G_N_ELEMENTS (epimone_modifier_names)] = { FALSE };
  const char *rest = gtk_label;
  GString *out;

  if (gtk_label == NULL)
    return NULL;

  /* Strip the modifier prefix, whatever order GTK used. A token only counts
   * when a non-empty remainder follows its "+", so a trailing key of "+"
   * (the plus key itself) is never eaten as a separator. */
  for (;;)
    {
      gboolean matched = FALSE;

      for (guint i = 0; i < G_N_ELEMENTS (epimone_modifier_names); i++)
        {
          gsize len = strlen (epimone_modifier_names[i]);

          if (g_str_has_prefix (rest, epimone_modifier_names[i]) &&
              rest[len] == '+' && rest[len + 1] != '\0')
            {
              have[i] = TRUE;
              rest += len + 1;
              matched = TRUE;
              break;
            }
        }
      if (!matched)
        break;
    }

  out = g_string_new (NULL);
  for (guint i = 0; i < G_N_ELEMENTS (epimone_modifier_names); i++)
    if (have[i])
      g_string_append_printf (out, "%s+", epimone_modifier_names[i]);
  g_string_append (out, rest);
  return g_string_free (out, FALSE);
}

char *
epimone_shortcuts_format_label (guint keyval, GdkModifierType mods)
{
  g_autofree char *gtk_label = gtk_accelerator_get_label (keyval, mods);

  return epimone_shortcuts_conventional_label (gtk_label);
}

/* Signal-callback form, for connecting to a popover's "map": items may be
 * built lazily (GtkMenuButton) and are re-rendered whenever their actions
 * resolve or the application's accels are re-applied, so map time (after all
 * of that, before the first visible frame) is the one reliable moment. */
void
epimone_shortcuts_fix_accel_labels_cb (GtkWidget *widget, gpointer user_data)
{
  (void) user_data;
  epimone_shortcuts_fix_accel_labels (widget);
}

/* Rewrite every accelerator label under @root into the conventional order.
 *
 * This is for text GTK owns: GtkPopoverMenu renders an item's accelerator
 * (from the "accel" attribute, or auto-discovered from the application's
 * accels when the item has none, as with the hamburger menu's items) through
 * gtk_accelerator_get_label, with no API to influence the result. The label
 * is identifiable by its "accelerator" CSS node name, so it is edited in
 * place. Idempotent: a label already in conventional order reorders to
 * itself. */
void
epimone_shortcuts_fix_accel_labels (GtkWidget *root)
{
  if (root == NULL)
    return;

  if (GTK_IS_LABEL (root) &&
      g_strcmp0 (gtk_widget_get_css_name (root), "accelerator") == 0)
    {
      g_autofree char *fixed =
        epimone_shortcuts_conventional_label (gtk_label_get_text (GTK_LABEL (root)));

      if (fixed != NULL)
        gtk_label_set_text (GTK_LABEL (root), fixed);
    }

  for (GtkWidget *child = gtk_widget_get_first_child (root);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    epimone_shortcuts_fix_accel_labels (child);
}

const EpimoneShortcut *
epimone_shortcuts_table (guint *n_out)
{
  if (n_out != NULL)
    *n_out = G_N_ELEMENTS (epimone_shortcut_table);
  return epimone_shortcut_table;
}

const EpimoneShortcut *
epimone_shortcuts_lookup (const char *key)
{
  if (key == NULL)
    return NULL;

  for (guint i = 0; i < G_N_ELEMENTS (epimone_shortcut_table); i++)
    {
      if (g_strcmp0 (epimone_shortcut_table[i].key, key) == 0)
        return &epimone_shortcut_table[i];
    }

  return NULL;
}

GSettings *
epimone_shortcuts_get_settings (void)
{
  static GSettings *settings;

  if (settings == NULL)
    settings = g_settings_new (EPIMONE_SHORTCUTS_SCHEMA);

  return settings;
}

char *
epimone_shortcuts_dup_accel (const char *key)
{
  g_return_val_if_fail (epimone_shortcuts_lookup (key) != NULL, g_strdup (""));

  return g_settings_get_string (epimone_shortcuts_get_settings (), key);
}

/* The factory binding, read from the schema rather than from a second copy of
 * the table, so the schema stays the single source of truth for defaults. */
char *
epimone_shortcuts_dup_default (const char *key)
{
  g_autoptr (GSettingsSchemaSource) source = NULL;
  g_autoptr (GSettingsSchema) schema = NULL;
  g_autoptr (GSettingsSchemaKey) schema_key = NULL;
  g_autoptr (GVariant) value = NULL;

  g_return_val_if_fail (epimone_shortcuts_lookup (key) != NULL, g_strdup (""));

  source = g_settings_schema_source_ref (g_settings_schema_source_get_default ());
  schema = g_settings_schema_source_lookup (source, EPIMONE_SHORTCUTS_SCHEMA, TRUE);
  if (schema == NULL)
    return g_strdup ("");

  schema_key = g_settings_schema_get_key (schema, key);
  if (schema_key == NULL)
    return g_strdup ("");

  value = g_settings_schema_key_get_default_value (schema_key);
  if (value == NULL)
    return g_strdup ("");

  return g_variant_dup_string (value, NULL);
}

void
epimone_shortcuts_set_accel (const char *key, const char *accel)
{
  g_return_if_fail (epimone_shortcuts_lookup (key) != NULL);

  g_settings_set_string (epimone_shortcuts_get_settings (), key,
                         (accel != NULL) ? accel : "");
}

void
epimone_shortcuts_reset (const char *key)
{
  g_return_if_fail (epimone_shortcuts_lookup (key) != NULL);

  g_settings_reset (epimone_shortcuts_get_settings (), key);
}

void
epimone_shortcuts_reset_all (void)
{
  GSettings *settings = epimone_shortcuts_get_settings ();

  /* One write batch: the accelerator table is re-applied once at the end
   * rather than once per key. */
  g_settings_delay (settings);
  for (guint i = 0; i < G_N_ELEMENTS (epimone_shortcut_table); i++)
    g_settings_reset (settings, epimone_shortcut_table[i].key);
  g_settings_apply (settings);
}

/* True when this keyval, pressed with no modifier, would type something or is
 * a control key the shell needs (Return/Tab/Escape and friends). */
static gboolean
epimone_keyval_is_plain_input (guint keyval)
{
  switch (keyval)
    {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab:
    case GDK_KEY_Escape:
    case GDK_KEY_BackSpace:
    case GDK_KEY_space:
      return TRUE;
    default:
      return gdk_keyval_to_unicode (keyval) != 0;
    }
}

EpimoneAccelVerdict
epimone_shortcuts_check_accel (const char             *accel,
                               const char             *key,
                               char                  **message_out,
                               const EpimoneShortcut **conflict_out)
{
  GdkModifierType mods = 0;
  guint keyval = 0;
  g_autofree char *label = NULL;

  if (message_out != NULL)
    *message_out = NULL;
  if (conflict_out != NULL)
    *conflict_out = NULL;

  /* Unbinding is always allowed: the key simply goes back to the shell. */
  if (accel == NULL || accel[0] == '\0')
    return EPIMONE_ACCEL_OK;

  if (!gtk_accelerator_parse (accel, &keyval, &mods) ||
      !gtk_accelerator_valid (keyval, mods))
    {
      if (message_out != NULL)
        *message_out = g_strdup ("That key combination cannot be used as a shortcut.");
      return EPIMONE_ACCEL_UNPARSABLE;
    }

  label = epimone_shortcuts_format_label (keyval, mods);
  mods &= gtk_accelerator_get_default_mod_mask ();

  /* Plain keys, including Return / Tab / Escape on their own: the terminal
   * would never see them again. Shift alone counts as plain whenever the key
   * still types a character (Shift+A), but leaves Shift+F10 usable. */
  if (mods == 0 || (mods == GDK_SHIFT_MASK && epimone_keyval_is_plain_input (keyval)))
    {
      if (message_out != NULL)
        *message_out = g_strdup_printf (
          "%s needs a modifier such as Ctrl, Alt or Super. On its own it would "
          "stop reaching the shell.", label);
      return EPIMONE_ACCEL_NO_MODIFIER;
    }

  /* Bare Ctrl+letter combinations that belong to the shell. Ctrl+Shift+C is
   * fine; it is Ctrl+C alone that carries SIGINT. */
  if (mods == GDK_CONTROL_MASK)
    {
      guint lower = gdk_keyval_to_lower (keyval);

      for (guint i = 0; i < G_N_ELEMENTS (epimone_reserved_ctrl_keys); i++)
        {
          if (lower != epimone_reserved_ctrl_keys[i])
            continue;

          if (message_out != NULL)
            *message_out = g_strdup_printf (
              "%s belongs to the shell and cannot be used as a shortcut.", label);
          return EPIMONE_ACCEL_SHELL_KEY;
        }
    }

  /* Conflicts are refused, not stolen: the shortcut that already owns the
   * combination keeps it, and the user is told which one that is. */
  for (guint i = 0; i < G_N_ELEMENTS (epimone_shortcut_table); i++)
    {
      const EpimoneShortcut *other = &epimone_shortcut_table[i];
      g_autofree char *other_accel = NULL;
      GdkModifierType other_mods = 0;
      guint other_keyval = 0;

      if (g_strcmp0 (other->key, key) == 0)
        continue;

      other_accel = epimone_shortcuts_dup_accel (other->key);
      if (other_accel[0] == '\0' ||
          !gtk_accelerator_parse (other_accel, &other_keyval, &other_mods))
        continue;

      other_mods &= gtk_accelerator_get_default_mod_mask ();
      if (other_keyval != keyval || other_mods != mods)
        continue;

      if (conflict_out != NULL)
        *conflict_out = other;
      if (message_out != NULL)
        *message_out = g_strdup_printf (
          "%s is already used by “%s”. Change or unbind that shortcut first.",
          label, other->label);
      return EPIMONE_ACCEL_CONFLICT;
    }

  return EPIMONE_ACCEL_OK;
}

/* ------------------------------------------------------------------ *
 * Applying the table to the application
 * ------------------------------------------------------------------ */

static void
epimone_shortcuts_apply_to_app (GtkApplication *app)
{
  for (guint i = 0; i < G_N_ELEMENTS (epimone_shortcut_table); i++)
    {
      g_autofree char *accel = epimone_shortcuts_dup_accel (epimone_shortcut_table[i].key);
      const char *accels[] = { accel, NULL };

      /* An unbound action gets an empty list, which is how the combination is
       * handed back to the terminal. */
      if (accel[0] == '\0')
        accels[0] = NULL;

      gtk_application_set_accels_for_action (app,
                                             epimone_shortcut_table[i].action,
                                             accels);
    }

  /* Every set_accels_for_action above made GTK re-render the accelerator
   * label of every open menu item, synchronously, and back in GTK's
   * Shift-first order (verified: this happens even when the value did not
   * change, and even for items whose accel comes from a menu attribute).
   * Re-fix them all, after the whole table has been pushed. Windows created
   * later fix their own menus at construction. */
  for (GList *l = gtk_application_get_windows (app); l != NULL; l = l->next)
    epimone_shortcuts_fix_accel_labels (GTK_WIDGET (l->data));
}

static void
epimone_shortcuts_changed_cb (GSettings *settings, const char *key, gpointer user_data)
{
  (void) settings;
  (void) key;

  /* Cheap enough to re-push the whole table: fifteen actions, and
   * gtk_application_set_accels_for_action updates every open window, so a
   * rebind is live with no restart. */
  epimone_shortcuts_apply_to_app (GTK_APPLICATION (user_data));
}

void
epimone_shortcuts_bind_application (GtkApplication *app)
{
  static gboolean connected = FALSE;

  g_return_if_fail (GTK_IS_APPLICATION (app));

  if (!connected)
    {
      connected = TRUE;
      g_signal_connect_object (epimone_shortcuts_get_settings (), "changed",
                               G_CALLBACK (epimone_shortcuts_changed_cb), app, 0);
    }

  epimone_shortcuts_apply_to_app (app);
}
