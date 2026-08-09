#include "epimone-window.h"
#include "epimone-client.h"
#include "epimone-layout.h"
#include "epimone-page.h"
#include "epimone-palette.h"
#include "epimone-chrome.h"

/* Hairline pane dividers. GtkPaned's handle is the `separator` CSS node under
 * `paned`, with `.horizontal`/`.vertical` orientation classes.
 *
 * The seam is built with the box model, not a background gradient: pixel-stop
 * gradients do not render reliably on a node this small. The separator's min
 * content size is the 1px visible line (the device-pixel floor for a crisp
 * line), painted with `background-clip: content-box`; 1px of transparent
 * padding on each side widens the draggable handle to 3px without widening
 * the line. A fainter look must come from the line's alpha, not its size.
 *
 * The `paned` background shows through that transparent padding, so it must
 * match the terminal background or it reads as a band either side of the
 * line. The palette-following rule lives in epimone-chrome.c (same provider
 * priority, added later, so it wins); the #000 here is only the pre-palette
 * default, covering the instants before apply_saved_appearance() runs. It
 * matches the FORCE_DARK scheme set below; without it a light band from the
 * theme default could flash. The seam itself is a light line: visible on dark
 * backgrounds while staying neutral. On light palettes that composites to
 * ~1:1 contrast, so epimone-chrome.c overrides it (same cascade mechanism as
 * the paned background) with the mirror-image black at the same alpha, and
 * also drops libadwaita's inset separator box-shadow there; dark palettes
 * keep this rule untouched. The alpha (0.35) is deliberately low so the 1px
 * line reads as a fine hairline rather than a hard seam. */
static const char EPIMONE_CSS[] =
  "paned {"
  "  background-color: #000000;"
  "}"
  "paned > separator {"
  "  min-width: 1px;"
  "  min-height: 1px;"
  "  border: none;"
  "  background-color: rgba(255,255,255,0.35);"
  "  background-clip: content-box;"
  "}"
  "paned.horizontal > separator {"
  "  padding: 0 1px;"   /* 1px line + 2px transparent grab margin = 3px handle */
  "}"
  "paned.vertical > separator {"
  "  padding: 1px 0;"
  "}"
  /* Overlay scrollbar: no ring around the slider.
   *
   * libadwaita outlines the overlay slider so it stays visible on unknown
   * content:
   *
   *   scrollbar.overlay-indicator > range > trough > slider {
   *     outline: 1px solid var(--scrollbar-outline-color); }
   *
   * In dark that colour is RGB(0 0 12 / 95%), near-opaque black, which over a
   * dark terminal reads as a hard border drawn around the slider (light
   * palettes get white, same problem inverted). Zeroing the outline also
   * keeps the slider's damage region off the border pixel, so fractional
   * scaling does not cascade into a compositor alpha blend
   * (GNOME/libadwaita#800). Vertical only: terminal panes are created
   * GTK_POLICY_NEVER horizontally (epimone_page_wrap_terminal), so no
   * horizontal overlay slider exists. */
  "scrollbar.overlay-indicator.vertical range.vertical trough slider {"
  "  outline-width: 0;"
  "}"
  /* Header elevation: keep the lift, drop the drawn line.
   *
   * libadwaita's raised-top-bar shadow is TWO layers under the whole
   * header + tab-bar revealer:
   *
   *   toolbarview > .top-bar.raised {
   *     box-shadow: 0 1px     <headerbar-shade 50%>,   <- L1: no blur = hairline
   *                 0 2px 4px <headerbar-shade 50%>; } <- L2: the soft lift
   *
   * L1 has no blur radius, so it renders as a solid 1px line immediately
   * below the header, darker than both the header and the body, which reads
   * as a hard drawn separator. L2 is the actual elevation: a 4px gradient
   * that makes the header look raised above the terminal.
   *
   * So the rule is re-declared with L2 only. `--headerbar-shade-color` is
   * libadwaita's own variable (RGB(0 0 6 / 36%) dark, 12% light) and GTK
   * cascades custom properties per element rather than per provider, so
   * reusing it keeps the lift theme-tuned and light/dark aware for free.
   *
   * Bottom bars and `.titlebar:not(.flat)` (the same shadow on plain
   * GtkWindow-titlebar dialogs) get nothing; epimone has no bottom bars, and a
   * dialog titlebar has no content below it to lift off. box-shadow only; no
   * border or outline is added anywhere. */
  "toolbarview > .top-bar.raised,"
  "toolbarview > .top-bar.raised.border {"
  "  box-shadow: 0 2px 4px color-mix(in srgb,"
  "              var(--headerbar-shade-color) 50%, transparent);"
  "}"
  "toolbarview > .bottom-bar.raised,"
  "toolbarview > .bottom-bar.raised.border,"
  "window:not(.ssd-frame) > .titlebar:not(.flat) {"
  "  box-shadow: none;"
  "}";
  /* The header sits a step lighter than the terminal body (epimone-chrome.c);
   * that tonal difference is the only separation. libadwaita's raised-top-bar
   * hairline/drop shadow is neutralised above, and there is no hand-rolled
   * box-shadow either.
   *
   * The preferences window follows the palette chrome theming too
   * (epimone-chrome.c redefines libadwaita's named colors globally), so its
   * header/sidebar/cards recolor with the selected palette. Nothing is
   * painted onto a window node, so no border. */

