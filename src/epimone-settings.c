#include "epimone-settings.h"
#include "epimone-page.h"      /* plain-C appearance setters + cursor enum */
#include "epimone-window.h"    /* window-chrome setters + tab-bar policy enum */
#include "epimone-palette.h"   /* built-in palette table */
#include "epimone-shortcuts.h" /* rebindable accelerator table + validation */
#include "epimone-chrome.h"    /* full chrome theming from a palette */
#include "epimone-client.h"    /* system default shell resolution */

/* The preferences window.
 *
 * Layout is a two-pane sidebar split (AdwNavigationSplitView), deliberately
 * NOT the AdwPreferencesWindow top-tab style: a category list on the left,
 * the selected category's content on the right. Most categories are real
 * pages of Adwaita boxed rows bound to GSettings; any without controls yet
 * get a placeholder page. */

struct _EpimoneSettings
{
  AdwWindow parent_instance;

  GSettings          *settings;
  AdwNavigationPage  *content_page;   /* right pane; title tracks selection */
  GtkStack           *content_stack;  /* one page per category */

  /* Appearance page widgets we update from GSettings "changed" signals. */
  GHashTable         *theme_cards;    /* palette id -> GtkWidget* card button */
  AdwComboRow        *cursor_combo;

  /* General page combos, likewise kept in sync from "changed" signals. */
  AdwComboRow        *tab_position_combo;
  AdwComboRow        *tab_policy_combo;

  /* Advanced page combos, kept in sync from "changed" signals. */
  AdwComboRow        *backspace_combo;
  AdwComboRow        *delete_combo;
  AdwComboRow        *cjk_combo;
  AdwComboRow        *preserve_combo;

  /* Shell & Profiles page. shell_paths holds one entry per combo row, in row
   * order, so a selection maps to a 'shell-path' value and back; entry 0 is
   * the empty string, meaning "System default". */
  AdwComboRow        *shell_combo;
  GPtrArray          *shell_paths;
  GtkWidget          *custom_command_entry;   /* AdwEntryRow */
  GtkWidget          *custom_command_expander;/* AdwExpanderRow */
};

G_DEFINE_FINAL_TYPE (EpimoneSettings, epimone_settings, ADW_TYPE_WINDOW)

typedef struct
{
  const char *id;      /* stack child name */
  const char *label;   /* sidebar + content title */
  const char *icon;    /* symbolic icon name */
} EpimoneCategory;

/* The six top-level preference categories, in sidebar order. The row index in
 * the list box maps 1:1 to this array, so selection needs no per-row data. */
static const EpimoneCategory epimone_categories[] = {
  { "general",    "General",          "preferences-system-symbolic" },
  { "appearance", "Appearance",       "preferences-desktop-appearance-symbolic" },
  { "terminal",   "Terminal",         "utilities-terminal-symbolic" },
  { "profiles",   "Shell & Profiles", "system-users-symbolic" },
  { "keyboard",   "Keyboard",         "preferences-desktop-keyboard-symbolic" },
  { "advanced",   "Advanced",         "emblem-system-symbolic" },
};

/* Selecting a sidebar row swaps the content pane to that category's page and
 * updates the content header title. */
static void
epimone_settings_row_selected_cb (GtkListBox    *box,
                                  GtkListBoxRow *row,
                                  gpointer       user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  const EpimoneCategory *cat;
  int idx;

  if (row == NULL)
    return;

  idx = gtk_list_box_row_get_index (row);
  if (idx < 0 || idx >= (int) G_N_ELEMENTS (epimone_categories))
    return;

  cat = &epimone_categories[idx];
  gtk_stack_set_visible_child_name (self->content_stack, cat->id);
  adw_navigation_page_set_title (self->content_page, cat->label);
}

/* Build one sidebar row: an icon plus a label in a horizontal box. */
static GtkWidget *
epimone_settings_make_row (const EpimoneCategory *cat)
{
  GtkWidget *rowbox = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 12);
  GtkWidget *icon = gtk_image_new_from_icon_name (cat->icon);
  GtkWidget *label = gtk_label_new (cat->label);

  gtk_widget_set_margin_top (rowbox, 8);
  gtk_widget_set_margin_bottom (rowbox, 8);
  gtk_widget_set_margin_start (rowbox, 6);
  gtk_widget_set_margin_end (rowbox, 6);

  gtk_label_set_xalign (GTK_LABEL (label), 0.0f);
  gtk_widget_set_hexpand (label, TRUE);

  gtk_box_append (GTK_BOX (rowbox), icon);
  gtk_box_append (GTK_BOX (rowbox), label);
  return rowbox;
}

/* Build one content placeholder: an AdwStatusPage naming the category, for
 * categories whose real controls do not exist yet. */
static GtkWidget *
epimone_settings_make_placeholder (const EpimoneCategory *cat)
{
  GtkWidget *status = adw_status_page_new ();
  /* BOTH the title and the description are rendered as Pango markup, so the
   * label has to be escaped for each (e.g. the '&' in "Shell & Profiles");
   * an unescaped title fails with "Entity did not end with a semicolon". */
  g_autofree char *escaped = g_markup_escape_text (cat->label, -1);
  g_autofree char *desc = g_strdup_printf ("%s settings will appear here.",
                                           escaped);

  adw_status_page_set_icon_name (ADW_STATUS_PAGE (status), cat->icon);
  adw_status_page_set_title (ADW_STATUS_PAGE (status), escaped);
  adw_status_page_set_description (ADW_STATUS_PAGE (status), desc);
  return status;
}

/* ------------------------------------------------------------------ *
 * Appearance page
 * ------------------------------------------------------------------ */

/* One-time CSS: per-palette swatch background colors, the small monospace
 * preview text, and the selected-card ring + checkmark badge. Installed on the
 * default display; guarded so repeated Preferences opens don't stack it. */
static void
epimone_settings_ensure_css (void)
{
  static gboolean installed = FALSE;
  GtkCssProvider *provider;
  GString *css;
  const EpimonePalette *palettes;
  gsize n, i;

  if (installed)
    return;
  installed = TRUE;

  css = g_string_new (
    ".epi-theme-card { padding: 6px; border-radius: 10px; }"
    ".epi-theme-card.epi-theme-selected {"
    "  box-shadow: inset 0 0 0 2px @accent_color;"
    "}"
    ".epi-swatch { padding: 8px; border-radius: 6px; }"
    ".epi-swatch label { font-family: monospace; font-size: 10px; }"
    ".epi-theme-check {"
    "  background-color: @accent_color; color: @accent_fg_color;"
    "  border-radius: 999px; padding: 2px; margin: 4px;"
    "}");

  palettes = epimone_palettes (&n);
  for (i = 0; i < n; i++)
    g_string_append_printf (css, ".epi-swatch-%s { background-color: %s; }",
                            palettes[i].id, palettes[i].background);

  provider = gtk_css_provider_new ();
  gtk_css_provider_load_from_string (provider, css->str);
  gtk_style_context_add_provider_for_display (
    gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
    GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);
  g_string_free (css, TRUE);
}

/* Resolve a theme id to colors and push them to every terminal. */
static void
epimone_settings_apply_theme (const char *id)
{
  const EpimonePalette *p = epimone_palette_by_id (id);
  GdkRGBA bg, fg, cursor, ansi[16];

  if (p == NULL)
    p = epimone_palette_by_id ("epimone-black");
  if (p != NULL && epimone_palette_get_rgba (p, &bg, &fg, &cursor, ansi))
    {
      epimone_terminals_set_colors (&bg, &fg, &cursor, ansi, 16);
      epimone_chrome_apply_palette (&bg, &fg);   /* theme the GUI to match */
    }
}

/* Show the ring + check on the card whose id matches, clear the others. */
static void
epimone_settings_mark_selected_theme (EpimoneSettings *self, const char *id)
{
  GHashTableIter it;
  gpointer key, value;

  g_hash_table_iter_init (&it, self->theme_cards);
  while (g_hash_table_iter_next (&it, &key, &value))
    {
      GtkWidget *card = GTK_WIDGET (value);
      GtkWidget *check = GTK_WIDGET (g_object_get_data (G_OBJECT (card), "epi-check"));
      gboolean selected = (g_strcmp0 ((const char *) key, id) == 0);

      if (selected)
        gtk_widget_add_css_class (card, "epi-theme-selected");
      else
        gtk_widget_remove_css_class (card, "epi-theme-selected");
      if (check != NULL)
        gtk_widget_set_visible (check, selected);
    }
}

static void
epimone_settings_card_clicked_cb (GtkButton *button, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  const char *id = g_object_get_data (G_OBJECT (button), "epi-theme-id");

  /* Write the key; the "changed::theme" handler applies + updates the ring. */
  g_settings_set_string (self->settings, "theme", id);
}

/* A palette card: a mini terminal swatch (bg color + colored sample text) with
 * the theme name below, wrapped in a flat button for click + focus. */
