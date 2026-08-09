#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

/* Forward declaration rather than including epimone-window.h, which would pull in
 * epimone-page.h and make the include graph circular. Identical to the typedef
 * G_DECLARE_FINAL_TYPE emits there, which C11 permits to be repeated. */
typedef struct _EpimoneWindow EpimoneWindow;

/* The session overview: one card per daemon group, which is to say one card per
 * tab, including tabs that have been closed, whose sessions are still running
 * detached. Clicking a card brings that tab back.
 *
 * Arrangement: a grid of scaled cards with the title under each and a close
 * control inset in each thumbnail, a New Tab button bottom centre, and an
 * overview-mode header bar carrying the search toggle and a live session
 * count. libadwaita's AdwTabOverview provides this arrangement but cannot be
 * reused here: it renders the pages of a live AdwTabView, and the cards that
 * matter most in Epimone belong to tabs that have no widgets at all. So the
 * arrangement is rebuilt over a flow box of cards, with AdwTabOverview's
 * card-sizing formula transcribed (see overview_card_thumb_width in the .c:
 * cards grow to fill the grid instead of sitting at a fixed size).
 *
 * The overview IS the window's content: it wraps the app's whole toolbar view
 * (header, tab bar, tab view) and draws its own header bar while it is up, so
 * the two headers trade places continuously during the zoom.
 *
 * Every way out routes through epimone_window_hide_overview(), which is the single
 * place that swaps the stack back and restores terminal focus. That single-exit
 * property is deliberate: scattering focus grabs across exit paths is how focus
 * ends up stranded on a header button, which turns the next Return into a
 * button press. */

#define EPIMONE_TYPE_OVERVIEW (epimone_overview_get_type ())
G_DECLARE_FINAL_TYPE (EpimoneOverview, epimone_overview, EPIMONE, OVERVIEW, GtkWidget)

/* Create the overview widget for @win. It becomes the window's content and wraps
 * the app content handed to it by epimone_overview_set_content. */
GtkWidget *epimone_overview_new (EpimoneWindow *win);

/* Hand the overview the app content it wraps. @child is what shows when the
 * overview is closed (the window's whole toolbar view: header, tab bar, tab
 * view), and @view is the tab view inside it: the widget whose rectangle a card
 * depicts, and therefore what the zoom interpolates to and from.
 *
 * The overview OWNS the content rather than sitting beside it in a stack. That
 * inversion is what makes the transition possible at all: a stack unmaps the page
 * it is not showing, and an unmapped widget cannot be drawn, let alone drawn
 * scaled into a card's rectangle. AdwTabOverview has the same shape and the same
 * reason (its `child` property). */
void epimone_overview_set_content (EpimoneOverview *self,
                                   GtkWidget       *child,
                                   GtkWidget       *view);

gboolean epimone_overview_get_open (EpimoneOverview *self);

/* Animate into and out of the overview, anchored on the card for @active_group
 * (the group of the tab that is showing, or about to show). Closing prefers the
 * card the user just activated, so the tab appears to come out of that card.
 *
 * Neither of these restores focus: the close animation's completion does, which
 * is how focus lands on the terminal after the transition rather than during it.
 * epimone_window_hide_overview remains the only exit. */
void epimone_overview_animate_open (EpimoneOverview *self, guint64 active_group);
void epimone_overview_animate_close (EpimoneOverview *self, guint64 active_group);

G_END_DECLS