/* Read the saved appearance from GSettings and push it to the core terminal
 * setters. Runs once at startup, before any terminal is built, so the initial
 * (and restored) terminals inherit the saved theme/font/cursor/padding. The
 * settings window later updates these live while it is open. */
static void
apply_saved_appearance (void)
{
  g_autoptr (GSettings) settings = g_settings_new ("org.felix.Epimone");
  g_autofree char *theme = g_settings_get_string (settings, "theme");
  g_autofree char *font = g_settings_get_string (settings, "font-name");
  g_autofree char *cursor_id = g_settings_get_string (settings, "cursor-shape");
  const EpimonePalette *p = epimone_palette_by_id (theme);
  GdkRGBA bg, fg, cursor, ansi[16];

  if (p == NULL)
    p = epimone_palette_by_id ("epimone-black");
  if (p != NULL && epimone_palette_get_rgba (p, &bg, &fg, &cursor, ansi))
    {
      epimone_terminals_set_colors (&bg, &fg, &cursor, ansi, 16);
      epimone_chrome_apply_palette (&bg, &fg);   /* theme the GUI to match */
    }

  epimone_terminals_set_font (font);
  epimone_terminals_set_cursor_shape (epimone_cursor_shape_from_id (cursor_id));
  epimone_terminals_set_padding (g_settings_get_int (settings, "cell-padding"));

  /* Terminal behaviour. "scrollback-unlimited" wins over the line count, and
   * resolves to VTE's -1; the stored count is deliberately left alone so it
   * comes back untouched when the switch goes off again. */
  epimone_terminals_set_scrollback_lines (
    g_settings_get_boolean (settings, "scrollback-unlimited")
      ? -1
      : g_settings_get_int (settings, "scrollback-lines"));
  epimone_terminals_set_audible_bell (
    g_settings_get_boolean (settings, "audible-bell"));
  epimone_terminals_set_scroll_on_output (
    g_settings_get_boolean (settings, "scroll-on-output"));
  epimone_terminals_set_scroll_on_keystroke (
    g_settings_get_boolean (settings, "scroll-on-keystroke"));
  epimone_pages_set_dim_inactive (
    g_settings_get_boolean (settings, "dim-inactive-panes"));

  /* Advanced. The three compatibility keys are per-terminal VTE settings, so
   * they inherit through the appearance registry like cursor/scrollback;
   * preserve-directory is read at spawn time. exit-action is deliberately not
   * read here: the key exists but is not wired up yet. */
  {
    g_autofree char *backspace = g_settings_get_string (settings, "backspace-binding");
    g_autofree char *delete_id = g_settings_get_string (settings, "delete-binding");
    g_autofree char *cjk = g_settings_get_string (settings, "cjk-ambiguous-width");
    g_autofree char *preserve = g_settings_get_string (settings, "preserve-directory");

    epimone_terminals_set_backspace_binding (epimone_erase_binding_from_id (backspace));
    epimone_terminals_set_delete_binding (epimone_erase_binding_from_id (delete_id));
    epimone_terminals_set_cjk_ambiguous_width (g_strcmp0 (cjk, "wide") == 0 ? 2 : 1);
    epimone_terminals_set_preserve_directory (
      epimone_preserve_directory_from_id (preserve));
  }

  /* General. The default grid must be set before any terminal is built, which
   * is the case here: this runs from startup, before the first window. */
  epimone_terminals_set_default_size (
    g_settings_get_int (settings, "default-columns"),
    g_settings_get_int (settings, "default-rows"));

  {
    g_autofree char *tab_pos = g_settings_get_string (settings, "tab-position");
    g_autofree char *tab_policy = g_settings_get_string (settings, "tab-bar-policy");

    epimone_windows_set_tab_bar_at_bottom (g_strcmp0 (tab_pos, "bottom") == 0);
    epimone_windows_set_tab_bar_policy (
      epimone_tab_bar_policy_from_id (tab_policy));
  }
  epimone_windows_set_confirm_close (
    g_settings_get_boolean (settings, "confirm-close"));
}

