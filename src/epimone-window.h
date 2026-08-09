#pragma once
#include <adwaita.h>
#include "epimone-page.h"

G_BEGIN_DECLS

#define EPIMONE_TYPE_WINDOW (epimone_window_get_type())
G_DECLARE_FINAL_TYPE (EpimoneWindow, epimone_window, EPIMONE, WINDOW, AdwApplicationWindow)

/* Create an empty window (no tabs). Callers add tabs (fresh start / restore). */
GtkWidget  *epimone_window_new (AdwApplication *app);

/* Append a fresh tab backed by a new daemon session, and select it. */
void        epimone_window_add_tab (EpimoneWindow *self);

/* Append a pre-built restored page (does not change the selection). */
AdwTabPage *epimone_window_adopt_page (EpimoneWindow *self, EpimonePage *page);

/* Append a pre-built page, select its tab, and put focus on its terminal.
 * The AdwTabPage-free counterpart of adopt_page + set_active_tab, so callers
 * rebuilding a tab do not have to touch AdwTabPage, and so the focus step is
 * in one place rather than left to each caller. */
void        epimone_window_adopt_and_select_page (EpimoneWindow *self,
                                                  EpimonePage   *page);

/* The page in this window currently showing @session_id, or NULL. Used to notice
 * that a group is already on screen before rebuilding it. */
EpimonePage *epimone_window_find_page_for_session (EpimoneWindow *self,
                                                   guint64        session_id);

/* The group ids of every tab attached in THIS window, derived live from the tab
 * view (no cached mapping, so "Move to New Window" is automatically correct).
 * Newly allocated; free with g_array_unref. Group id 0 (a page not yet enrolled)
 * is skipped. */
GArray     *epimone_window_dup_attached_group_ids (EpimoneWindow *self);

/* The group id of the tab currently showing, or 0. The overview rings this
 * card with the system accent. */
guint64     epimone_window_get_active_group (EpimoneWindow *self);

/* Select the tab holding @page and focus its terminal. */
void        epimone_window_select_page (EpimoneWindow *self, EpimonePage *page);

/* Act on the tab holding @page, wherever it is: both resolve the window from
 * @page's own widget tree, so a page that has moved between windows is handled
 * by the one showing it. The tab context menu and the overview's card menu
 * share these, so each behaviour has exactly one implementation.
 *
 * close_page DETACHES: the sessions keep running and the group stays
 * restorable, as with every other close in Epimone. move_page_to_new_window
 * TRANSFERS the live page, so no session is detached or re-attached; it does
 * nothing when the window has only that one tab. */
void        epimone_window_close_page (EpimonePage *page);
void        epimone_window_move_page_to_new_window (EpimonePage *page);

/* ------------------------------------------------------------------ *
 * The session overview lives in this window's content area behind a stack, so the
 * window switches views rather than having a panel appear over it.
 * ------------------------------------------------------------------ */

void        epimone_window_show_overview   (EpimoneWindow *self);

/* THE single exit from the overview. The only place that swaps the stack back and
 * restores terminal focus; every route out of the overview calls this. */
void        epimone_window_hide_overview   (EpimoneWindow *self);

void        epimone_window_toggle_overview (EpimoneWindow *self);
gboolean    epimone_window_overview_visible (EpimoneWindow *self);

/* Put keyboard focus on the selected tab's active pane. Called from anywhere
 * that takes focus away and has to give it back; the overview closing, in
 * particular, where leaving it to GTK strands focus on a header button. */
void        epimone_window_focus_active_terminal (EpimoneWindow *self);

AdwTabView *epimone_window_get_tab_view (EpimoneWindow *self);
void        epimone_window_set_active_tab (EpimoneWindow *self, int index);

/* When the tab bar is shown. MULTIPLE is AdwTabBar's own autohide behaviour
 * (revealed once a second tab exists) and is the default. */
typedef enum
{
  EPIMONE_TAB_BAR_ALWAYS,
  EPIMONE_TAB_BAR_MULTIPLE,
  EPIMONE_TAB_BAR_NEVER,
} EpimoneTabBarPolicy;

/* Map the GSettings "tab-bar-policy" string to the enum; falls back to
 * MULTIPLE for anything unrecognized. */
EpimoneTabBarPolicy epimone_tab_bar_policy_from_id (const char *id);

/* Window-chrome preferences. These mirror the epimone_terminals_set_* family:
 * each updates shared state, applies to every open window immediately, and is
 * inherited by windows opened later. */
void epimone_windows_set_tab_bar_policy   (EpimoneTabBarPolicy policy);
void epimone_windows_set_tab_bar_at_bottom (gboolean bottom);
void epimone_windows_set_confirm_close     (gboolean enabled);

G_END_DECLS