static GtkWidget *
epimone_settings_make_theme_card (EpimoneSettings *self, const EpimonePalette *p)
{
  GtkWidget *swatch = gtk_box_new (GTK_ORIENTATION_VERTICAL, 2);
  GtkWidget *line1 = gtk_label_new (NULL);
  GtkWidget *line2 = gtk_label_new (NULL);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *check = gtk_image_new_from_icon_name ("object-select-symbolic");
  GtkWidget *content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 6);
  GtkWidget *name = gtk_label_new (p->name);
  GtkWidget *button = gtk_button_new ();
  g_autofree char *swatch_class = g_strdup_printf ("epi-swatch-%s", p->id);
  g_autofree char *m1 = NULL;
  g_autofree char *m2 = NULL;

  /* Swatch background comes from the per-palette CSS class. */
  gtk_widget_add_css_class (swatch, "epi-swatch");
  gtk_widget_add_css_class (swatch, swatch_class);
  gtk_widget_set_size_request (swatch, 150, 68);

  /* A prompt-ish line showing this palette's green (ANSI 2) and blue (ANSI 4)
   * against the foreground, plus a plain foreground line. */
  m1 = g_strdup_printf (
    "<span foreground=\"%s\">user@epimone</span>"
    "<span foreground=\"%s\">:</span>"
    "<span foreground=\"%s\">~/src</span>"
    "<span foreground=\"%s\">$ ls</span>",
    p->ansi[2], p->foreground, p->ansi[4], p->foreground);
  gtk_label_set_markup (GTK_LABEL (line1), m1);
  gtk_label_set_xalign (GTK_LABEL (line1), 0.0f);

  m2 = g_strdup_printf ("<span foreground=\"%s\">the quick brown fox</span>",
                        p->foreground);
  gtk_label_set_markup (GTK_LABEL (line2), m2);
  gtk_label_set_xalign (GTK_LABEL (line2), 0.0f);

  gtk_box_append (GTK_BOX (swatch), line1);
  gtk_box_append (GTK_BOX (swatch), line2);

  /* Checkmark badge, top-right, hidden until this card is the selected one. */
  gtk_overlay_set_child (GTK_OVERLAY (overlay), swatch);
  gtk_widget_set_halign (check, GTK_ALIGN_END);
  gtk_widget_set_valign (check, GTK_ALIGN_START);
  gtk_widget_add_css_class (check, "epi-theme-check");
  gtk_widget_set_visible (check, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), check);

  gtk_widget_add_css_class (name, "caption");
  gtk_box_append (GTK_BOX (content), overlay);
  gtk_box_append (GTK_BOX (content), name);

  gtk_button_set_child (GTK_BUTTON (button), content);
  gtk_widget_add_css_class (button, "flat");
  gtk_widget_add_css_class (button, "epi-theme-card");
  gtk_widget_set_tooltip_text (button, p->name);
  g_object_set_data (G_OBJECT (button), "epi-theme-id", (gpointer) p->id);
  g_object_set_data (G_OBJECT (button), "epi-check", check);
  g_signal_connect (button, "clicked",
                    G_CALLBACK (epimone_settings_card_clicked_cb), self);

  g_hash_table_insert (self->theme_cards, (gpointer) p->id, button);
  return button;
}

/* Keep the font chooser to monospace families/faces. */
static gboolean
epimone_settings_mono_filter (gpointer item, gpointer user_data)
{
  if (PANGO_IS_FONT_FAMILY (item))
    return pango_font_family_is_monospace (PANGO_FONT_FAMILY (item));
  if (PANGO_IS_FONT_FACE (item))
    {
      PangoFontFamily *fam = pango_font_face_get_family (PANGO_FONT_FACE (item));
      return fam != NULL && pango_font_family_is_monospace (fam);
    }
  return TRUE;
}

static void
epimone_settings_font_changed_cb (GObject    *button,
                                  GParamSpec *pspec,
                                  gpointer    user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  const PangoFontDescription *desc =
    gtk_font_dialog_button_get_font_desc (GTK_FONT_DIALOG_BUTTON (button));

  if (desc != NULL)
    {
      g_autofree char *s = pango_font_description_to_string (desc);
      g_settings_set_string (self->settings, "font-name", s);
    }
}

static void
epimone_settings_cursor_selected_cb (GObject    *row,
                                     GParamSpec *pspec,
                                     gpointer    user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint sel = adw_combo_row_get_selected (ADW_COMBO_ROW (row));
  EpimoneCursorShape shape = (sel <= EPIMONE_CURSOR_UNDERLINE)
                             ? (EpimoneCursorShape) sel : EPIMONE_CURSOR_BLOCK;

  g_settings_set_string (self->settings, "cursor-shape",
                         epimone_cursor_shape_to_id (shape));
}

/* GSettings "changed" handlers: the single live-apply path. Widgets write keys;
 * these push the new value to the core terminal setters (and reflect back into
 * the widgets, so external changes also apply live). */
static void
epimone_settings_theme_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "theme");

  epimone_settings_apply_theme (id);
  epimone_settings_mark_selected_theme (self, id);
}

static void
epimone_settings_font_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  g_autofree char *font = g_settings_get_string (settings, "font-name");

  epimone_terminals_set_font (font);
}

static void
epimone_settings_cursor_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *cs = g_settings_get_string (settings, "cursor-shape");
  EpimoneCursorShape shape = epimone_cursor_shape_from_id (cs);

  epimone_terminals_set_cursor_shape (shape);
  if (self->cursor_combo != NULL)
    adw_combo_row_set_selected (self->cursor_combo, (guint) shape);
}

static void
epimone_settings_padding_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  epimone_terminals_set_padding (g_settings_get_int (settings, "cell-padding"));
}

/* Scrollback is two keys feeding one core setter: the switch decides whether
 * the stored line count is used at all. Both keys route through here so either
 * one changing recomputes the same effective value. */
static void
epimone_settings_apply_scrollback (GSettings *settings)
{
  epimone_terminals_set_scrollback_lines (
    g_settings_get_boolean (settings, "scrollback-unlimited")
      ? -1
      : g_settings_get_int (settings, "scrollback-lines"));
}

static void
epimone_settings_scrollback_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_settings_apply_scrollback (settings);
}

static void
epimone_settings_bell_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_terminals_set_audible_bell (
    g_settings_get_boolean (settings, "audible-bell"));
}

static void
epimone_settings_scroll_output_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_terminals_set_scroll_on_output (
    g_settings_get_boolean (settings, "scroll-on-output"));
}

static void
epimone_settings_scroll_keystroke_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_terminals_set_scroll_on_keystroke (
    g_settings_get_boolean (settings, "scroll-on-keystroke"));
}

static void
epimone_settings_dim_inactive_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_pages_set_dim_inactive (
    g_settings_get_boolean (settings, "dim-inactive-panes"));
}

/* ------------------------------------------------------------------ *
 * General page
 * ------------------------------------------------------------------ */

/* Combo rows carry an index, so each one needs a small index <-> key-string
 * mapping. Order here must match the GtkStringList built alongside it. */
static const char * const epimone_tab_positions[] = { "top", "bottom" };
static const char * const epimone_tab_policies[] = { "always", "multiple", "never" };

/* Advanced-page combo index -> key-string maps. Order matches the GtkStringList
 * built alongside each combo. The erase list is shared by the Backspace and
 * Delete combos; its order is the labels' order (Automatic / ASCII Delete /
 * Escape Sequence / Control-H / TTY), which is NOT the EpimoneEraseBinding enum
 * order, so the id strings do the mapping. */
static const char * const epimone_erase_ids[] = {
  "auto", "ascii-delete", "delete-sequence", "ascii-backspace", "tty"
};
static const char * const epimone_cjk_widths[] = { "narrow", "wide" };
static const char * const epimone_preserve_dirs[] = { "never", "safe", "always" };

static guint
epimone_index_of (const char * const *ids, guint n, const char *id, guint fallback)
{
  for (guint i = 0; i < n; i++)
    if (g_strcmp0 (ids[i], id) == 0)
      return i;
  return fallback;
}

static void
epimone_settings_tab_position_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < G_N_ELEMENTS (epimone_tab_positions))
    g_settings_set_string (self->settings, "tab-position",
                           epimone_tab_positions[i]);
}

static void
epimone_settings_tab_policy_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < G_N_ELEMENTS (epimone_tab_policies))
    g_settings_set_string (self->settings, "tab-bar-policy",
                           epimone_tab_policies[i]);
}

static void
epimone_settings_tab_position_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "tab-position");

  (void) key;

  epimone_windows_set_tab_bar_at_bottom (g_strcmp0 (id, "bottom") == 0);
  if (self->tab_position_combo != NULL)
    adw_combo_row_set_selected (
      self->tab_position_combo,
      epimone_index_of (epimone_tab_positions,
                        G_N_ELEMENTS (epimone_tab_positions), id, 0));
}

static void
epimone_settings_tab_policy_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "tab-bar-policy");

  (void) key;

  epimone_windows_set_tab_bar_policy (epimone_tab_bar_policy_from_id (id));
  if (self->tab_policy_combo != NULL)
    adw_combo_row_set_selected (
      self->tab_policy_combo,
      epimone_index_of (epimone_tab_policies,
                        G_N_ELEMENTS (epimone_tab_policies), id, 1));
}

static void
epimone_settings_default_size_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_terminals_set_default_size (
    g_settings_get_int (settings, "default-columns"),
    g_settings_get_int (settings, "default-rows"));
}

