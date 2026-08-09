#include "epimone-window.h"
#include "epimone-page.h"
#include "epimone-layout.h"
#include "epimone-overview.h"
#include "epimone-settings.h"
#include "epimone-shortcuts.h"
#include "epimone-client.h"

struct _EpimoneWindow
{
  AdwApplicationWindow parent_instance;
  AdwTabView *tab_view;

  /* Kept so the window-chrome preferences can be re-applied live: the tab bar
   * moves between the toolbar view's top and bottom slots, and its autohide /
   * visibility follow the policy. */
  AdwToolbarView *toolbar_view;
  AdwTabBar      *tab_bar;
  gboolean        tab_bar_at_bottom;

  /* Header title. `window_title` shows the active tab's title plus its cwd as a
   * dimmed second line; the two bindings track whichever page is selected and
   * are re-made on every tab switch (see epimone_window_rebind_title). */
  AdwWindowTitle *window_title;
  GBinding       *title_binding;
  GBinding       *subtitle_binding;

  /* Set once the close confirmation has been answered "close", so the
   * close-request handler lets the second attempt through. */
  gboolean        closing;

  /* The tab context menu (AdwTabView::setup-menu), and the page it was opened
   * on. Every item acts on `menu_page` (the tab that was RIGHT-CLICKED,
   * which need not be the selected one), not on the current tab.
   *
   * A WEAK pointer, and cleared from an IDLE rather than when the menu closes:
   * activating an item and the popover closing happen in the same main-loop
   * iteration, and GTK does not document which comes first, so clearing on
   * "closed" could drop the capture before the action reads it. An idle runs
   * after the whole event is dispatched, which makes the order irrelevant.
   * Clearing at all matters because the ACCELERATORS share these actions and
   * must act on the selected tab, not on whatever was right-clicked earlier. */
  GMenu          *tab_menu;
  AdwTabPage     *menu_page;      /* weak; NULL = act on the selected tab */
  guint           menu_page_clear_idle;
  guint           menu_hook_idle;   /* deferred popover hook; see setup_menu_cb */

  /* The session overview. It is the window's CONTENT and wraps the toolbar view
   * (see epimone_overview_set_content), rather than being a page of a stack
   * beside it; that inversion is what lets the tab content be drawn scaled into
   * a card during the zoom instead of merely crossfaded. Built with the window,
   * since it is the content host. */
  GtkWidget      *overview;
};

G_DEFINE_FINAL_TYPE (EpimoneWindow, epimone_window, ADW_TYPE_APPLICATION_WINDOW)

/* ------------------------------------------------------------------ *
 * Window-chrome preferences
 *
 * The same registry shape epimone-page.c uses for terminals: a list of the
 * live windows plus the current desired chrome. Setters update the state and
 * re-apply to every window; a window applies the current state when it is
 * built, so windows opened later inherit it.
 * ------------------------------------------------------------------ */

static GSList *epimone_all_windows = NULL;   /* borrowed EpimoneWindow* */

static struct
{
  EpimoneTabBarPolicy tab_bar_policy;
  gboolean            tab_bar_at_bottom;
  gboolean            confirm_close;
} epimone_window_prefs = {
  /* Mirror the schema defaults. MULTIPLE matches AdwTabBar's own autohide, so
   * an unconfigured window behaves exactly as it did before this existed. */
  .tab_bar_policy = EPIMONE_TAB_BAR_MULTIPLE,
  .tab_bar_at_bottom = FALSE,
  .confirm_close = TRUE,
};

EpimoneTabBarPolicy
epimone_tab_bar_policy_from_id (const char *id)
{
  if (g_strcmp0 (id, "always") == 0)
    return EPIMONE_TAB_BAR_ALWAYS;
  if (g_strcmp0 (id, "never") == 0)
    return EPIMONE_TAB_BAR_NEVER;
  return EPIMONE_TAB_BAR_MULTIPLE;
}

/* Push the whole current chrome state onto one window. */
static void
epimone_apply_window_prefs_to (EpimoneWindow *self)
{
  EpimoneTabBarPolicy policy = epimone_window_prefs.tab_bar_policy;

  if (self->tab_bar == NULL || self->toolbar_view == NULL)
    return;

  /* "never" hides the widget outright; the other two differ only in whether
   * AdwTabBar is allowed to autohide itself down to nothing on one tab. */
  gtk_widget_set_visible (GTK_WIDGET (self->tab_bar),
                          policy != EPIMONE_TAB_BAR_NEVER);
  adw_tab_bar_set_autohide (self->tab_bar,
                            policy == EPIMONE_TAB_BAR_MULTIPLE);

  if (epimone_window_prefs.tab_bar_at_bottom != self->tab_bar_at_bottom)
    {
      /* Re-parent between the toolbar view's two slots. Ref across the move so
       * removing the last reference does not destroy the bar. */
      g_object_ref (self->tab_bar);
      adw_toolbar_view_remove (self->toolbar_view, GTK_WIDGET (self->tab_bar));
      if (epimone_window_prefs.tab_bar_at_bottom)
        adw_toolbar_view_add_bottom_bar (self->toolbar_view,
                                         GTK_WIDGET (self->tab_bar));
      else
        adw_toolbar_view_add_top_bar (self->toolbar_view,
                                      GTK_WIDGET (self->tab_bar));
      g_object_unref (self->tab_bar);

      self->tab_bar_at_bottom = epimone_window_prefs.tab_bar_at_bottom;
    }
}

static void
epimone_apply_window_prefs_to_all (void)
{
  for (GSList *l = epimone_all_windows; l != NULL; l = l->next)
    epimone_apply_window_prefs_to (EPIMONE_WINDOW (l->data));
}

void
epimone_windows_set_tab_bar_policy (EpimoneTabBarPolicy policy)
{
  epimone_window_prefs.tab_bar_policy = policy;
  epimone_apply_window_prefs_to_all ();
}

void
epimone_windows_set_tab_bar_at_bottom (gboolean bottom)
{
  epimone_window_prefs.tab_bar_at_bottom = bottom;
  epimone_apply_window_prefs_to_all ();
}

void
epimone_windows_set_confirm_close (gboolean enabled)
{
  /* Read at close time, so there is nothing to push to open windows. */
  epimone_window_prefs.confirm_close = enabled;
}

static void
epimone_window_unregister_cb (GtkWidget *widget, gpointer user_data)
{
  (void) user_data;

  epimone_all_windows = g_slist_remove (epimone_all_windows, widget);
}

AdwTabView *
epimone_window_get_tab_view (EpimoneWindow *self)
{
  g_return_val_if_fail (EPIMONE_IS_WINDOW (self), NULL);
  return self->tab_view;
}

/* Return the EpimonePage backing the currently selected tab, or NULL. */
static EpimonePage *
epimone_window_current_page (EpimoneWindow *self)
{
  AdwTabPage *tab_page = adw_tab_view_get_selected_page (self->tab_view);

  if (tab_page == NULL)
    return NULL;

  return EPIMONE_PAGE (adw_tab_page_get_child (tab_page));
}

/* Move the keyboard focus onto the selected tab's active pane. The window calls
 * this wherever GTK would otherwise decide for itself where focus goes: after
 * a tab switch, after a new tab, and when a header menu closes. */
static void
epimone_window_focus_current_terminal (EpimoneWindow *self)
{
  EpimonePage *page = epimone_window_current_page (self);

  if (page != NULL)
    epimone_page_focus_terminal (page);
}

/* A page's last pane has closed: close the tab holding it.
 *
 * The window is resolved from the widget tree, not from the connect-time
 * user_data: "Move to New Window" transfers a live page between tab views, and
 * a handler still pointing at the SOURCE window would ask it to close a page
 * it no longer holds, so the moved tab would never close when its last pane
 * exited. Same event-time-resolution rule as epimone_page_for_terminal. The
 * connect-time window stays as the fallback for a page mid-teardown. */