/* Window / app icon.
 *
 * The main mechanism is the GApplication id: it is "org.felix.Epimone", which
 * GTK hands to the compositor as the toplevel app_id, and the shell matches
 * that against org.felix.Epimone.desktop to find Icon=org.felix.Epimone. That
 * path needs no code; it works as soon as the .desktop file is installed.
 *
 * The two calls here cover what the app id alone does not:
 *
 *  - the icon-name default, so every window (main and preferences) carries the
 *    name explicitly. Wayland ignores it in favour of the app id, but X11
 *    window lists and Alt-Tab read it, and GTK uses it for its own chrome.
 *
 *  - an uninstalled run has no icons in XDG_DATA_DIRS at all, so the theme
 *    lookup fails and the shell falls back to a generic icon. Adding the
 *    in-tree data/icons to the search path makes ./build/epimone show the real
 *    icon too. Guarded on the directory existing, so an installed copy (where
 *    the source tree may be gone) simply skips it and uses the system theme. */
static void
epimone_setup_icon (void)
{
  gtk_window_set_default_icon_name ("org.felix.Epimone");

#ifdef EPIMONE_ICONS_SRC
  if (g_file_test (EPIMONE_ICONS_SRC, G_FILE_TEST_IS_DIR))
    {
      GdkDisplay *display = gdk_display_get_default ();

      if (display != NULL)
        gtk_icon_theme_add_search_path (gtk_icon_theme_get_for_display (display),
                                        EPIMONE_ICONS_SRC);
    }
#endif
}

static void
startup_cb (GApplication *app, gpointer user_data)
{
  GtkCssProvider *provider = gtk_css_provider_new ();

  (void) app;
  (void) user_data;

  /* Default to dark before the palette applies; apply_saved_appearance() below
   * calls into the chrome theming, which then pins the color scheme (dark or
   * light) to the saved palette's luminance. */
  adw_style_manager_set_color_scheme (adw_style_manager_get_default (),
                                      ADW_COLOR_SCHEME_FORCE_DARK);

  gtk_css_provider_load_from_string (provider, EPIMONE_CSS);
  gtk_style_context_add_provider_for_display (gdk_display_get_default (),
                                              GTK_STYLE_PROVIDER (provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref (provider);

  epimone_setup_icon ();

  /* Establish the saved appearance before any terminal exists. */
  apply_saved_appearance ();
}

/* ------------------------------------------------------------------ *
 * Application-scoped actions, and the CLI flags that drive them.
 *
 * app.new-window / app.new-tab / app.preferences exist for callers that have
 * no window in hand: the dock icon's Desktop Actions (via the CLI flags
 * below) and any second `epimone --<flag>` invocation, which GApplication
 * forwards to the primary instance over D-Bus. Each one resolves the most
 * recently focused Epimone window and activates the corresponding win.*
 * action on it, so the behaviour itself has exactly one implementation
 * (the window action the menus already use); these are thin routers.
 *
 * Desktop Actions are Exec-based; DBusActivatable would need a D-Bus service
 * file and would buy startup-notification integration. Any further CLI flags
 * should follow the same pattern: local options that forward to app actions
 * on the primary instance. */

/* The most recently focused terminal window, or NULL. The windows list is
 * MRU-ordered but also holds non-terminal windows (settings), so filter. */
static EpimoneWindow *
epimone_app_focused_window (GtkApplication *app)
{
  GList *l;

  for (l = gtk_application_get_windows (app); l != NULL; l = l->next)
    if (EPIMONE_IS_WINDOW (l->data))
      return EPIMONE_WINDOW (l->data);
  return NULL;
}

/* Route an app action to @win_action on the focused window; with no window
 * (cold forward, or only a settings window left holding the app open) fall
 * back to a normal activate, which produces or presents a window. For
 * "preferences" the activate fallback is followed by a retry so the request
 * is not silently dropped. */
static void
epimone_app_route_to_window (GApplication *app,
                             const char   *win_action,
                             gboolean      retry_after_activate)
{
  EpimoneWindow *win = epimone_app_focused_window (GTK_APPLICATION (app));

  if (win == NULL)
    {
      g_application_activate (app);
      if (!retry_after_activate)
        return;
      win = epimone_app_focused_window (GTK_APPLICATION (app));
      if (win == NULL)
        return;
    }

  g_action_group_activate_action (G_ACTION_GROUP (win), win_action, NULL);
}

static void
app_new_window_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action; (void) param;
  epimone_app_route_to_window (G_APPLICATION (user_data), "new-window", FALSE);
}

static void
app_new_tab_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  /* No window -> plain activate: a fresh launch already opens with a tab. */
  (void) action; (void) param;
  epimone_app_route_to_window (G_APPLICATION (user_data), "new-tab", FALSE);
}