static void
epimone_settings_confirm_close_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  (void) key;
  (void) user_data;

  epimone_windows_set_confirm_close (
    g_settings_get_boolean (settings, "confirm-close"));
}

/* Build the General content: default window size, tab bar, close behaviour. */
static GtkWidget *
epimone_settings_build_general (EpimoneSettings *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *size_group = adw_preferences_group_new ();
  GtkWidget *tabs_group = adw_preferences_group_new ();
  GtkWidget *close_group = adw_preferences_group_new ();
  GtkWidget *columns_row;
  GtkWidget *rows_row;
  GtkWidget *position_row = adw_combo_row_new ();
  GtkWidget *policy_row = adw_combo_row_new ();
  GtkWidget *confirm_row = adw_switch_row_new ();
  GtkAdjustment *columns_adj;
  GtkAdjustment *rows_adj;
  g_autofree char *position_id = NULL;
  g_autofree char *policy_id = NULL;

  /* ---- Default window size, in cells ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (size_group),
                                   "Default Window Size");
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (size_group),
    "Applies to windows opened from now on; open windows keep their size.");

  columns_adj = gtk_adjustment_new (80.0, 20.0, 500.0, 1.0, 10.0, 0.0);
  columns_row = adw_spin_row_new (columns_adj, 1.0, 0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (columns_row), "Columns");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (columns_row),
                               "Character cells across");
  g_settings_bind (self->settings, "default-columns",
                   columns_adj, "value", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (size_group), columns_row);

  rows_adj = gtk_adjustment_new (24.0, 5.0, 200.0, 1.0, 10.0, 0.0);
  rows_row = adw_spin_row_new (rows_adj, 1.0, 0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (rows_row), "Rows");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (rows_row),
                               "Character cells down");
  g_settings_bind (self->settings, "default-rows",
                   rows_adj, "value", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (size_group), rows_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (size_group));

  /* ---- Tab bar ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (tabs_group), "Tab Bar");

  self->tab_position_combo = ADW_COMBO_ROW (position_row);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (position_row), "Position");
  adw_combo_row_set_model (
    ADW_COMBO_ROW (position_row),
    G_LIST_MODEL (gtk_string_list_new ((const char *[]){ "Top", "Bottom", NULL })));
  position_id = g_settings_get_string (self->settings, "tab-position");
  adw_combo_row_set_selected (
    ADW_COMBO_ROW (position_row),
    epimone_index_of (epimone_tab_positions,
                      G_N_ELEMENTS (epimone_tab_positions), position_id, 0));
  g_signal_connect (position_row, "notify::selected",
                    G_CALLBACK (epimone_settings_tab_position_selected_cb), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (tabs_group), position_row);

  self->tab_policy_combo = ADW_COMBO_ROW (policy_row);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (policy_row), "Show Tab Bar");
  adw_combo_row_set_model (
    ADW_COMBO_ROW (policy_row),
    G_LIST_MODEL (gtk_string_list_new ((const char *[]){
      "Always", "When multiple tabs", "Never", NULL })));
  policy_id = g_settings_get_string (self->settings, "tab-bar-policy");
  adw_combo_row_set_selected (
    ADW_COMBO_ROW (policy_row),
    epimone_index_of (epimone_tab_policies,
                      G_N_ELEMENTS (epimone_tab_policies), policy_id, 1));
  g_signal_connect (policy_row, "notify::selected",
                    G_CALLBACK (epimone_settings_tab_policy_selected_cb), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (tabs_group), policy_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (tabs_group));

  /* ---- Closing ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (close_group), "Closing");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (confirm_row),
                                 "Confirm Before Closing");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (confirm_row),
                               "Ask when a window still has several tabs open");
  g_settings_bind (self->settings, "confirm-close",
                   confirm_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (close_group), confirm_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (close_group));

  /* Live-apply path: keys -> core setters, same as the other pages. */
  g_signal_connect (self->settings, "changed::default-columns",
                    G_CALLBACK (epimone_settings_default_size_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::default-rows",
                    G_CALLBACK (epimone_settings_default_size_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::tab-position",
                    G_CALLBACK (epimone_settings_tab_position_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::tab-bar-policy",
                    G_CALLBACK (epimone_settings_tab_policy_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::confirm-close",
                    G_CALLBACK (epimone_settings_confirm_close_key_changed_cb), self);

  return page;
}

/* Build the Terminal content: scrollback depth, scrolling behaviour, bell. */
static GtkWidget *
epimone_settings_build_terminal (EpimoneSettings *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *scrollback_group = adw_preferences_group_new ();
  GtkWidget *scrolling_group = adw_preferences_group_new ();
  GtkWidget *bell_group = adw_preferences_group_new ();
  GtkWidget *unlimited_row = adw_switch_row_new ();
  GtkWidget *lines_row;
  GtkWidget *output_row = adw_switch_row_new ();
  GtkWidget *keystroke_row = adw_switch_row_new ();
  GtkWidget *bell_row = adw_switch_row_new ();
  GtkAdjustment *lines_adj;

  /* ---- Scrollback: unlimited switch gating a line count ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (scrollback_group),
                                   "Scrollback");
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (scrollback_group),
    "How much output is kept above the visible screen.");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (unlimited_row),
                                 "Unlimited Scrollback");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (unlimited_row),
                               "Keep every line; memory grows with output");
  g_settings_bind (self->settings, "scrollback-unlimited",
                   unlimited_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (scrollback_group),
                             unlimited_row);

  lines_adj = gtk_adjustment_new (10000.0, 0.0, 100000.0, 100.0, 1000.0, 0.0);
  lines_row = adw_spin_row_new (lines_adj, 100.0, 0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (lines_row),
                                 "Scrollback Lines");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (lines_row),
                               "0 keeps no scrollback at all");
  g_settings_bind (self->settings, "scrollback-lines",
                   lines_adj, "value", G_SETTINGS_BIND_DEFAULT);
  /* The line count is meaningless while unlimited is on, so grey it out.
   * GET-only + INVERT_BOOLEAN: the row's sensitivity follows the switch, and
   * nothing ever writes back to the key from the widget. */
  g_settings_bind (self->settings, "scrollback-unlimited",
                   lines_row, "sensitive",
                   G_SETTINGS_BIND_GET | G_SETTINGS_BIND_INVERT_BOOLEAN);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (scrollback_group),
                             lines_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (scrollback_group));

  /* ---- Scrolling: when the view jumps back to the bottom ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (scrolling_group),
                                   "Scrolling");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (output_row),
                                 "Scroll on Output");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (output_row),
                               "Jump to the bottom when new output arrives");
  g_settings_bind (self->settings, "scroll-on-output",
                   output_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (scrolling_group),
                             output_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (keystroke_row),
                                 "Scroll on Keystroke");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (keystroke_row),
                               "Jump to the bottom when you start typing");
  g_settings_bind (self->settings, "scroll-on-keystroke",
                   keystroke_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (scrolling_group),
                             keystroke_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (scrolling_group));

  /* ---- Bell ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (bell_group), "Bell");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (bell_row), "Audible Bell");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (bell_row),
                               "Play a sound when a program rings the bell");
  g_settings_bind (self->settings, "audible-bell",
                   bell_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (bell_group), bell_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (bell_group));

  /* Live-apply path: keys -> core setters, same as the Appearance page. Bound
   * to the key rather than the widget so an external `gsettings set` lands in
   * open terminals too. */
  g_signal_connect (self->settings, "changed::scrollback-lines",
                    G_CALLBACK (epimone_settings_scrollback_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::scrollback-unlimited",
                    G_CALLBACK (epimone_settings_scrollback_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::audible-bell",
                    G_CALLBACK (epimone_settings_bell_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::scroll-on-output",
                    G_CALLBACK (epimone_settings_scroll_output_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::scroll-on-keystroke",
                    G_CALLBACK (epimone_settings_scroll_keystroke_key_changed_cb), self);

  return page;
}

/* ------------------------------------------------------------------ *
 * Advanced page
 * ------------------------------------------------------------------ */

/* Selection handlers: each writes its combo's current id string to the key. The
 * corresponding changed:: handler then applies it and re-syncs the combo, so
 * this path and an external `gsettings set` converge on the same code. */
static void
epimone_settings_backspace_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < G_N_ELEMENTS (epimone_erase_ids))
    g_settings_set_string (self->settings, "backspace-binding", epimone_erase_ids[i]);
}

static void
epimone_settings_delete_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < G_N_ELEMENTS (epimone_erase_ids))
    g_settings_set_string (self->settings, "delete-binding", epimone_erase_ids[i]);
}

static void
epimone_settings_cjk_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < G_N_ELEMENTS (epimone_cjk_widths))
    g_settings_set_string (self->settings, "cjk-ambiguous-width", epimone_cjk_widths[i]);
}

static void
epimone_settings_preserve_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < G_N_ELEMENTS (epimone_preserve_dirs))
    g_settings_set_string (self->settings, "preserve-directory", epimone_preserve_dirs[i]);
}

static void
epimone_settings_backspace_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "backspace-binding");

  (void) key;

  epimone_terminals_set_backspace_binding (epimone_erase_binding_from_id (id));
  if (self->backspace_combo != NULL)
    adw_combo_row_set_selected (
      self->backspace_combo,
      epimone_index_of (epimone_erase_ids, G_N_ELEMENTS (epimone_erase_ids), id, 0));
}