static void
epimone_window_page_close_cb (EpimonePage *page,
                              gpointer     user_data)
{
  GtkWidget *root = gtk_widget_get_ancestor (GTK_WIDGET (page),
                                             EPIMONE_TYPE_WINDOW);
  EpimoneWindow *self = root != NULL ? EPIMONE_WINDOW (root)
                                    : EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (page));

  if (tab_page != NULL)
    adw_tab_view_close_page (self->tab_view, tab_page);
}

/* Close the tab holding @page: an ordinary DETACH, the same thing the tab
 * bar's × and the tab menu's Detach Tab do: the sessions keep running and the
 * group stays restorable. The window is resolved from @page's own tree, so a
 * page that has been moved between windows closes in the one showing it. */
void
epimone_window_close_page (EpimonePage *page)
{
  GtkWidget *root;
  EpimoneWindow *self;
  AdwTabPage *tab_page;

  g_return_if_fail (EPIMONE_IS_PAGE (page));

  root = gtk_widget_get_ancestor (GTK_WIDGET (page), EPIMONE_TYPE_WINDOW);
  if (root == NULL)
    return;
  self = EPIMONE_WINDOW (root);
  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (page));
  if (tab_page != NULL)
    adw_tab_view_close_page (self->tab_view, tab_page);
}

/* Move @page's tab into a window of its own, by TRANSFERRING the live page:
 * the terminals, bridges and sockets are untouched, so no session is detached
 * or re-attached and nothing replays. The group blob is untouched too; only
 * which window shows the group changes, and the debounced layout sync records
 * that by itself. If this was the source window's last tab, its own
 * notify::n-pages path closes it, exactly as when the last tab goes any other
 * way.
 *
 * The window comes from @page's tree rather than from a caller-supplied one,
 * for the same reason epimone_window_page_close_cb resolves it that way: a
 * page's window is whatever is showing it now. Both the tab context menu and
 * the overview card menu call this: one implementation, two entry points. */
void
epimone_window_move_page_to_new_window (EpimonePage *page)
{
  GtkWidget *root;
  EpimoneWindow *self;
  AdwTabPage *tab_page;
  GtkApplication *app;
  GtkWidget *window;

  g_return_if_fail (EPIMONE_IS_PAGE (page));

  root = gtk_widget_get_ancestor (GTK_WIDGET (page), EPIMONE_TYPE_WINDOW);
  if (root == NULL)
    return;
  self = EPIMONE_WINDOW (root);
  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (page));
  app = gtk_window_get_application (GTK_WINDOW (self));
  if (tab_page == NULL || app == NULL)
    return;
  /* The only tab of a window has nowhere to go: moving it would empty this
   * window (closing it) and hand the tab to a new one, which is a lot of churn
   * to end up in an equivalent state. */
  if (adw_tab_view_get_n_pages (self->tab_view) < 2)
    return;

  window = epimone_window_new (ADW_APPLICATION (app));
  adw_tab_view_transfer_page (self->tab_view, tab_page,
                              epimone_window_get_tab_view (EPIMONE_WINDOW (window)),
                              0);
  gtk_window_present (GTK_WINDOW (window));
  epimone_page_focus_terminal (page);
  epimone_layout_schedule_save ();
}

/* When the tab view empties out, close the window. */
static void
epimone_window_notify_n_pages_cb (AdwTabView *tab_view,
                                  GParamSpec *pspec,
                                  gpointer    user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);

  if (adw_tab_view_get_n_pages (tab_view) == 0)
    gtk_window_close (GTK_WINDOW (self));
  else
    epimone_layout_schedule_save ();
}

/* Point the header title at the page that is now selected.
 *
 * EpimonePage already computes both strings and notifies on change (title from
 * VTE's OSC 0/2, subtitle from OSC 7), so a property binding gives live updates
 * for free: a `cd` or a starting command refreshes the header with no work
 * here. Only the *source* changes on a tab switch, so the bindings are dropped
 * and re-made rather than being chased with signal handlers.
 *
 * The GtkWindow's own title is set alongside, so the overview and window list
 * show the same thing instead of a permanent "Epimone". */
/* Drop a stored binding. g_object_bind_property() hands back a borrowed
 * reference, so an extra reference is taken to keep the pointer valid;
 * unbinding releases the binding's internal reference, and the extra one has
 * to go too. */
static void
epimone_window_clear_binding (GBinding **binding)
{
  if (*binding == NULL)
    return;

  g_binding_unbind (*binding);
  g_object_unref (*binding);
  *binding = NULL;
}

static void
epimone_window_rebind_title (EpimoneWindow *self)
{
  AdwTabPage *tab_page;
  EpimonePage *page;

  epimone_window_clear_binding (&self->title_binding);
  epimone_window_clear_binding (&self->subtitle_binding);

  tab_page = adw_tab_view_get_selected_page (self->tab_view);
  if (tab_page == NULL)
    {
      /* No tabs (briefly, during teardown or before the first is added). */
      adw_window_title_set_title (self->window_title, "Epimone");
      adw_window_title_set_subtitle (self->window_title, NULL);
      gtk_window_set_title (GTK_WINDOW (self), "Epimone");
      return;
    }

  page = EPIMONE_PAGE (adw_tab_page_get_child (tab_page));

  self->title_binding =
    g_object_bind_property (page, "title", self->window_title, "title",
                            G_BINDING_SYNC_CREATE);
  self->subtitle_binding =
    g_object_bind_property (page, "subtitle", self->window_title, "subtitle",
                            G_BINDING_SYNC_CREATE);

  g_object_ref (self->title_binding);
  g_object_ref (self->subtitle_binding);
}

/* Keep the toplevel window title in step with the header title. */
static void
epimone_window_notify_header_title_cb (GObject *window_title,
                                       GParamSpec *pspec,
                                       gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  const char *title = adw_window_title_get_title (ADW_WINDOW_TITLE (window_title));

  (void) pspec;

  gtk_window_set_title (GTK_WINDOW (self),
                        (title != NULL && title[0] != '\0') ? title : "Epimone");
}

static void
epimone_window_notify_selected_cb (AdwTabView *tab_view,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  (void) tab_view;
  (void) pspec;

  epimone_window_rebind_title (EPIMONE_WINDOW (user_data));
  /* Don't leave the switched-to tab's focus up to AdwTabView: it carries focus
   * into the new page only when the page being left held it, so a switch made
   * while focus sat anywhere else would keep typing away from the terminal. */
  epimone_window_focus_current_terminal (EPIMONE_WINDOW (user_data));
  epimone_layout_schedule_save ();
}

/* A PINNED tab is drawn narrow (no title, no close button, just an icon), so
 * with no icon set libadwaita has nothing to draw and the tab renders as an
 * empty dashed placeholder. Epimone's own terminal glyph fills it (the same
 * name the overview's thumbnail placeholder uses, so the app has one terminal
 * symbol rather than two).
 *
 * The icon is set only WHILE PINNED and cleared on unpin: an unpinned tab
 * would otherwise show the same glyph beside every title, which carries no
 * information when every tab is a terminal.
 *
 * Driven off the page's own "pinned" property rather than from the pin/unpin
 * actions, so every route into that state (the menu, a future accelerator,
 * a restore) keeps the icon in step with one piece of code. */
static void
epimone_tab_page_notify_pinned_cb (AdwTabPage *tab_page,
                                   GParamSpec *pspec,
                                   gpointer    user_data)
{
  (void) pspec;
  (void) user_data;

  if (adw_tab_page_get_pinned (tab_page))
    {
      g_autoptr (GIcon) icon = g_themed_icon_new ("utilities-terminal-symbolic");

      adw_tab_page_set_icon (tab_page, icon);
    }
  else
    {
      adw_tab_page_set_icon (tab_page, NULL);
    }
}