static void
app_preferences_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action; (void) param;
  epimone_app_route_to_window (G_APPLICATION (user_data), "preferences", TRUE);
}

static const GActionEntry epimone_app_actions[] = {
  { .name = "new-window",  .activate = app_new_window_action },
  { .name = "new-tab",     .activate = app_new_tab_action },
  { .name = "preferences", .activate = app_preferences_action },
};

static const GOptionEntry epimone_main_options[] = {
  { "new-window", 0, 0, G_OPTION_ARG_NONE, NULL,
    "Open a new window", NULL },
  { "new-tab", 0, 0, G_OPTION_ARG_NONE, NULL,
    "Open a new tab in the most recently focused window", NULL },
  { "preferences", 0, 0, G_OPTION_ARG_NONE, NULL,
    "Show the preferences window", NULL },
  { NULL }
};

/* Runs in every invocation before D-Bus registration. When one of the flags
 * above is present, register and branch on the outcome: as a remote instance,
 * activate the app action (GApplication proxies it to the primary) and exit,
 * so no duplicate app. As the primary (cold start), record the request and
 * fall through to the normal startup/activate; activate_cb applies it after
 * the usual launch so the daemon/restore path is not bypassed. */
static int
handle_local_options_cb (GApplication *app,
                         GVariantDict *options,
                         gpointer      user_data)
{
  static const char *flag_names[] = { "new-window", "new-tab", "preferences" };
  const char *requested = NULL;
  GError *err = NULL;
  guint i;

  (void) user_data;

  for (i = 0; i < G_N_ELEMENTS (flag_names); i++)
    if (g_variant_dict_contains (options, flag_names[i]))
      {
        requested = flag_names[i];
        break;
      }

  if (requested == NULL)
    return -1;   /* no flag: default processing, launch as before */

  if (!g_application_register (app, NULL, &err))
    {
      g_printerr ("epimone: %s\n", err->message);
      g_error_free (err);
      return 1;
    }

  if (g_application_get_is_remote (app))
    {
      g_action_group_activate_action (G_ACTION_GROUP (app), requested, NULL);
      return 0;   /* forwarded to the primary; this process exits */
    }

  g_object_set_data (G_OBJECT (app), "epimone-pending-action",
                     (gpointer) requested);
  return -1;
}