static void
epimone_settings_delete_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "delete-binding");

  (void) key;

  epimone_terminals_set_delete_binding (epimone_erase_binding_from_id (id));
  if (self->delete_combo != NULL)
    adw_combo_row_set_selected (
      self->delete_combo,
      epimone_index_of (epimone_erase_ids, G_N_ELEMENTS (epimone_erase_ids), id, 0));
}

static void
epimone_settings_cjk_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "cjk-ambiguous-width");

  (void) key;

  epimone_terminals_set_cjk_ambiguous_width (g_strcmp0 (id, "wide") == 0 ? 2 : 1);
  if (self->cjk_combo != NULL)
    adw_combo_row_set_selected (
      self->cjk_combo,
      epimone_index_of (epimone_cjk_widths, G_N_ELEMENTS (epimone_cjk_widths), id, 0));
}

static void
epimone_settings_preserve_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *id = g_settings_get_string (settings, "preserve-directory");

  (void) key;

  epimone_terminals_set_preserve_directory (epimone_preserve_directory_from_id (id));
  if (self->preserve_combo != NULL)
    adw_combo_row_set_selected (
      self->preserve_combo,
      epimone_index_of (epimone_preserve_dirs, G_N_ELEMENTS (epimone_preserve_dirs), id, 2));
}

/* One combo row: labels model, initial selection from @key, write-back on
 * selection. Returns the row so the caller can stash it for changed:: sync. */
static AdwComboRow *
epimone_settings_add_combo (EpimoneSettings   *self,
                            GtkWidget         *group,
                            const char        *title,
                            const char        *subtitle,
                            const char *const *labels,
                            const char *const *ids,
                            guint              n_ids,
                            const char        *key,
                            guint              fallback,
                            GCallback          on_selected)
{
  GtkWidget *row = adw_combo_row_new ();
  g_autofree char *id = g_settings_get_string (self->settings, key);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), title);
  if (subtitle != NULL)
    adw_action_row_set_subtitle (ADW_ACTION_ROW (row), subtitle);
  adw_combo_row_set_model (ADW_COMBO_ROW (row),
                           G_LIST_MODEL (gtk_string_list_new (labels)));
  adw_combo_row_set_selected (ADW_COMBO_ROW (row),
                              epimone_index_of (ids, n_ids, id, fallback));
  g_signal_connect (row, "notify::selected", on_selected, self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (group), row);
  return ADW_COMBO_ROW (row);
}