/* Shared wiring for a page added to the tab view. @position is a tab index,
 * or -1 to append at the end. */
static AdwTabPage *
epimone_window_attach_page_at (EpimoneWindow *self, EpimonePage *page,
                               int position)
{
  AdwTabPage *tab_page;

  if (position < 0)
    tab_page = adw_tab_view_append (self->tab_view, GTK_WIDGET (page));
  else
    tab_page = adw_tab_view_insert (self->tab_view, GTK_WIDGET (page), position);

  g_object_bind_property (page, "title", tab_page, "title", G_BINDING_SYNC_CREATE);
  g_signal_connect (page, "close-page",
                    G_CALLBACK (epimone_window_page_close_cb), self);
  /* Keep the pinned-tab icon in step. On the AdwTabPage, which survives a
   * transfer to another window intact, so a moved tab keeps this. */
  g_signal_connect (tab_page, "notify::pinned",
                    G_CALLBACK (epimone_tab_page_notify_pinned_cb), NULL);
  epimone_tab_page_notify_pinned_cb (tab_page, NULL, NULL);
  return tab_page;
}

static AdwTabPage *
epimone_window_attach_page (EpimoneWindow *self, EpimonePage *page)
{
  return epimone_window_attach_page_at (self, page, -1);
}

static void
epimone_window_add_tab_with_cwd (EpimoneWindow *self, const char *cwd)
{
  GtkWidget *page;
  AdwTabPage *tab_page;

  g_return_if_fail (EPIMONE_IS_WINDOW (self));

  page = epimone_page_new_with_cwd (cwd);
  tab_page = epimone_window_attach_page (self, EPIMONE_PAGE (page));
  adw_tab_view_set_selected_page (self->tab_view, tab_page);
  /* The new pane itself, not the page: grabbing focus on the page leaves the
   * choice of descendant to GTK. */
  epimone_page_focus_terminal (EPIMONE_PAGE (page));
}

void
epimone_window_add_tab (EpimoneWindow *self)
{
  epimone_window_add_tab_with_cwd (self, NULL);
}

AdwTabPage *
epimone_window_adopt_page (EpimoneWindow *self, EpimonePage *page)
{
  g_return_val_if_fail (EPIMONE_IS_WINDOW (self), NULL);
  return epimone_window_attach_page (self, page);
}

void
epimone_window_adopt_and_select_page (EpimoneWindow *self, EpimonePage *page)
{
  AdwTabPage *tab_page;

  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  g_return_if_fail (EPIMONE_IS_PAGE (page));

  tab_page = epimone_window_attach_page (self, page);
  adw_tab_view_set_selected_page (self->tab_view, tab_page);
  /* The pane, not the page: the same reason epimone_window_add_tab_with_cwd
   * focuses the terminal directly rather than letting GTK pick a descendant,
   * which would otherwise leave focus on a header button. */
  epimone_page_focus_terminal (page);
}

EpimonePage *
epimone_window_find_page_for_session (EpimoneWindow *self, guint64 session_id)
{
  int n;

  g_return_val_if_fail (EPIMONE_IS_WINDOW (self), NULL);
  if (session_id == 0)
    return NULL;

  n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++)
    {
      AdwTabPage *tp = adw_tab_view_get_nth_page (self->tab_view, i);
      EpimonePage *page = EPIMONE_PAGE (adw_tab_page_get_child (tp));

      if (epimone_page_has_session (page, session_id))
        return page;
    }
  return NULL;
}

GArray *
epimone_window_dup_attached_group_ids (EpimoneWindow *self)
{
  GArray *ids = g_array_new (FALSE, FALSE, sizeof (guint64));
  int n;

  g_return_val_if_fail (EPIMONE_IS_WINDOW (self), ids);

  n = adw_tab_view_get_n_pages (self->tab_view);
  for (int i = 0; i < n; i++)
    {
      AdwTabPage *tp = adw_tab_view_get_nth_page (self->tab_view, i);
      EpimonePage *page = EPIMONE_PAGE (adw_tab_page_get_child (tp));
      guint64 gid = epimone_page_get_group_id (page);

      if (gid != 0)
        g_array_append_val (ids, gid);
    }
  return ids;
}

gboolean
epimone_window_overview_visible (EpimoneWindow *self)
{
  g_return_val_if_fail (EPIMONE_IS_WINDOW (self), FALSE);
  if (self->overview == NULL)
    return FALSE;
  return epimone_overview_get_open (EPIMONE_OVERVIEW (self->overview));
}

/* The group id of the tab that is showing: what both directions of the zoom are
 * anchored on when the user did not click a specific card. */
static guint64
epimone_window_active_group (EpimoneWindow *self)
{
  EpimonePage *page = epimone_window_current_page (self);

  return page != NULL ? epimone_page_get_group_id (page) : 0;
}

guint64
epimone_window_get_active_group (EpimoneWindow *self)
{
  g_return_val_if_fail (EPIMONE_IS_WINDOW (self), 0);
  return epimone_window_active_group (self);
}

void
epimone_window_show_overview (EpimoneWindow *self)
{
  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  if (self->overview == NULL)
    return;

  /* No chrome juggling any more: the header and tab bar are part of the content
   * the overview wraps, so they zoom away with it instead of being hidden and
   * restored. The overview draws its own header while it is up.
   *
   * No focus grab here: the overview moves focus onto its own chrome as part of
   * animate_open. (A grab on self->overview would be a silent no-op: the
   * overview is a plain non-focusable widget, and GTK's default grab_focus
   * does not descend to a focusable child, which would leave the terminal
   * focused and eating every key, Escape included.) */
  epimone_overview_animate_open (EPIMONE_OVERVIEW (self->overview),
                                 epimone_window_active_group (self));
}

void
epimone_window_hide_overview (EpimoneWindow *self)
{
  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  if (self->overview == NULL)
    return;

  /* Still the single exit, and the only place that hands focus back; the
   * hand-back happens when the transition finishes, not here. The overview's
   * animation-done callback calls epimone_window_focus_active_terminal, so
   * focus lands on the terminal after the zoom rather than mid-flight. */
  epimone_overview_animate_close (EPIMONE_OVERVIEW (self->overview),
                                  epimone_window_active_group (self));
}

void
epimone_window_toggle_overview (EpimoneWindow *self)
{
  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  if (epimone_window_overview_visible (self))
    epimone_window_hide_overview (self);
  else
    epimone_window_show_overview (self);
}

void
epimone_window_focus_active_terminal (EpimoneWindow *self)
{
  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  epimone_window_focus_current_terminal (self);
}

void
epimone_window_select_page (EpimoneWindow *self, EpimonePage *page)
{
  AdwTabPage *tab_page;

  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  g_return_if_fail (EPIMONE_IS_PAGE (page));

  tab_page = adw_tab_view_get_page (self->tab_view, GTK_WIDGET (page));
  if (tab_page == NULL)
    return;
  adw_tab_view_set_selected_page (self->tab_view, tab_page);
  epimone_page_focus_terminal (page);
}

void
epimone_window_set_active_tab (EpimoneWindow *self, int index)
{
  AdwTabPage *tab_page;
  int n;

  g_return_if_fail (EPIMONE_IS_WINDOW (self));
  n = adw_tab_view_get_n_pages (self->tab_view);
  if (n == 0)
    return;
  if (index < 0)
    index = 0;
  if (index >= n)
    index = n - 1;
  tab_page = adw_tab_view_get_nth_page (self->tab_view, index);
  adw_tab_view_set_selected_page (self->tab_view, tab_page);
}