static void
activate_cb (AdwApplication *app, gpointer user_data)
{
  GtkWidget *window;
  GError *err = NULL;
  const char *pending;

  (void) user_data;

  /* Re-activation while already running: just present an existing window. */
  if (gtk_application_get_windows (GTK_APPLICATION (app)) != NULL)
    {
      gtk_window_present (GTK_WINDOW (gtk_application_get_windows (GTK_APPLICATION (app))->data));
      return;
    }

  /* Make sure the persistence daemon is up before any session is created. */
  if (!epimone_client_ensure_daemon (&err))
    {
      g_warning ("epimone: %s", err->message);
      g_clear_error (&err);
    }

  /* Compare the recorded daemon instance with the running one before anything
   * relies on stored group ids. Only logs; nothing acts on the result yet. */
  epimone_layout_check_instance ();

  /* Restore the saved layout if there is one and any of its sessions survive;
   * otherwise start fresh with a single window/tab/session. */
  if (!epimone_layout_restore (app))
    {
      window = epimone_window_new (app);
      epimone_window_add_tab (EPIMONE_WINDOW (window));
      gtk_window_present (GTK_WINDOW (window));
    }

  /* A flag on a cold start: the launch above already IS the new window (and
   * its tab), so --new-window and --new-tab need nothing more. Only
   * --preferences has an extra step, opening settings over the new window;
   * a settings-only instance would make no sense, since Epimone settings act
   * on live windows. */
  pending = g_object_get_data (G_OBJECT (app), "epimone-pending-action");
  if (pending != NULL)
    {
      g_object_set_data (G_OBJECT (app), "epimone-pending-action", NULL);
      if (g_strcmp0 (pending, "preferences") == 0)
        g_action_group_activate_action (G_ACTION_GROUP (app), "preferences", NULL);
    }
}

/* Make the GSettings schema discoverable when running straight from the build
 * tree (./build/epimone) without the caller setting GSETTINGS_SCHEMA_DIR.
 *
 * The uninstalled layout is: exe at build/epimone, compiled schema at
 * build/data/gschemas.compiled, i.e. <exedir>/data/gschemas.compiled. If that
 * file exists this is an uninstalled run, so prepend <exedir>/data to the
 * schema search path. On a real install the exe lives in <prefix>/bin with no
 * such file beside it, so the guard fails and nothing happens: normal system
 * resolution (schema in <datadir>/glib-2.0/schemas) applies, and a genuinely
 * missing schema still hard-aborts in g_settings_new() as before.
 *
 * Must run before any GSettings access and before GTK/Adw init, since GLib
 * caches its schema source on first use. */
static void
epimone_setup_dev_schema_dir (void)
{
  g_autofree char *exe = NULL;
  g_autofree char *exedir = NULL;
  g_autofree char *schema_dir = NULL;
  g_autofree char *compiled = NULL;
  const char *existing;

  exe = g_file_read_link ("/proc/self/exe", NULL);
  if (exe == NULL)
    return;   /* no /proc/self/exe: leave system resolution untouched */

  exedir = g_path_get_dirname (exe);
  schema_dir = g_build_filename (exedir, "data", NULL);
  compiled = g_build_filename (schema_dir, "gschemas.compiled", NULL);

  if (!g_file_test (compiled, G_FILE_TEST_EXISTS))
    return;   /* installed layout: no dev-tree schema beside the exe */

  /* Prepend, preserving any value the caller explicitly set (colon-separated). */
  existing = g_getenv ("GSETTINGS_SCHEMA_DIR");
  if (existing != NULL && existing[0] != '\0')
    {
      g_autofree char *combined = g_strconcat (schema_dir, ":", existing, NULL);
      g_setenv ("GSETTINGS_SCHEMA_DIR", combined, TRUE);
    }
  else
    {
      g_setenv ("GSETTINGS_SCHEMA_DIR", schema_dir, TRUE);
    }
}

int
main (int argc, char *argv[])
{
  g_autoptr (AdwApplication) app = NULL;

  /* Before anything touches GSettings: make the build-tree schema loadable so
   * an uninstalled `./build/epimone` works without a manual env var. */
  epimone_setup_dev_schema_dir ();

  app = adw_application_new ("org.felix.Epimone",
                             G_APPLICATION_DEFAULT_FLAGS);
  g_application_add_main_option_entries (G_APPLICATION (app),
                                         epimone_main_options);
  g_action_map_add_action_entries (G_ACTION_MAP (app),
                                   epimone_app_actions,
                                   G_N_ELEMENTS (epimone_app_actions),
                                   app);
  g_signal_connect (app, "startup", G_CALLBACK (startup_cb), NULL);
  g_signal_connect (app, "activate", G_CALLBACK (activate_cb), NULL);
  g_signal_connect (app, "handle-local-options",
                    G_CALLBACK (handle_local_options_cb), NULL);

  return g_application_run (G_APPLICATION (app), argc, argv);
}