/* Build the Advanced content: erase-key compatibility, then behaviour. */
static GtkWidget *
epimone_settings_build_advanced (EpimoneSettings *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *compat_group = adw_preferences_group_new ();
  GtkWidget *behavior_group = adw_preferences_group_new ();
  static const char *const erase_labels[] = {
    "Automatic", "ASCII Delete", "Escape Sequence", "Control-H", "TTY", NULL
  };
  static const char *const cjk_labels[] = { "Narrow", "Wide", NULL };
  static const char *const preserve_labels[] = { "Never", "Safe", "Always", NULL };

  /* ---- Compatibility: what the Backspace/Delete keys send ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (compat_group),
                                   "Compatibility");
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (compat_group),
    "Byte sequences and cell widths for programs that expect a particular "
    "terminal. Leave these alone unless something misbehaves.");

  self->backspace_combo = epimone_settings_add_combo (
    self, compat_group, "Backspace Key",
    "Sequence sent when Backspace is pressed",
    erase_labels, epimone_erase_ids, G_N_ELEMENTS (epimone_erase_ids),
    "backspace-binding", 0,
    G_CALLBACK (epimone_settings_backspace_selected_cb));

  self->delete_combo = epimone_settings_add_combo (
    self, compat_group, "Delete Key",
    "Sequence sent when Delete is pressed",
    erase_labels, epimone_erase_ids, G_N_ELEMENTS (epimone_erase_ids),
    "delete-binding", 0,
    G_CALLBACK (epimone_settings_delete_selected_cb));

  self->cjk_combo = epimone_settings_add_combo (
    self, compat_group, "Ambiguous-Width Characters",
    "Cells given East-Asian ambiguous-width characters",
    cjk_labels, epimone_cjk_widths, G_N_ELEMENTS (epimone_cjk_widths),
    "cjk-ambiguous-width", 0,
    G_CALLBACK (epimone_settings_cjk_selected_cb));

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (compat_group));

  /* ---- Behaviour ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (behavior_group),
                                   "Behavior");

  self->preserve_combo = epimone_settings_add_combo (
    self, behavior_group, "Preserve Working Directory",
    "Where a new tab or split starts: Safe skips remote SSH directories",
    preserve_labels, epimone_preserve_dirs, G_N_ELEMENTS (epimone_preserve_dirs),
    "preserve-directory", 2,
    G_CALLBACK (epimone_settings_preserve_selected_cb));

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (behavior_group));

  /* Live-apply path: keys -> core setters, same as the other pages. The three
   * compatibility keys reach open terminals through the appearance registry;
   * preserve-directory only affects the next spawn. */
  g_signal_connect (self->settings, "changed::backspace-binding",
                    G_CALLBACK (epimone_settings_backspace_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::delete-binding",
                    G_CALLBACK (epimone_settings_delete_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::cjk-ambiguous-width",
                    G_CALLBACK (epimone_settings_cjk_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::preserve-directory",
                    G_CALLBACK (epimone_settings_preserve_key_changed_cb), self);

  return page;
}

/* Build the Appearance content: color-scheme grid, font, cursor, padding. */
static GtkWidget *
epimone_settings_build_appearance (EpimoneSettings *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *scheme_group = adw_preferences_group_new ();
  GtkWidget *text_group = adw_preferences_group_new ();
  GtkWidget *window_group = adw_preferences_group_new ();
  GtkWidget *flow = gtk_flow_box_new ();
  GtkWidget *font_button;
  GtkWidget *font_row = adw_action_row_new ();
  GtkWidget *cursor_row = adw_combo_row_new ();
  GtkWidget *padding_row;
  GtkWidget *dim_row = adw_switch_row_new ();
  GtkFontDialog *font_dialog = gtk_font_dialog_new ();
  GtkCustomFilter *mono = gtk_custom_filter_new (epimone_settings_mono_filter,
                                                 NULL, NULL);
  GtkStringList *shapes;
  GtkAdjustment *padding_adj;
  const EpimonePalette *palettes;
  gsize n, i;
  g_autofree char *font_name = NULL;
  g_autofree char *cursor_id = NULL;

  epimone_settings_ensure_css ();

  /* ---- Color Scheme: a grid of palette cards ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (scheme_group),
                                   "Color Scheme");
  adw_preferences_group_set_description (ADW_PREFERENCES_GROUP (scheme_group),
                                         "Palette applied to every terminal.");
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 2);
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow), 4);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), 12);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), 12);
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  palettes = epimone_palettes (&n);
  for (i = 0; i < n; i++)
    gtk_flow_box_append (GTK_FLOW_BOX (flow),
                         epimone_settings_make_theme_card (self, &palettes[i]));
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (scheme_group), flow);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (scheme_group));

  /* ---- Text: font + cursor shape ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (text_group), "Text");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (font_row), "Font");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (font_row),
                               "System monospace when unset");
  gtk_font_dialog_set_filter (font_dialog, GTK_FILTER (mono));
  font_button = gtk_font_dialog_button_new (font_dialog); /* takes dialog */
  gtk_widget_set_valign (font_button, GTK_ALIGN_CENTER);
  font_name = g_settings_get_string (self->settings, "font-name");
  if (font_name != NULL && font_name[0] != '\0')
    {
      PangoFontDescription *d = pango_font_description_from_string (font_name);
      gtk_font_dialog_button_set_font_desc (
        GTK_FONT_DIALOG_BUTTON (font_button), d);
      pango_font_description_free (d);
    }
  g_signal_connect (font_button, "notify::font-desc",
                    G_CALLBACK (epimone_settings_font_changed_cb), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (font_row), font_button);
  adw_action_row_set_activatable_widget (ADW_ACTION_ROW (font_row),
                                         font_button);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (text_group), font_row);

  self->cursor_combo = ADW_COMBO_ROW (cursor_row);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (cursor_row),
                                 "Cursor Shape");
  shapes = gtk_string_list_new ((const char *[]){ "Block", "I-Beam",
                                                  "Underline", NULL });
  adw_combo_row_set_model (ADW_COMBO_ROW (cursor_row), G_LIST_MODEL (shapes));
  cursor_id = g_settings_get_string (self->settings, "cursor-shape");
  adw_combo_row_set_selected (ADW_COMBO_ROW (cursor_row),
                              (guint) epimone_cursor_shape_from_id (cursor_id));
  g_signal_connect (cursor_row, "notify::selected",
                    G_CALLBACK (epimone_settings_cursor_selected_cb), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (text_group), cursor_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (text_group));

  /* ---- Window: terminal padding ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (window_group),
                                   "Window");
  padding_adj = gtk_adjustment_new (8.0, 0.0, 32.0, 2.0, 8.0, 0.0);
  padding_row = adw_spin_row_new (padding_adj, 1.0, 0);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (padding_row), "Padding");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (padding_row),
                               "Space around the text, in pixels");
  g_settings_bind (self->settings, "cell-padding",
                   padding_adj, "value", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (window_group), padding_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (dim_row),
                                 "Dim Inactive Panes");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (dim_row),
                               "Fade the text of unfocused panes in a split tab");
  g_settings_bind (self->settings, "dim-inactive-panes",
                   dim_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (window_group), dim_row);

  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (window_group));

  /* Live-apply path: keys -> core setters. */
  g_signal_connect (self->settings, "changed::theme",
                    G_CALLBACK (epimone_settings_theme_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::font-name",
                    G_CALLBACK (epimone_settings_font_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::cursor-shape",
                    G_CALLBACK (epimone_settings_cursor_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::cell-padding",
                    G_CALLBACK (epimone_settings_padding_key_changed_cb), self);
  g_signal_connect (self->settings, "changed::dim-inactive-panes",
                    G_CALLBACK (epimone_settings_dim_inactive_key_changed_cb), self);

  return page;
}

/* ------------------------------------------------------------------ *
 * Shell & Profiles page
 * ------------------------------------------------------------------ */

/* The candidate shells offered by the combo, in row order: the empty string
 * ("System default") followed by each absolute path listed in /etc/shells,
 * deduplicated and skipping ones that are not currently executable so the list
 * cannot offer a shell that would fail to spawn. A missing or unreadable
 * /etc/shells simply yields the single default entry. */
static GPtrArray *
epimone_settings_shell_paths (void)
{
  GPtrArray *paths = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *contents = NULL;
  g_auto(GStrv) lines = NULL;

  g_ptr_array_add (paths, g_strdup (""));

  if (!g_file_get_contents ("/etc/shells", &contents, NULL, NULL))
    return paths;

  lines = g_strsplit (contents, "\n", -1);
  for (guint i = 0; lines[i] != NULL; i++)
    {
      const char *line = g_strstrip (lines[i]);
      gboolean seen = FALSE;

      if (line[0] != '/')
        continue;   /* comment, blank line, or a relative entry */
      if (!g_file_test (line, G_FILE_TEST_IS_EXECUTABLE))
        continue;

      for (guint j = 1; j < paths->len && !seen; j++)
        seen = g_strcmp0 (g_ptr_array_index (paths, j), line) == 0;
      if (!seen)
        g_ptr_array_add (paths, g_strdup (line));
    }

  return paths;
}

/* Row index for a stored 'shell-path'. An unknown path (a shell removed from
 * /etc/shells since it was chosen) falls back to row 0 rather than silently
 * selecting the wrong entry. */
static guint
epimone_settings_shell_index (EpimoneSettings *self, const char *path)
{
  for (guint i = 0; i < self->shell_paths->len; i++)
    if (g_strcmp0 (g_ptr_array_index (self->shell_paths, i), path) == 0)
      return i;
  return 0;
}

static void
epimone_settings_shell_selected_cb (GObject *row, GParamSpec *pspec, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  guint i = adw_combo_row_get_selected (ADW_COMBO_ROW (row));

  (void) pspec;

  if (i < self->shell_paths->len)
    g_settings_set_string (self->settings, "shell-path",
                           g_ptr_array_index (self->shell_paths, i));
}

static void
epimone_settings_shell_key_changed_cb (GSettings *settings, char *key, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  g_autofree char *path = g_settings_get_string (settings, "shell-path");

  (void) key;

  if (self->shell_combo != NULL)
    adw_combo_row_set_selected (self->shell_combo,
                                epimone_settings_shell_index (self, path));
}

/* Validate the custom command as the user types and show the result on the
 * row itself: a parse failure marks the entry and explains itself in the
 * expander's subtitle, so a command that cannot be split never reaches the
 * spawn path. Word-splitting only; the string is not run through a shell. */
static void
epimone_settings_custom_command_validate (EpimoneSettings *self)
{
  const char *text;
  GError *err = NULL;
  int argc = 0;
  char **argv = NULL;

  if (self->custom_command_entry == NULL)
    return;

  text = gtk_editable_get_text (GTK_EDITABLE (self->custom_command_entry));

  /* Empty is not an error to display while typing; it just is not runnable,
   * and the spawn path treats it as "no custom command". */
  if (text == NULL || *text == '\0')
    {
      gtk_widget_remove_css_class (self->custom_command_entry, "error");
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (self->custom_command_expander),
                                     "Run this instead of a shell");
      return;
    }

  if (g_shell_parse_argv (text, &argc, &argv, &err))
    {
      gtk_widget_remove_css_class (self->custom_command_entry, "error");
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (self->custom_command_expander),
                                     "Run this instead of a shell");
      g_strfreev (argv);
    }
  else
    {
      g_autofree char *msg = g_strdup_printf ("Cannot parse command: %s",
                                              err->message);
      gtk_widget_add_css_class (self->custom_command_entry, "error");
      adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (self->custom_command_expander),
                                     msg);
      g_clear_error (&err);
    }
}

static void
epimone_settings_custom_command_changed_cb (GtkEditable *editable, gpointer user_data)
{
  (void) editable;
  epimone_settings_custom_command_validate (EPIMONE_SETTINGS (user_data));
}

/* Build the Shell & Profiles content: which shell runs, how it is started, and
 * what is typed into it. A single global profile; no named profiles yet. */
static GtkWidget *
epimone_settings_build_profiles (EpimoneSettings *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *shell_group = adw_preferences_group_new ();
  GtkWidget *command_group = adw_preferences_group_new ();
  GtkWidget *shell_row = adw_combo_row_new ();
  GtkWidget *login_row = adw_switch_row_new ();
  GtkWidget *expander = adw_expander_row_new ();
  GtkWidget *custom_row = adw_entry_row_new ();
  GtkWidget *launch_row = adw_entry_row_new ();
  /* set_model takes a ref of its own, so this one is dropped here. */
  g_autoptr (GtkStringList) shell_names = gtk_string_list_new (NULL);
  g_autofree char *shell_path = NULL;
  g_autofree char *system_shell = epimone_client_default_shell ();
  g_autofree char *system_label = NULL;

  self->shell_paths = epimone_settings_shell_paths ();

  /* ---- Which shell ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (shell_group), "Shell");
  adw_preferences_group_set_description (
    ADW_PREFERENCES_GROUP (shell_group),
    "Applies to terminals opened from now on; running ones keep their shell.");

  system_label = g_strdup_printf ("System default (%s)", system_shell);
  gtk_string_list_append (shell_names, system_label);
  for (guint i = 1; i < self->shell_paths->len; i++)
    gtk_string_list_append (shell_names, g_ptr_array_index (self->shell_paths, i));

  self->shell_combo = ADW_COMBO_ROW (shell_row);
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (shell_row), "Default Shell");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (shell_row),
                               "Program run in a new terminal");
  adw_combo_row_set_model (ADW_COMBO_ROW (shell_row), G_LIST_MODEL (shell_names));
  shell_path = g_settings_get_string (self->settings, "shell-path");
  adw_combo_row_set_selected (ADW_COMBO_ROW (shell_row),
                              epimone_settings_shell_index (self, shell_path));
  g_signal_connect (shell_row, "notify::selected",
                    G_CALLBACK (epimone_settings_shell_selected_cb), self);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (shell_group), shell_row);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (login_row),
                                 "Run as Login Shell");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (login_row),
                               "Start the shell as a login shell");
  g_settings_bind (self->settings, "login-shell",
                   login_row, "active", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (shell_group), login_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (shell_group));

  /* ---- Commands ---- */
  adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (command_group),
                                   "Commands");

  self->custom_command_expander = expander;
  self->custom_command_entry = custom_row;
  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (expander),
                                 "Use Custom Command");
  adw_expander_row_set_subtitle (ADW_EXPANDER_ROW (expander),
                                 "Run this instead of a shell");
  adw_expander_row_set_show_enable_switch (ADW_EXPANDER_ROW (expander), TRUE);
  g_settings_bind (self->settings, "use-custom-command",
                   expander, "enable-expansion", G_SETTINGS_BIND_DEFAULT);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (custom_row), "Command");
  g_settings_bind (self->settings, "custom-command",
                   custom_row, "text", G_SETTINGS_BIND_DEFAULT);
  g_signal_connect (custom_row, "changed",
                    G_CALLBACK (epimone_settings_custom_command_changed_cb), self);
  adw_expander_row_add_row (ADW_EXPANDER_ROW (expander), custom_row);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (command_group), expander);

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (launch_row),
                                 "Run Command on Launch");
  g_settings_bind (self->settings, "launch-command",
                   launch_row, "text", G_SETTINGS_BIND_DEFAULT);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (command_group), launch_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (command_group));

  /* Reflect the command already stored, so an invalid one is flagged on open
   * rather than only after the next keystroke. */
  epimone_settings_custom_command_validate (self);

  /* These keys are read at spawn time rather than applied to live terminals,
   * so the only sync needed is keeping the combo honest when the value
   * changes externally (gsettings CLI, a second Preferences window). */
  g_signal_connect (self->settings, "changed::shell-path",
                    G_CALLBACK (epimone_settings_shell_key_changed_cb), self);

  return page;
}