static void
new_tab_action (GSimpleAction *action,
                GVariant      *parameter,
                gpointer       user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  EpimonePage *current = epimone_window_current_page (self);
  g_autofree char *cwd = NULL;

  /* Inherit the active pane's working directory, matching split behavior and
   * subject to the same preserve-directory policy. Falls back to the daemon
   * default when there is no current page or the policy/cwd resolution yields
   * nothing (dup_cwd_for_spawn returns NULL). */
  if (current != NULL)
    cwd = epimone_page_dup_cwd_for_spawn (current);

  epimone_window_add_tab_with_cwd (self, cwd);
}

static void
split_right_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_split (page, GTK_ORIENTATION_HORIZONTAL);
}

static void
split_down_action (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_split (page, GTK_ORIENTATION_VERTICAL);
}

static void
close_pane_action (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_close_pane (page);
}

static void
zoom_action (GSimpleAction *action,
             GVariant      *parameter,
             gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_toggle_zoom (page);
}

/* Move the focused pane out of its split into a tab of its own.
 *
 * The terminal WIDGET moves, bridge intact: the session is never detached or
 * re-attached (ATTACH would forcibly detach the incumbent and replay the whole
 * ring), so nothing flickers, nothing re-scrolls, and the daemon never
 * notices. The daemon side follows on the next debounced sync: the new page
 * has no group yet, so the sync mints one and GROUP_ADD (an atomic move)
 * pulls the session out of the source group. The source group keeps its other
 * member(s), so it survives under its own id with a healed blob. */
static void
open_in_new_tab_action (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  EpimonePage *page = epimone_window_current_page (self);
  EpimonePage *new_page;
  AdwTabPage *src_tab;
  AdwTabPage *new_tab;
  GtkWidget *leaf;
  int position;

  (void) action;
  (void) parameter;

  /* The menu item is greyed out on a single-pane tab, but the accelerator
   * lands here regardless of that display state, so the same guard applies:
   * one pane has nothing to move out of. */
  if (page == NULL || epimone_page_get_pane_count (page) < 2)
    return;

  /* Read the source tab's position BEFORE the surgery below touches focus. */
  src_tab = adw_tab_view_get_selected_page (self->tab_view);
  position = (src_tab != NULL)
    ? adw_tab_view_get_page_position (self->tab_view, src_tab) + 1
    : -1;

  leaf = epimone_page_take_focused_leaf (page);
  if (leaf == NULL)
    return;

  new_page = EPIMONE_PAGE (epimone_page_new_empty ());
  epimone_page_adopt_leaf (new_page, leaf);
  g_object_unref (leaf);

  /* Right AFTER the source tab, and selected, with focus following the moved
   * pane: the user is acting on this pane, so its new home follows it, the
   * same reason a split focuses the new pane. Appending at the end would tear
   * it away spatially from the tab it came out of. */
  new_tab = epimone_window_attach_page_at (self, new_page, position);
  adw_tab_view_set_selected_page (self->tab_view, new_tab);
  epimone_page_focus_terminal (new_page);
  epimone_layout_schedule_save ();
}

/* Clipboard actions live on the window (not the page) because GtkApplication
 * accelerators dispatch from the window: a widget-class action installed on a
 * descendant page is not reachable from the window's action muxer, so the old
 * page-level term.copy/paste/select-all never fired via Ctrl+Shift+C/V/A. These
 * resolve the current tab's page and act on its focused inner VteTerminal. */
static void
copy_action (GSimpleAction *action,
             GVariant      *parameter,
             gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_copy (page);
}

static void
paste_action (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_paste (page);
}

static void
select_all_action (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_select_all (page);
}

static void
select_none_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_select_none (page);
}

/* Toggle the FOCUSED pane's read-only latch. Stateful so the menu item shows
 * a check mark; the state is only trusted for display, and is re-synced from
 * the focused pane whenever the context menu opens (the latch is per-pane,
 * the action per-window). */
static void
read_only_action (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));
  gboolean read_only;

  (void) parameter;
  if (page == NULL)
    return;

  read_only = !epimone_page_get_read_only (page);
  epimone_page_set_read_only (page, read_only);
  g_simple_action_set_state (action, g_variant_new_boolean (read_only));
}

static void
reset_action (GSimpleAction *action,
              GVariant      *parameter,
              gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  if (page != NULL)
    epimone_page_reset_terminal (page, FALSE);
}

static void
reset_and_clear_action (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));

  /* No confirmation, deliberately: only the VTE's local scrollback view is
   * dropped. The daemon's ring buffer keeps the history (a detach/restore
   * replays it), so nothing is irreversibly lost, and a dialog here would
   * cry wolf next to the kill confirmations, which DO destroy things. */
  if (page != NULL)
    epimone_page_reset_terminal (page, TRUE);
}

/* Set Title: rename the TAB from the pane menu, through the same custom-title
 * mechanism as the overview card menu's Set Title: epimone_page_set_custom_title
 * retitles immediately and the debounced sync persists it in the group blob.
 * The dialog copy matches the overview's rename dialog. */
static void
set_title_response_cb (AdwAlertDialog *dialog, GAsyncResult *result,
                       gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  const char *response = adw_alert_dialog_choose_finish (dialog, result);
  /* The page was captured at open (a ref on the dialog): re-resolving the
   * current page here would rename whatever tab the user switched to while
   * the dialog was up. */
  EpimonePage *page = g_object_get_data (G_OBJECT (dialog), "epi-page");
  GtkWidget *entry;

  if (g_strcmp0 (response, "set") == 0 && EPIMONE_IS_PAGE (page) &&
      (entry = adw_alert_dialog_get_extra_child (dialog)) != NULL)
    {
      g_autofree char *name =
        g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry)));

      g_strstrip (name);
      /* Empty (or all-whitespace) means back to automatic, shell titles. */
      epimone_page_set_custom_title (page, name[0] != '\0' ? name : NULL);
    }

  /* Either way, the keyboard goes back to the terminal, as the kill-pane and
   * close-confirm dialogs do. */
  epimone_window_focus_current_terminal (self);
}

/* ONE Set Title dialog, two entry points: the pane menu's win.set-title (acts
 * on the current tab) and the tab menu's win.tab-set-title (acts on the tab
 * that was right-clicked). Same dialog, same epimone_page_set_custom_title
 * mechanism, same persistence; the page to rename is simply a parameter. */
static void
epimone_window_present_set_title (EpimoneWindow *self, EpimonePage *page)
{
  AdwAlertDialog *dialog;
  GtkWidget *entry;
  const char *shell_title;

  if (page == NULL)
    return;

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new ("Set Title", NULL));
  adw_alert_dialog_set_body (dialog,
                             "Leave empty to use the automatic title.");

  entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry),
                         epimone_page_get_custom_title (page) ?: "");
  /* The automatic title as the placeholder, so what "empty" returns to is
   * visible while choosing; same as the overview's rename dialog. */
  shell_title = epimone_page_get_shell_title (page);
  if (shell_title != NULL)
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry), shell_title);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  adw_alert_dialog_set_extra_child (dialog, entry);

  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel",
                                  "set", "_Set", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "set",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "set");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  g_object_set_data_full (G_OBJECT (dialog), "epi-page",
                          g_object_ref (page), g_object_unref);

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) set_title_response_cb, self);
}

static void
set_title_action (GSimpleAction *action,
                  GVariant      *parameter,
                  gpointer       user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);

  (void) action;
  (void) parameter;
  epimone_window_present_set_title (self, epimone_window_current_page (self));
}

/* ------------------------------------------------------------------ *
 * the tab context menu (AdwTabView::setup-menu)
 *
 * Every action here acts on epimone_window_tab_menu_page(): the tab that was
 * right-clicked while the menu is up, and the selected tab otherwise, which
 * is what makes the same actions usable as accelerators, where there is no
 * click to take a target from.
 * ------------------------------------------------------------------ */