/* ------------------------------------------------------------------ *
 * Keyboard page
 *
 * One row per rebindable action, each showing its accelerator and opening a
 * capture dialog when activated. Storage: a GSettings string key per action;
 * Escape cancels, Backspace unbinds, and the schema default is what "reset"
 * restores.
 *
 * Captured accelerators are validated before being stored:
 *
 *  - accelerators the shell owns (Ctrl+C and friends) and plain unmodified
 *    keys are refused, because a terminal that cannot send SIGINT is broken
 *    in a way the user cannot undo from inside the terminal.
 *
 *  - a combination already in use is refused and the current owner named,
 *    leaving that owner untouched, rather than silently leaving two actions
 *    claiming one combination.
 *
 * No accelerator suppression is needed while capturing: Epimone's shortcuts
 * are GtkApplication accelerators, which GTK installs only on
 * GtkApplicationWindows, and this preferences window is an AdwWindow.
 * Pressing Ctrl+Shift+T here never reaches win.new-tab (verified). The
 * dialog's own key controller runs in the capture phase and stops every key
 * it consumes, so nothing else in the window sees them either.
 * ------------------------------------------------------------------ */

/* Per-row state, owned by the row widget. */
typedef struct
{
  EpimoneSettings       *settings;    /* borrowed; the preferences window */
  const EpimoneShortcut *shortcut;    /* borrowed; static table entry */
  GtkWidget             *accel_label; /* GtkLabel showing the accelerator */
  GtkWidget             *reset_button;
} EpimoneShortcutRow;

/* Live state of one capture dialog, owned by the dialog. */
typedef struct
{
  EpimoneShortcutRow *row;            /* borrowed; the row being edited */
  AdwDialog          *dialog;
  GtkStack           *stack;          /* "capture" / "confirm" */
  GtkWidget          *error_label;    /* refusal text, hidden until needed */
  GtkWidget          *confirm_label;  /* the captured accelerator */
  GtkWidget          *set_button;
  guint               keyval;
  GdkModifierType     modifier;
  gboolean            editing;
} EpimoneAccelCapture;

/* Human-readable form of a stored accelerator, or NULL when unbound. */
static char *
epimone_settings_accel_label (const char *accel)
{
  GdkModifierType mods;
  guint keyval;

  if (accel == NULL || accel[0] == '\0')
    return NULL;
  if (!gtk_accelerator_parse (accel, &keyval, &mods))
    return NULL;

  /* Conventional Ctrl-first order, not GTK's Shift-first; see
   * epimone_shortcuts_conventional_label. */
  return epimone_shortcuts_format_label (keyval, mods);
}

/* Repaint one row from its stored key: accelerator text (or "Disabled"), and
 * a reset button that only appears once the binding differs from the schema
 * default. */
static void
epimone_settings_shortcut_row_update (EpimoneShortcutRow *row)
{
  g_autofree char *accel = epimone_shortcuts_dup_accel (row->shortcut->key);
  g_autofree char *fallback = epimone_shortcuts_dup_default (row->shortcut->key);
  g_autofree char *label = epimone_settings_accel_label (accel);

  gtk_label_set_label (GTK_LABEL (row->accel_label),
                       (label != NULL) ? label : "Disabled");
  if (label != NULL)
    gtk_widget_remove_css_class (row->accel_label, "dim-label");
  else
    gtk_widget_add_css_class (row->accel_label, "dim-label");

  gtk_widget_set_visible (row->reset_button, g_strcmp0 (accel, fallback) != 0);
}

static void
epimone_settings_shortcut_key_changed_cb (GSettings  *settings,
                                          const char *key,
                                          gpointer    user_data)
{
  EpimoneShortcutRow *row = g_object_get_data (G_OBJECT (user_data),
                                               "epimone-shortcut-row");

  (void) settings;
  (void) key;

  if (row != NULL)
    epimone_settings_shortcut_row_update (row);
}

static void
epimone_settings_shortcut_reset_clicked_cb (GtkButton *button, gpointer user_data)
{
  EpimoneShortcutRow *row = user_data;

  (void) button;
  epimone_shortcuts_reset (row->shortcut->key);
}

/* ---- capture dialog ---- */

static void
epimone_settings_capture_show_error (EpimoneAccelCapture *cap, const char *message)
{
  gtk_label_set_label (GTK_LABEL (cap->error_label), message);
  gtk_widget_set_visible (cap->error_label, TRUE);
  gtk_stack_set_visible_child_name (cap->stack, "capture");
  gtk_widget_set_sensitive (cap->set_button, FALSE);

  /* Stay in capture mode: the next combination is tried straight away, with
   * no extra click to get back into the "press a key" state. */
  cap->editing = TRUE;
  cap->keyval = 0;
  cap->modifier = 0;
}

/* Store what the dialog captured, then close it. */
static void
epimone_settings_capture_commit (EpimoneAccelCapture *cap, const char *accel)
{
  epimone_shortcuts_set_accel (cap->row->shortcut->key, accel);
  adw_dialog_close (cap->dialog);
}

static void
epimone_settings_capture_set_clicked_cb (GtkButton *button, gpointer user_data)
{
  EpimoneAccelCapture *cap = user_data;
  g_autofree char *accel = NULL;

  (void) button;

  if (cap->keyval == 0)
    return;

  accel = gtk_accelerator_name (cap->keyval, cap->modifier);
  epimone_settings_capture_commit (cap, accel);
}

static void
epimone_settings_capture_cancel_clicked_cb (GtkButton *button, gpointer user_data)
{
  EpimoneAccelCapture *cap = user_data;

  (void) button;
  adw_dialog_close (cap->dialog);
}

static void
epimone_settings_capture_reset_clicked_cb (GtkButton *button, gpointer user_data)
{
  EpimoneAccelCapture *cap = user_data;

  (void) button;
  epimone_shortcuts_reset (cap->row->shortcut->key);
  adw_dialog_close (cap->dialog);
}

/* Key normalisation, ported from Ptyxis's shortcut accel dialog
 * (src/ptyxis-shortcut-accel-dialog.c, © Endless / Christian Hergert,
 * GPL-3.0-or-later): keep Shift when it changed the case of the key, drop it
 * for repeated arrows, fold ISO_Left_Tab back to Tab, and avoid Alt+Print
 * turning into SysRq. */
static gboolean
epimone_settings_capture_should_drop_shift (guint keyval_was, guint keyval_is)
{
  if (keyval_was == GDK_KEY_Left  || keyval_is == GDK_KEY_Left ||
      keyval_was == GDK_KEY_Right || keyval_is == GDK_KEY_Right ||
      keyval_was == GDK_KEY_Up    || keyval_is == GDK_KEY_Up ||
      keyval_was == GDK_KEY_Down  || keyval_is == GDK_KEY_Down)
    return FALSE;

  return keyval_was == keyval_is;
}

static gboolean
epimone_settings_capture_key_pressed_cb (GtkEventControllerKey *controller,
                                         guint                  keyval,
                                         guint                  keycode,
                                         GdkModifierType        state,
                                         gpointer               user_data)
{
  EpimoneAccelCapture *cap = user_data;
  GdkEvent *event = gtk_event_controller_get_current_event (GTK_EVENT_CONTROLLER (controller));
  EpimoneAccelVerdict verdict;
  GdkModifierType real_mask;
  g_autofree char *accel = NULL;
  g_autofree char *message = NULL;
  g_autofree char *label = NULL;
  guint keyval_lower;

  if (keycode == 0x01D8)   /* macbook fn key: never a shortcut */
    return GDK_EVENT_PROPAGATE;

  if (!cap->editing)
    return GDK_EVENT_PROPAGATE;

  /* Modifiers alone are not a shortcut; wait for the key they qualify. */
  if (event != NULL && gdk_key_event_is_modifier (event))
    return GDK_EVENT_PROPAGATE;

  real_mask = state & gtk_accelerator_get_default_mod_mask ();
  keyval_lower = gdk_keyval_to_lower (keyval);

  if (keyval_lower == GDK_KEY_ISO_Left_Tab)
    keyval_lower = GDK_KEY_Tab;
  if (keyval_lower != keyval)
    real_mask |= GDK_SHIFT_MASK;
  if (keyval_lower == GDK_KEY_Sys_Req && (real_mask & GDK_ALT_MASK) != 0)
    keyval_lower = GDK_KEY_Print;

  /* Escape cancels, Backspace unbinds: the two conventions the dialog's own
   * hint line promises. */
  if (real_mask == 0 && keyval_lower == GDK_KEY_Escape)
    {
      adw_dialog_close (cap->dialog);
      return GDK_EVENT_STOP;
    }

  if (real_mask == 0 && keyval_lower == GDK_KEY_BackSpace)
    {
      epimone_settings_capture_commit (cap, "");
      return GDK_EVENT_STOP;
    }

  cap->keyval = keyval_lower;
  cap->modifier = state & gtk_accelerator_get_default_mod_mask () & ~GDK_LOCK_MASK;

  if ((state & GDK_SHIFT_MASK) != 0 &&
      epimone_settings_capture_should_drop_shift (cap->keyval, keyval))
    cap->modifier &= ~GDK_SHIFT_MASK;
  if ((state & GDK_LOCK_MASK) == 0 && cap->keyval != keyval)
    cap->modifier |= GDK_SHIFT_MASK;
  if (cap->keyval == GDK_KEY_ISO_Left_Tab && cap->modifier == GDK_CONTROL_MASK)
    {
      cap->keyval = GDK_KEY_Tab;
      cap->modifier = GDK_CONTROL_MASK | GDK_SHIFT_MASK;
    }

  accel = gtk_accelerator_name (cap->keyval, cap->modifier);
  verdict = epimone_shortcuts_check_accel (accel, cap->row->shortcut->key,
                                           &message, NULL);
  if (verdict != EPIMONE_ACCEL_OK)
    {
      epimone_settings_capture_show_error (cap, message);
      return GDK_EVENT_STOP;
    }

  /* Accepted: show it and wait for Set, so a mis-hit can still be cancelled. */
  label = epimone_shortcuts_format_label (cap->keyval, cap->modifier);
  gtk_label_set_label (GTK_LABEL (cap->confirm_label), label);
  gtk_widget_set_visible (cap->error_label, FALSE);
  gtk_stack_set_visible_child_name (cap->stack, "confirm");
  gtk_widget_set_sensitive (cap->set_button, TRUE);
  gtk_widget_grab_focus (cap->set_button);
  cap->editing = FALSE;

  return GDK_EVENT_STOP;
}