static AdwTabPage *
epimone_window_tab_menu_page (EpimoneWindow *self)
{
  if (self->menu_page != NULL)
    return self->menu_page;
  return adw_tab_view_get_selected_page (self->tab_view);
}

static EpimonePage *
epimone_window_tab_menu_epi_page (EpimoneWindow *self)
{
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);
  GtkWidget *child = tab_page != NULL ? adw_tab_page_get_child (tab_page) : NULL;

  return EPIMONE_IS_PAGE (child) ? EPIMONE_PAGE (child) : NULL;
}

/* Reordering does NOT wrap: adw_tab_view_reorder_backward/forward return FALSE
 * at the ends and leave the order alone (measured, libadwaita 1.9). No custom
 * wrap logic is added, so Move Left on the leftmost tab is simply a no-op. */
static void
tab_move_left_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);

  (void) action; (void) param;
  if (tab_page != NULL)
    adw_tab_view_reorder_backward (self->tab_view, tab_page);
}

static void
tab_move_right_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);

  (void) action; (void) param;
  if (tab_page != NULL)
    adw_tab_view_reorder_forward (self->tab_view, tab_page);
}

/* The live-page transfer itself lives in epimone_window_move_page_to_new_window,
 * which the overview's card menu shares. */
static void
tab_move_to_new_window_action (GSimpleAction *action, GVariant *param,
                               gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  EpimonePage *page = epimone_window_tab_menu_epi_page (self);

  (void) action; (void) param;
  if (page != NULL)
    epimone_window_move_page_to_new_window (page);
}

static void
tab_pin_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);

  (void) action; (void) param;
  if (tab_page != NULL)
    adw_tab_view_set_page_pinned (self->tab_view, tab_page, TRUE);
}

static void
tab_unpin_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);

  (void) action; (void) param;
  if (tab_page != NULL)
    adw_tab_view_set_page_pinned (self->tab_view, tab_page, FALSE);
}

static void
tab_set_title_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);

  (void) action; (void) param;
  epimone_window_present_set_title (self,
                                    epimone_window_tab_menu_epi_page (self));
}

/* Close DETACHES, as everywhere else in Epimone: the tab goes, its sessions
 * keep running and come back through the overview. Nothing dies, so neither
 * of these confirms. */
static void
tab_close_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);

  (void) action; (void) param;
  if (tab_page != NULL)
    adw_tab_view_close_page (self->tab_view, tab_page);
}

static void
tab_close_others_action (GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  AdwTabPage *tab_page = epimone_window_tab_menu_page (self);

  (void) action; (void) param;
  /* libadwaita spares PINNED pages here (measured), which is the behaviour a
   * pin is for; no custom logic is added to override it. */
  if (tab_page != NULL)
    adw_tab_view_close_other_pages (self->tab_view, tab_page);
}

static GtkWidget *epimone_window_find_tab_menu_popover (GtkWidget  *widget,
                                                        GMenuModel *model);

/* See the call site in epimone_window_setup_menu_cb for why this is deferred. */
static gboolean
epimone_window_hook_tab_popover_cb (gpointer user_data)
{
  EpimoneWindow *self = user_data;
  GtkWidget *popover;

  self->menu_hook_idle = 0;
  popover = epimone_window_find_tab_menu_popover (GTK_WIDGET (self),
                                                  G_MENU_MODEL (self->tab_menu));
  if (popover == NULL)
    return G_SOURCE_REMOVE;   /* not built yet; the next opening tries again */

  /* Always fix the labels of the menu that is showing right now. */
  epimone_shortcuts_fix_accel_labels (popover);

  if (g_object_get_data (G_OBJECT (popover), "epi-hooked") == NULL)
    {
      g_object_set_data (G_OBJECT (popover), "epi-hooked", GINT_TO_POINTER (1));
      g_signal_connect (popover, "map",
                        G_CALLBACK (epimone_shortcuts_fix_accel_labels_cb),
                        NULL);
      g_signal_connect_swapped (popover, "closed",
                                G_CALLBACK (epimone_window_focus_current_terminal),
                                self);
    }
  return G_SOURCE_REMOVE;
}

static gboolean
epimone_window_clear_menu_page_cb (gpointer user_data)
{
  EpimoneWindow *self = user_data;

  self->menu_page_clear_idle = 0;
  g_clear_weak_pointer (&self->menu_page);
  return G_SOURCE_REMOVE;
}

/* The tab menu is about to be shown for @page (or has closed, @page NULL).
 * Capture the page every item acts on, and set the Pin/Unpin pair's enabled
 * state; each item is hidden-when="action-disabled", so exactly one shows. */
static void
epimone_window_setup_menu_cb (AdwTabView *view,
                              AdwTabPage *page,
                              gpointer    user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  GAction *pin, *unpin, *others, *to_window;
  gboolean pinned;

  if (page == NULL)
    {
      /* Menu closed. The capture is dropped from an idle, so an item
       * activated by this same click still sees it (see the struct note). */
      if (self->menu_page_clear_idle == 0)
        self->menu_page_clear_idle =
          g_idle_add (epimone_window_clear_menu_page_cb, self);
      return;
    }

  g_clear_handle_id (&self->menu_page_clear_idle, g_source_remove);
  g_set_weak_pointer (&self->menu_page, page);

  /* Hook the popover AdwTabBar shows for this menu, from an IDLE.
   *
   * It does not exist yet at setup-menu time (measured: searching the window
   * tree here finds nothing), because AdwTabBar builds the popover as part of
   * showing it, after this signal. An idle runs once it does exist (and is
   * mapped), which is why the fixer is also called ONCE DIRECTLY there rather
   * than only being connected to future maps: the first showing has already
   * mapped by then. Both hooks are the ones every other popover in the app
   * gets: the Ctrl-first accel-label fix, and focus back to the terminal on
   * close, without which GTK's focus-to-NULL-then-first-focusable fallback
   * lands on the header "+" and the next Return opens a tab. */
  if (self->menu_hook_idle == 0)
    self->menu_hook_idle = g_idle_add (epimone_window_hook_tab_popover_cb, self);

  pinned = adw_tab_page_get_pinned (page);
  pin = g_action_map_lookup_action (G_ACTION_MAP (self), "tab-pin");
  unpin = g_action_map_lookup_action (G_ACTION_MAP (self), "tab-unpin");
  if (G_IS_SIMPLE_ACTION (pin))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (pin), !pinned);
  if (G_IS_SIMPLE_ACTION (unpin))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (unpin), pinned);

  /* Nothing to close beside a lone tab, and nowhere to move it to. */
  others = g_action_map_lookup_action (G_ACTION_MAP (self), "tab-close-others");
  to_window = g_action_map_lookup_action (G_ACTION_MAP (self),
                                          "tab-move-to-new-window");
  if (G_IS_SIMPLE_ACTION (others))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (others),
                                 adw_tab_view_get_n_pages (view) > 1);
  if (G_IS_SIMPLE_ACTION (to_window))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (to_window),
                                 adw_tab_view_get_n_pages (view) > 1);
}

/* Build the tab menu model. No "accel" attributes: GtkPopoverMenu discovers
 * the application's accelerator for each item's action by itself (verified),
 * so a rebind is reflected with no work here, and the map-time fixer below
 * puts those labels into the conventional Ctrl-first order. */