static void
epimone_settings_shortcut_row_activated_cb (AdwActionRow *action_row, gpointer user_data)
{
  EpimoneShortcutRow *row = user_data;
  EpimoneAccelCapture *cap = g_new0 (EpimoneAccelCapture, 1);
  g_autofree char *prompt = NULL;
  g_autofree char *current = epimone_shortcuts_dup_accel (row->shortcut->key);
  g_autofree char *fallback = epimone_shortcuts_dup_default (row->shortcut->key);
  GtkWidget *toolbar = adw_toolbar_view_new ();
  GtkWidget *header = adw_header_bar_new ();
  GtkWidget *cancel_button = gtk_button_new_with_label ("Cancel");
  GtkWidget *reset_button = gtk_button_new_with_label ("Reset");
  GtkWidget *capture_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *confirm_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *prompt_label;
  GtkWidget *hint_label = gtk_label_new ("Press Escape to cancel, or Backspace "
                                         "to remove the shortcut.");
  GtkEventController *keys;

  cap->row = row;
  cap->editing = TRUE;
  cap->dialog = ADW_DIALOG (adw_dialog_new ());
  cap->set_button = gtk_button_new_with_label ("Set");
  cap->stack = GTK_STACK (gtk_stack_new ());
  cap->error_label = gtk_label_new (NULL);
  cap->confirm_label = gtk_label_new (NULL);

  prompt = g_strdup_printf ("Enter the new shortcut for “%s”.",
                            adw_preferences_row_get_title (ADW_PREFERENCES_ROW (action_row)));
  prompt_label = gtk_label_new (prompt);

  /* ---- header: Cancel | Reset ......... Set ---- */
  adw_header_bar_set_show_start_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_set_show_end_title_buttons (ADW_HEADER_BAR (header), FALSE);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), cancel_button);
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), reset_button);
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), cap->set_button);
  gtk_widget_add_css_class (cap->set_button, "suggested-action");
  gtk_widget_set_sensitive (cap->set_button, FALSE);
  /* Reset is only meaningful once the binding has drifted from the default. */
  gtk_widget_set_visible (reset_button, g_strcmp0 (current, fallback) != 0);

  g_signal_connect (cancel_button, "clicked",
                    G_CALLBACK (epimone_settings_capture_cancel_clicked_cb), cap);
  g_signal_connect (reset_button, "clicked",
                    G_CALLBACK (epimone_settings_capture_reset_clicked_cb), cap);
  g_signal_connect (cap->set_button, "clicked",
                    G_CALLBACK (epimone_settings_capture_set_clicked_cb), cap);

  /* ---- "press a key" page ---- */
  gtk_label_set_wrap (GTK_LABEL (prompt_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (prompt_label), GTK_JUSTIFY_CENTER);
  gtk_widget_set_valign (capture_box, GTK_ALIGN_CENTER);
  gtk_box_append (GTK_BOX (capture_box), prompt_label);

  gtk_label_set_wrap (GTK_LABEL (cap->error_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (cap->error_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (cap->error_label, "error");
  gtk_widget_set_visible (cap->error_label, FALSE);
  gtk_box_append (GTK_BOX (capture_box), cap->error_label);

  gtk_label_set_wrap (GTK_LABEL (hint_label), TRUE);
  gtk_label_set_justify (GTK_LABEL (hint_label), GTK_JUSTIFY_CENTER);
  gtk_widget_add_css_class (hint_label, "dim-label");
  gtk_widget_add_css_class (hint_label, "caption");
  gtk_box_append (GTK_BOX (capture_box), hint_label);

  /* ---- captured-combination page ---- */
  gtk_widget_set_valign (confirm_box, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (cap->confirm_label, "title-2");
  gtk_box_append (GTK_BOX (confirm_box), cap->confirm_label);
  {
    GtkWidget *again = gtk_label_new ("Choose Set to apply it, or Cancel to keep "
                                      "the current shortcut.");

    gtk_label_set_wrap (GTK_LABEL (again), TRUE);
    gtk_label_set_justify (GTK_LABEL (again), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class (again, "dim-label");
    gtk_widget_add_css_class (again, "caption");
    gtk_box_append (GTK_BOX (confirm_box), again);
  }

  gtk_stack_add_named (cap->stack, capture_box, "capture");
  gtk_stack_add_named (cap->stack, confirm_box, "confirm");
  gtk_widget_set_margin_top (GTK_WIDGET (cap->stack), 18);
  gtk_widget_set_margin_bottom (GTK_WIDGET (cap->stack), 18);
  gtk_widget_set_margin_start (GTK_WIDGET (cap->stack), 18);
  gtk_widget_set_margin_end (GTK_WIDGET (cap->stack), 18);

  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar), header);
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar), GTK_WIDGET (cap->stack));

  adw_dialog_set_title (cap->dialog, "Set Shortcut");
  adw_dialog_set_content_width (cap->dialog, 400);
  adw_dialog_set_content_height (cap->dialog, 260);
  adw_dialog_set_child (cap->dialog, toolbar);
  /* Enter confirms once a combination has been captured. It cannot fire during
   * capture: while the dialog is listening, the key handler below consumes
   * every key, and a bare Return is refused as a plain key anyway. */
  adw_dialog_set_default_widget (cap->dialog, cap->set_button);
  g_object_set_data_full (G_OBJECT (cap->dialog), "epimone-accel-capture",
                          cap, g_free);

  /* Capture phase, and every consumed key is stopped: while the dialog is up,
   * a keystroke is a shortcut being chosen, never a shortcut being used. */
  keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed",
                    G_CALLBACK (epimone_settings_capture_key_pressed_cb), cap);
  gtk_widget_add_controller (GTK_WIDGET (cap->dialog), keys);

  adw_dialog_present (cap->dialog, GTK_WIDGET (row->settings));
}

/* Build one shortcut row: title, current accelerator, per-row reset. */
static GtkWidget *
epimone_settings_make_shortcut_row (EpimoneSettings       *self,
                                    const EpimoneShortcut *shortcut)
{
  GtkWidget *row = adw_action_row_new ();
  EpimoneShortcutRow *state = g_new0 (EpimoneShortcutRow, 1);
  g_autofree char *signal_name = g_strdup_printf ("changed::%s", shortcut->key);

  state->settings = self;
  state->shortcut = shortcut;
  state->accel_label = gtk_label_new (NULL);
  state->reset_button = gtk_button_new_from_icon_name ("edit-undo-symbolic");

  /* Also markup, for the same reason as the group title above: no shortcut label
   * contains an '&' today, but the table is plain text by contract and a future
   * entry must not be able to break its own row. */
  {
    g_autofree char *row_title = g_markup_escape_text (shortcut->label, -1);

    adw_preferences_row_set_title (ADW_PREFERENCES_ROW (row), row_title);
  }
  gtk_list_box_row_set_activatable (GTK_LIST_BOX_ROW (row), TRUE);

  gtk_widget_set_valign (state->accel_label, GTK_ALIGN_CENTER);
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), state->accel_label);

  gtk_widget_set_tooltip_text (state->reset_button, "Reset to default");
  gtk_widget_set_valign (state->reset_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (state->reset_button, "flat");
  adw_action_row_add_suffix (ADW_ACTION_ROW (row), state->reset_button);

  g_object_set_data_full (G_OBJECT (row), "epimone-shortcut-row", state, g_free);

  g_signal_connect (state->reset_button, "clicked",
                    G_CALLBACK (epimone_settings_shortcut_reset_clicked_cb), state);
  g_signal_connect (row, "activated",
                    G_CALLBACK (epimone_settings_shortcut_row_activated_cb), state);
  /* Connected against the row, so the handler dies with it; this is also what
   * repaints the row after a reset-all or an external `gsettings set`. */
  g_signal_connect_object (epimone_shortcuts_get_settings (), signal_name,
                           G_CALLBACK (epimone_settings_shortcut_key_changed_cb),
                           row, 0);

  epimone_settings_shortcut_row_update (state);
  return row;
}