static GMenu *
epimone_window_build_tab_menu (void)
{
  GMenu *menu = g_menu_new ();
  GMenu *section = g_menu_new ();
  GMenuItem *item;

  g_menu_append (section, "Move Left", "win.tab-move-left");
  g_menu_append (section, "Move Right", "win.tab-move-right");
  g_menu_append (section, "Move to New Window", "win.tab-move-to-new-window");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);

  /* One visible item, two actions: whichever is disabled is hidden, so this
   * reads as a single entry that flips between Pin Tab and Unpin Tab. */
  section = g_menu_new ();
  item = g_menu_item_new ("Pin Tab", "win.tab-pin");
  g_menu_item_set_attribute (item, "hidden-when", "s", "action-disabled");
  g_menu_append_item (section, item);
  g_object_unref (item);
  item = g_menu_item_new ("Unpin Tab", "win.tab-unpin");
  g_menu_item_set_attribute (item, "hidden-when", "s", "action-disabled");
  g_menu_append_item (section, item);
  g_object_unref (item);
  g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);

  section = g_menu_new ();
  g_menu_append (section, "Set Title", "win.tab-set-title");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);

  section = g_menu_new ();
  /* "Detach", not "Close": both of these only detach; the sessions keep
   * running and reappear in the overview. See the pane menu's note in
   * epimone-page.c for why the labels say detach while the action names keep
   * their close-* spelling. */
  g_menu_append (section, "Detach Other Tabs", "win.tab-close-others");
  g_menu_append (section, "Detach Tab", "win.tab-close");
  g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
  g_object_unref (section);

  return menu;
}

/* Find the popover AdwTabBar built for the tab menu, by matching the model
 * rather than by guessing at the widget tree's shape. */
static GtkWidget *
epimone_window_find_tab_menu_popover (GtkWidget *widget, GMenuModel *model)
{
  if (GTK_IS_POPOVER_MENU (widget) &&
      gtk_popover_menu_get_menu_model (GTK_POPOVER_MENU (widget)) == model)
    return widget;

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *found = epimone_window_find_tab_menu_popover (child, model);

      if (found != NULL)
        return found;
    }
  return NULL;
}

static void
new_window_action (GSimpleAction *action,
                   GVariant      *parameter,
                   gpointer       user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GtkWidget *window;

  if (app == NULL)
    return;

  window = epimone_window_new (ADW_APPLICATION (app));
  epimone_window_add_tab (EPIMONE_WINDOW (window));
  gtk_window_present (GTK_WINDOW (window));
}

static void
preferences_action (GSimpleAction *action,
                    GVariant      *parameter,
                    gpointer       user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  GtkApplication *app = gtk_window_get_application (GTK_WINDOW (self));
  GtkWidget *settings;
  GList *l;

  /* One settings window at a time: raise the existing one (whatever window
   * it was opened from) instead of stacking a second. Settings live in the
   * application's window list because epimone_settings_new set that up. */
  if (app != NULL)
    for (l = gtk_application_get_windows (app); l != NULL; l = l->next)
      if (EPIMONE_IS_SETTINGS (l->data))
        {
          gtk_window_present (GTK_WINDOW (l->data));
          return;
        }

  settings = epimone_settings_new (GTK_WINDOW (self));
  gtk_window_present (GTK_WINDOW (settings));
}

static void
focus_direction_action (GSimpleAction *action,
                        GVariant      *parameter,
                        gpointer       user_data)
{
  EpimonePage *page = epimone_window_current_page (EPIMONE_WINDOW (user_data));
  const char *dir = g_variant_get_string (parameter, NULL);
  GtkDirectionType direction;

  if (page == NULL)
    return;

  if (g_strcmp0 (dir, "left") == 0)
    direction = GTK_DIR_LEFT;
  else if (g_strcmp0 (dir, "right") == 0)
    direction = GTK_DIR_RIGHT;
  else if (g_strcmp0 (dir, "up") == 0)
    direction = GTK_DIR_UP;
  else if (g_strcmp0 (dir, "down") == 0)
    direction = GTK_DIR_DOWN;
  else
    return;

  epimone_page_focus_direction (page, direction);
}

/* Closing any window persists the full layout (all open windows) once, then
 * freezes further saves so the closing cascade doesn't shrink the file. The
 * sessions detach (their sockets close) but keep running for next launch. */
/* "Close" chosen in the confirmation: mark the window so the close-request
 * handler stops asking, then close it for real. */
static void
epimone_window_confirm_close_cb (GObject      *source,
                                 GAsyncResult *result,
                                 gpointer      user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  const char *response = adw_alert_dialog_choose_finish (ADW_ALERT_DIALOG (source),
                                                         result);

  if (g_strcmp0 (response, "close") != 0)
    {
      /* Cancelled or dismissed: the window stays open, so put focus back on the
       * terminal instead of wherever the dialog's buttons left it. */
      epimone_window_focus_current_terminal (self);
      return;
    }

  self->closing = TRUE;
  gtk_window_close (GTK_WINDOW (self));
}

/* Closing a window detaches its sessions rather than killing them, but it still
 * tears down every tab at once, so ask first when there is more than one.
 *
 * Scope note: the criterion is "more than one tab", not "has running
 * processes". Telling an idle shell apart from a running job means inspecting
 * the foreground process group of each pane's PTY, which is a bigger piece of
 * work; the tab count is the part that is both cheap and reliably useful.
 * TODO: refine to "running processes" by checking each pane's foreground pgid
 * against its shell pid. */
/* Total terminal panes across every tab in this window. A window holding one
 * tab that has been split several ways is just as much of a "you are about to
 * throw away several things" situation as several tabs, so the confirmation
 * keys off both. */
static guint
epimone_window_count_panes (EpimoneWindow *self)
{
  int n_pages = adw_tab_view_get_n_pages (self->tab_view);
  guint panes = 0;

  for (int i = 0; i < n_pages; i++)
    {
      AdwTabPage *tab_page = adw_tab_view_get_nth_page (self->tab_view, i);
      GtkWidget *child = adw_tab_page_get_child (tab_page);

      if (EPIMONE_IS_PAGE (child))
        panes += epimone_page_get_pane_count (EPIMONE_PAGE (child));
    }

  return panes;
}

static gboolean
epimone_window_close_request_cb (GtkWindow *window, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (window);
  AdwDialog *dialog;
  int n_pages;
  guint n_panes;
  gboolean ask;

  (void) user_data;

  n_pages = adw_tab_view_get_n_pages (self->tab_view);
  n_panes = epimone_window_count_panes (self);
  ask = epimone_window_prefs.confirm_close && !self->closing &&
        (n_pages > 1 || n_panes > 1);

  if (ask)
    {
      dialog = adw_alert_dialog_new ("Close this window?", NULL);
      if (n_pages > 1)
        adw_alert_dialog_format_body (
          ADW_ALERT_DIALOG (dialog),
          "%d tabs are open. They will be closed together.", n_pages);
      else
        adw_alert_dialog_format_body (
          ADW_ALERT_DIALOG (dialog),
          "This tab is split into %u panes. They will be closed together.",
          n_panes);
      adw_alert_dialog_add_responses (ADW_ALERT_DIALOG (dialog),
                                      "cancel", "_Cancel",
                                      "close", "_Close",
                                      NULL);
      adw_alert_dialog_set_response_appearance (ADW_ALERT_DIALOG (dialog),
                                                "close",
                                                ADW_RESPONSE_DESTRUCTIVE);
      adw_alert_dialog_set_default_response (ADW_ALERT_DIALOG (dialog), "cancel");
      adw_alert_dialog_set_close_response (ADW_ALERT_DIALOG (dialog), "cancel");
      adw_alert_dialog_choose (ADW_ALERT_DIALOG (dialog), GTK_WIDGET (self),
                               NULL, epimone_window_confirm_close_cb, self);

      return GDK_EVENT_STOP;   /* hold the window open until answered */
    }

  /* Past the point of no return: freeze layout saves so the closing cascade
   * doesn't shrink the file. */
  epimone_layout_begin_shutdown ();
  return GDK_EVENT_PROPAGATE;
}

/* Rebuild a closed tab from its daemon group. This is the ONE restore path: the
 * overview's cards activate it, and it stays reachable over the exported
 * org.gtk.Actions interface, which is how it is tested without clicking:
 *
 *   gdbus call --session --dest org.felix.Epimone \
 *     --object-path /org/felix/Epimone/window/1 \
 *     --method org.gtk.Actions.Activate restore-group '[<uint64 2>]' '{}'
 *
 * Deliberately absent from the shortcuts table and the menus: a group id is not
 * something a keybinding can supply. */
static void
restore_group_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  GError *err = NULL;
  guint64 gid;

  (void) action;

  if (param == NULL || !g_variant_is_of_type (param, G_VARIANT_TYPE_UINT64))
    {
      g_warning ("epimone: win.restore-group needs a uint64 group id");
      return;
    }
  gid = g_variant_get_uint64 (param);

  if (!epimone_layout_restore_group (self, gid, &err))
    {
      g_warning ("epimone: restore-group %" G_GUINT64_FORMAT " failed: %s",
                 gid, err != NULL ? err->message : "unknown error");
      g_clear_error (&err);
    }
}

static void
overview_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  (void) action;
  (void) param;
  epimone_window_toggle_overview (EPIMONE_WINDOW (user_data));
}

/* Killing is the only thing in the UI that ends a session; closing a pane, tab or
 * window merely detaches. So it confirms, and it says plainly what the difference
 * is: someone reaching for "close" and finding "kill" would lose work. */
static void
kill_pane_response_cb (AdwAlertDialog *dialog, GAsyncResult *result,
                       gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  const char *response = adw_alert_dialog_choose_finish (dialog, result);
  guint64 sid;

  if (g_strcmp0 (response, "kill") != 0)
    {
      /* Cancelled: focus goes back to the terminal, exactly as the close-confirm
       * cancel path does. */
      epimone_window_focus_current_terminal (self);
      return;
    }

  sid = (guint64) GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog), "epi-sid"));
  if (sid != 0)
    {
      GError *err = NULL;
      if (!epimone_client_kill_session (sid, &err))
        {
          g_warning ("epimone: could not kill session %" G_GUINT64_FORMAT ": %s",
                     sid, err != NULL ? err->message : "unknown error");
          g_clear_error (&err);
        }
    }
  epimone_window_focus_current_terminal (self);
}

static void
kill_pane_action (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneWindow *self = EPIMONE_WINDOW (user_data);
  EpimonePage *page = epimone_window_current_page (self);
  AdwAlertDialog *dialog;
  guint64 sid;

  (void) action;
  (void) param;

  if (page == NULL)
    return;
  sid = epimone_page_get_focused_session (page);
  if (sid == 0)
    return;

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new ("Kill this pane?", NULL));
  adw_alert_dialog_set_body (dialog,
                             "End the program running in this pane?\n\nDetaching "
                             "a pane leaves the program running. Killing stops "
                             "it.");
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel", "kill", "_Kill", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "kill", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  g_object_set_data (G_OBJECT (dialog), "epi-sid", GUINT_TO_POINTER ((guint) sid));

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) kill_pane_response_cb, self);
}

static const GActionEntry epimone_window_actions[] = {
  { .name = "new-tab",     .activate = new_tab_action },
  { .name = "split-right", .activate = split_right_action },
  { .name = "split-down",  .activate = split_down_action },
  { .name = "close-pane",  .activate = close_pane_action },
  { .name = "zoom",        .activate = zoom_action },
  { .name = "open-in-new-tab", .activate = open_in_new_tab_action },
  { .name = "new-window",  .activate = new_window_action },
  { .name = "copy",        .activate = copy_action },
  { .name = "paste",       .activate = paste_action },
  { .name = "select-all",  .activate = select_all_action },
  { .name = "select-none", .activate = select_none_action },
  { .name = "read-only",   .activate = read_only_action, .state = "false" },
  { .name = "reset",       .activate = reset_action },
  { .name = "reset-and-clear", .activate = reset_and_clear_action },
  { .name = "set-title",   .activate = set_title_action },
  /* Tab context menu. These act on the right-clicked tab while that menu is
   * up, and on the selected tab otherwise (so the accelerators work). */
  { .name = "tab-move-left",  .activate = tab_move_left_action },
  { .name = "tab-move-right", .activate = tab_move_right_action },
  { .name = "tab-move-to-new-window", .activate = tab_move_to_new_window_action },
  { .name = "tab-pin",     .activate = tab_pin_action },
  { .name = "tab-unpin",   .activate = tab_unpin_action },
  { .name = "tab-set-title", .activate = tab_set_title_action },
  { .name = "tab-close",   .activate = tab_close_action },
  { .name = "tab-close-others", .activate = tab_close_others_action },
  { .name = "preferences", .activate = preferences_action },
  { .name = "focus",       .activate = focus_direction_action, .parameter_type = "s" },
  { .name = "overview",    .activate = overview_action },
  { .name = "kill-pane",   .activate = kill_pane_action },
  { .name = "restore-group", .activate = restore_group_action, .parameter_type = "t" },
};