static void
epimone_settings_reset_all_response_cb (AdwAlertDialog *dialog,
                                        GAsyncResult   *result,
                                        gpointer        user_data)
{
  const char *response = adw_alert_dialog_choose_finish (dialog, result);

  (void) user_data;

  if (g_strcmp0 (response, "reset") == 0)
    epimone_shortcuts_reset_all ();
}

static void
epimone_settings_reset_all_clicked_cb (GtkButton *button, gpointer user_data)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (user_data);
  AdwDialog *dialog;

  (void) button;

  dialog = adw_alert_dialog_new ("Reset all shortcuts?", NULL);
  adw_alert_dialog_set_body (ADW_ALERT_DIALOG (dialog),
                             "Every keyboard shortcut goes back to its default. "
                             "Shortcuts you have changed or removed will be restored.");
  adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                  "cancel", "_Cancel",
                                  "reset", "_Reset All",
                                  NULL);
  adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog), "reset",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
  adw_alert_dialog_choose (ADW_ALERT_DIALOG (dialog), GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) epimone_settings_reset_all_response_cb,
                           NULL);
}

/* Build the Keyboard content: every rebindable action, grouped as in the
 * shortcut table, plus the reset-everything escape hatch. */
static GtkWidget *
epimone_settings_build_keyboard (EpimoneSettings *self)
{
  GtkWidget *page = adw_preferences_page_new ();
  GtkWidget *group = NULL;
  GtkWidget *reset_group;
  GtkWidget *reset_row;
  GtkWidget *reset_button;
  const EpimoneShortcut *table;
  const char *current_group = NULL;
  guint n_shortcuts = 0;

  table = epimone_shortcuts_table (&n_shortcuts);

  for (guint i = 0; i < n_shortcuts; i++)
    {
      /* The table is ordered by group, so a change of group starts a new one. */
      if (g_strcmp0 (table[i].group, current_group) != 0)
        {
          g_autofree char *group_title = NULL;

          current_group = table[i].group;
          group = adw_preferences_group_new ();
          /* A group title is Pango markup, and these come from a table of
           * plain human text; unescaped, the '&' in "Tabs & Windows" fails to
           * parse and the title is dropped entirely. Escaping here keeps the
           * table plain and covers whatever is added to it later. */
          group_title = g_markup_escape_text (current_group, -1);
          adw_preferences_group_set_title (ADW_PREFERENCES_GROUP (group),
                                           group_title);
          if (i == 0)
            adw_preferences_group_set_description (
              ADW_PREFERENCES_GROUP (group),
              "Select a shortcut to change it. Backspace removes a shortcut, "
              "handing the key back to the shell.");
          adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                                    ADW_PREFERENCES_GROUP (group));
        }

      adw_preferences_group_add (ADW_PREFERENCES_GROUP (group),
                                 epimone_settings_make_shortcut_row (self, &table[i]));
    }

  reset_group = adw_preferences_group_new ();
  reset_row = adw_action_row_new ();
  reset_button = gtk_button_new_with_label ("Reset All");

  adw_preferences_row_set_title (ADW_PREFERENCES_ROW (reset_row),
                                 "Reset All Shortcuts");
  adw_action_row_set_subtitle (ADW_ACTION_ROW (reset_row),
                               "Restore every shortcut on this page to its default");
  gtk_widget_set_valign (reset_button, GTK_ALIGN_CENTER);
  gtk_widget_add_css_class (reset_button, "destructive-action");
  g_signal_connect (reset_button, "clicked",
                    G_CALLBACK (epimone_settings_reset_all_clicked_cb), self);
  adw_action_row_add_suffix (ADW_ACTION_ROW (reset_row), reset_button);
  adw_preferences_group_add (ADW_PREFERENCES_GROUP (reset_group), reset_row);
  adw_preferences_page_add (ADW_PREFERENCES_PAGE (page),
                            ADW_PREFERENCES_GROUP (reset_group));

  return page;
}

static void
epimone_settings_dispose (GObject *object)
{
  EpimoneSettings *self = EPIMONE_SETTINGS (object);

  g_clear_pointer (&self->theme_cards, g_hash_table_unref);
  g_clear_pointer (&self->shell_paths, g_ptr_array_unref);
  g_clear_object (&self->settings);

  G_OBJECT_CLASS (epimone_settings_parent_class)->dispose (object);
}

static void
epimone_settings_class_init (EpimoneSettingsClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = epimone_settings_dispose;
}

static void
epimone_settings_init (EpimoneSettings *self)
{
  AdwNavigationSplitView *split_view;
  AdwNavigationPage *sidebar_page;
  GtkListBox *sidebar_list;
  GtkWidget *sidebar_toolbar;
  GtkWidget *sidebar_scroll;
  GtkWidget *content_toolbar;
  g_autofree char *theme = NULL;

  /* The GSettings instance every page binds its widgets against. Log the
   * loaded `theme` default to confirm the schema compiled and is on the
   * search path. */
  self->settings = g_settings_new ("org.felix.Epimone");
  theme = g_settings_get_string (self->settings, "theme");
  g_message ("epimone: preferences GSettings loaded (theme default = \"%s\")",
             theme);

  /* palette id -> card button; ids are static strings, values borrowed. */
  self->theme_cards = g_hash_table_new (g_str_hash, g_str_equal);

  gtk_window_set_title (GTK_WINDOW (self), "Preferences");
  gtk_window_set_default_size (GTK_WINDOW (self), 820, 560);
  gtk_widget_add_css_class (GTK_WIDGET (self), "epimone-settings");

  /* ---- Left pane: category list ---- */
  sidebar_list = GTK_LIST_BOX (gtk_list_box_new ());
  gtk_list_box_set_selection_mode (sidebar_list, GTK_SELECTION_SINGLE);
  gtk_widget_add_css_class (GTK_WIDGET (sidebar_list), "navigation-sidebar");
  for (guint i = 0; i < G_N_ELEMENTS (epimone_categories); i++)
    gtk_list_box_append (sidebar_list,
                         epimone_settings_make_row (&epimone_categories[i]));
  g_signal_connect (sidebar_list, "row-selected",
                    G_CALLBACK (epimone_settings_row_selected_cb), self);

  sidebar_scroll = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sidebar_scroll),
                                 GTK_WIDGET (sidebar_list));

  sidebar_toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (sidebar_toolbar),
                                adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (sidebar_toolbar), sidebar_scroll);

  sidebar_page = adw_navigation_page_new (sidebar_toolbar, "Preferences");

  /* ---- Right pane: the Appearance page, placeholders for the rest ---- */
  self->content_stack = GTK_STACK (gtk_stack_new ());
  for (guint i = 0; i < G_N_ELEMENTS (epimone_categories); i++)
    {
      GtkWidget *child;

      if (g_strcmp0 (epimone_categories[i].id, "general") == 0)
        child = epimone_settings_build_general (self);
      else if (g_strcmp0 (epimone_categories[i].id, "appearance") == 0)
        child = epimone_settings_build_appearance (self);
      else if (g_strcmp0 (epimone_categories[i].id, "terminal") == 0)
        child = epimone_settings_build_terminal (self);
      else if (g_strcmp0 (epimone_categories[i].id, "profiles") == 0)
        child = epimone_settings_build_profiles (self);
      else if (g_strcmp0 (epimone_categories[i].id, "keyboard") == 0)
        child = epimone_settings_build_keyboard (self);
      else if (g_strcmp0 (epimone_categories[i].id, "advanced") == 0)
        child = epimone_settings_build_advanced (self);
      else
        child = epimone_settings_make_placeholder (&epimone_categories[i]);

      gtk_stack_add_named (self->content_stack, child, epimone_categories[i].id);
    }

  /* Reflect the current theme's selection ring now that the cards exist. */
  epimone_settings_mark_selected_theme (self, theme);

  content_toolbar = adw_toolbar_view_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (content_toolbar),
                                adw_header_bar_new ());
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (content_toolbar),
                                GTK_WIDGET (self->content_stack));

  self->content_page = adw_navigation_page_new (content_toolbar,
                                                epimone_categories[0].label);

  /* ---- The split: sidebar + content, side by side ---- */
  split_view = ADW_NAVIGATION_SPLIT_VIEW (adw_navigation_split_view_new ());
  adw_navigation_split_view_set_sidebar (split_view, sidebar_page);
  adw_navigation_split_view_set_content (split_view, self->content_page);
  adw_navigation_split_view_set_min_sidebar_width (split_view, 220);
  adw_navigation_split_view_set_max_sidebar_width (split_view, 260);

  adw_window_set_content (ADW_WINDOW (self), GTK_WIDGET (split_view));

  /* Start on the first category. */
  gtk_list_box_select_row (sidebar_list,
                           gtk_list_box_get_row_at_index (sidebar_list, 0));
}

GtkWidget *
epimone_settings_new (GtkWindow *parent)
{
  EpimoneSettings *self = g_object_new (EPIMONE_TYPE_SETTINGS, NULL);

  if (parent != NULL)
    {
      GtkApplication *app = gtk_window_get_application (parent);

      gtk_window_set_transient_for (GTK_WINDOW (self), parent);
      if (app != NULL)
        gtk_window_set_application (GTK_WINDOW (self), app);
    }

  return GTK_WIDGET (self);
}