static void
epimone_window_init (EpimoneWindow *self)
{
  GtkWidget *toolbar_view;
  GtkWidget *header;
  GtkWidget *menu_button;
  GtkWidget *new_tab_button;
  GtkWidget *overview_button;
  GMenu *menu;
  AdwTabBar *tab_bar;

  gtk_window_set_title (GTK_WINDOW (self), "Epimone");
  /* No pixel default size on purpose: the window takes its natural size from
   * the first terminal's requested grid (default-columns x default-rows, set
   * in epimone_page_make_vte), which is how the General preference for default
   * size takes effect. Callers add a tab before presenting, so the request is
   * already in place by then. */

  g_action_map_add_action_entries (G_ACTION_MAP (self),
                                    epimone_window_actions,
                                    G_N_ELEMENTS (epimone_window_actions),
                                    self);

  toolbar_view = adw_toolbar_view_new ();
  /* RAISED is kept not for its shadow (neutralised in main.c's CSS, because
   * its unblurred `0 1px` layer drew a hard line across the top of the
   * terminal) but because raised gives the whole top-bar revealer one flat
   * headerbar_bg fill, so the header and tab bar read as a single block that
   * blends into the body. ADW_TOOLBAR_FLAT would leave the revealer
   * transparent and additionally make libadwaita add `.undershoot-top`,
   * whose inset hairline is the very seam being removed. */
  adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (toolbar_view),
                                      ADW_TOOLBAR_RAISED);
  header = adw_header_bar_new ();
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view), header);

  /* Header layout: new-tab button at the start, a two-line AdwWindowTitle in
   * the centre, the primary menu at the end just inside the window controls. */

  /* Start: new tab. Drives the existing win.new-tab action, which already
   * inherits the active pane's directory. focus-on-click off so clicking it
   * does not steal focus from the terminal. */
  new_tab_button = gtk_button_new_from_icon_name ("tab-new-symbolic");
  gtk_widget_set_tooltip_text (new_tab_button, "New Tab");
  gtk_button_set_has_frame (GTK_BUTTON (new_tab_button), FALSE);
  gtk_widget_set_focus_on_click (new_tab_button, FALSE);
  /* Out of the keyboard focus chain entirely: it is the first focusable
   * widget in the window, so GTK's focus fallback (a popover closing, a
   * focused widget going away) would land here, and GtkWindow turns Return
   * into "activate the focused widget", making the next Return open a tab.
   * can-focus, not focusable: it also keeps focus out of the button's
   * descendants. The accelerator and clicking still work. */
  gtk_widget_set_can_focus (new_tab_button, FALSE);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (new_tab_button), "win.new-tab");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), new_tab_button);

  /* Start, next to it: the session overview. Same focus treatment as the
   * new-tab button, and for the same reason: it must never be where focus
   * falls back to, or Return after the overview closes would re-open the
   * overview. */
  overview_button = gtk_button_new_from_icon_name ("view-grid-symbolic");
  gtk_widget_set_tooltip_text (overview_button, "All Sessions");
  gtk_button_set_has_frame (GTK_BUTTON (overview_button), FALSE);
  gtk_widget_set_focus_on_click (overview_button, FALSE);
  gtk_widget_set_can_focus (overview_button, FALSE);
  gtk_actionable_set_action_name (GTK_ACTIONABLE (overview_button), "win.overview");
  adw_header_bar_pack_start (ADW_HEADER_BAR (header), overview_button);

  /* Centre: the active tab's title over a dimmed cwd. AdwWindowTitle handles
   * the two-line styling and ellipsizing; contents are bound in
   * epimone_window_rebind_title(). */

  self->window_title = ADW_WINDOW_TITLE (adw_window_title_new ("Epimone", NULL));
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (header),
                                   GTK_WIDGET (self->window_title));
  g_signal_connect (self->window_title, "notify::title",
                    G_CALLBACK (epimone_window_notify_header_title_cb), self);

  /* End: primary (hamburger) menu. Packed end, so libadwaita keeps it inside
   * the window controls rather than beyond them. */
  menu = g_menu_new ();
  g_menu_append (menu, "New _Tab", "win.new-tab");
  g_menu_append (menu, "_New Window", "win.new-window");
  g_menu_append (menu, "_Preferences", "win.preferences");
  menu_button = gtk_menu_button_new ();
  gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button), "open-menu-symbolic");
  gtk_widget_set_tooltip_text (menu_button, "Main Menu");
  gtk_widget_set_focus_on_click (menu_button, FALSE);
  gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button), G_MENU_MODEL (menu));
  adw_header_bar_pack_end (ADW_HEADER_BAR (header), menu_button);
  g_object_unref (menu);

  /* Same treatment as the new-tab button, but applied to the GtkToggleButton
   * GtkMenuButton wraps: the toggle button is the focusable part (the
   * GtkMenuButton itself is not), and it is what focus ended up on after the
   * menu closed. can-focus on the GtkMenuButton would also cover its popover,
   * whose items must stay keyboard-navigable, so only the button is excluded. */
  {
    GtkWidget *toggle = gtk_widget_get_first_child (menu_button);

    if (GTK_IS_TOGGLE_BUTTON (toggle))
      gtk_widget_set_can_focus (toggle, FALSE);
  }

  /* And when the menu closes, hand focus back to the terminal rather than
   * letting GTK pick (see epimone_page_focus_terminal). */
  {
    GtkPopover *popover = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu_button));

    if (popover != NULL)
      {
        g_signal_connect_swapped (popover, "closed",
                                  G_CALLBACK (epimone_window_focus_current_terminal),
                                  self);
        /* The items carry no accel attributes, but GtkPopoverMenu
         * auto-discovers the application accels for their actions and
         * renders them Shift-first, and builds the items lazily, so the fix
         * runs on map, just before each showing. */
        g_signal_connect (popover, "map",
                          G_CALLBACK (epimone_shortcuts_fix_accel_labels_cb),
                          NULL);
      }
  }

  self->tab_view = ADW_TAB_VIEW (adw_tab_view_new ());

  tab_bar = ADW_TAB_BAR (adw_tab_bar_new ());
  adw_tab_bar_set_view (tab_bar, self->tab_view);

  /* Right-click menu on a tab. AdwTabView owns the model and emits setup-menu
   * with the page it was opened on just before showing it. */
  self->tab_menu = epimone_window_build_tab_menu ();
  adw_tab_view_set_menu_model (self->tab_view, G_MENU_MODEL (self->tab_menu));
  g_signal_connect (self->tab_view, "setup-menu",
                    G_CALLBACK (epimone_window_setup_menu_cb), self);
  adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (toolbar_view),
                                GTK_WIDGET (tab_bar));

  /* Bottom bars get the same raised treatment as the top, so the tab bar reads
   * the same either way once the position preference moves it. */
  adw_toolbar_view_set_bottom_bar_style (ADW_TOOLBAR_VIEW (toolbar_view),
                                         ADW_TOOLBAR_RAISED);

  self->toolbar_view = ADW_TOOLBAR_VIEW (toolbar_view);
  self->tab_bar = tab_bar;
  self->tab_bar_at_bottom = FALSE;   /* built into the top slot above */

  /* The tab view is the toolbar view's content directly. The OVERVIEW then wraps
   * that whole toolbar view and becomes the window's content, so the header, tab
   * bar and terminals are all one thing that can be zoomed into a card. The tab
   * view is handed over separately because its rectangle, not the window's, is
   * what a card depicts, and that is what the zoom interpolates against. */
  adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (toolbar_view),
                                GTK_WIDGET (self->tab_view));

  self->overview = epimone_overview_new (self);
  epimone_overview_set_content (EPIMONE_OVERVIEW (self->overview), toolbar_view,
                                GTK_WIDGET (self->tab_view));
  adw_application_window_set_content (ADW_APPLICATION_WINDOW (self),
                                      self->overview);

  /* Join the registry and take the current chrome preferences, so a window
   * opened after the user changed them starts out matching. */
  epimone_all_windows = g_slist_prepend (epimone_all_windows, self);
  g_signal_connect (self, "destroy",
                    G_CALLBACK (epimone_window_unregister_cb), NULL);
  epimone_apply_window_prefs_to (self);

  g_signal_connect (self->tab_view, "notify::n-pages",
                    G_CALLBACK (epimone_window_notify_n_pages_cb), self);
  g_signal_connect (self->tab_view, "notify::selected-page",
                    G_CALLBACK (epimone_window_notify_selected_cb), self);
  g_signal_connect (self, "close-request",
                    G_CALLBACK (epimone_window_close_request_cb), NULL);

  /* Start on the no-tabs title; notify::selected-page rebinds it as soon as the
   * caller adds or adopts the first page. */
  epimone_window_rebind_title (self);


  /* No tab is added here: callers add a fresh tab (new session) or adopt
   * restored pages. */
}

static void
epimone_window_dispose (GObject *object)
{
  EpimoneWindow *self = EPIMONE_WINDOW (object);

  /* Before the header widgets go, so the bindings never outlive their target. */
  epimone_window_clear_binding (&self->title_binding);
  epimone_window_clear_binding (&self->subtitle_binding);

  g_clear_handle_id (&self->menu_page_clear_idle, g_source_remove);
  g_clear_handle_id (&self->menu_hook_idle, g_source_remove);
  g_clear_weak_pointer (&self->menu_page);
  g_clear_object (&self->tab_menu);

  G_OBJECT_CLASS (epimone_window_parent_class)->dispose (object);
}

static void
epimone_window_class_init (EpimoneWindowClass *klass)
{
  G_OBJECT_CLASS (klass)->dispose = epimone_window_dispose;
}

/* The accelerator table lives in GSettings (epimone-shortcuts.c), one key
 * per action, so the Keyboard preferences page can rebind any of them. Binding
 * here also subscribes to changes, which is what makes a rebind take effect in
 * every open window without a restart. The actions themselves are ordinary
 * window-level win.* actions. */
static void
epimone_window_setup_accels (AdwApplication *app)
{
  epimone_shortcuts_bind_application (GTK_APPLICATION (app));
}

GtkWidget *
epimone_window_new (AdwApplication *app)
{
  epimone_window_setup_accels (app);

  return g_object_new (EPIMONE_TYPE_WINDOW,
                       "application", app,
                       NULL);
}
