/* The session overview. See epimone-overview.h for the arrangement and for why
 * this is a flow box of cards rather than an AdwTabOverview. */
#include "epimone-overview.h"
#include "epimone-window.h"
#include "epimone-page.h"
#include "epimone-layout.h"
#include "epimone-client.h"

#include <vte/vte.h>
#include <math.h>

struct _EpimoneOverview
{
  GtkWidget     parent_instance;

  EpimoneWindow *win;          /* borrowed; the window this overview belongs to */

  /* The two children, both permanently parented and both allocated the full
   * size. `child` is the whole app content (the window's toolbar view: header,
   * tab bar and tab view); `overview_ui` is this widget's own chrome and card
   * grid. Which one is DRAWN is decided in snapshot, and during the transition
   * both are, with `child` scaled into the anchor card's rect.
   *
   * That is the whole reason this is a plain GtkWidget with two children rather
   * than a page of the window's GtkStack: a stack unmaps the page it is not
   * showing, and an unmapped widget cannot be drawn at all, let alone drawn
   * scaled. AdwTabOverview is built exactly this way and never unmaps its child
   * either; it only toggles child-visible (adw-tab-overview.c
   * set_overview_visible). */
  GtkWidget    *child;
  GtkWidget    *overview_ui;
  GtkWidget    *view;          /* borrowed: the tab view, whose rect a card depicts */

  /* Zoom transition. `progress` is 0 with the tabs showing and 1 with the grid
   * showing; the animation only ever sets it and queues a draw, exactly as
   * AdwTabOverview does. All the geometry is computed per frame in snapshot, so
   * nothing has to be kept in step with the layout. */
  gboolean      renderer_warm;   /* first GSK texture render costs ~130 ms; see below */
  guint         last_cols, last_rows;   /* grid the recycled terminal is already at */

  AdwAnimation *open_animation;
  double        progress;
  gboolean      is_open;
  gboolean      animating;
  gboolean      crossfade;     /* animations disabled: fade instead of morph */

  /* The card the transition flies to and from. `anchor` is the thumbnail widget
   * whose rect is interpolated; it is hidden for the duration so the card is not
   * drawn twice, and restored on completion (AdwTabThumbnail does the same with
   * fade_out/fade_in). `pending_anchor` is set by a card activation so the close
   * that follows anchors on the card the user actually clicked.
   *
   * WEAK pointers, not plain ones: these name widgets owned by the card grid, and
   * a reload destroys every card. Held plainly, a reopen after a rebuild called
   * gtk_widget_set_opacity on freed memory and took the process out with a
   * SIGSEGV inside g_type_check_instance_is_a. Weak pointers go NULL with the
   * widget, and a NULL anchor is already a supported state: it cross-fades. */
  GtkWidget    *anchor;
  GtkWidget    *pending_anchor;

  GtkWidget    *stack;         /* cards vs the empty state */
  /* One continuous grid: every card (this window's attached tabs first, then
   * all detached ones) lives in a single `flow`, with no divider or section
   * break between them. Membership is derived per reload from this window's
   * tab view, with no cached window-to-group map, so Move to New Window is
   * automatically right. */
  GtkWidget    *grid_box;      /* vertical box holding `flow` */
  GtkWidget    *flow;          /* the one GtkFlowBox: attached cards then detached */
  GtkWidget    *search_bar;
  GtkWidget    *search_entry;
  GtkWidget    *search_button;
  GtkWidget    *scroller;

  /* The overview-mode header bar (search, live count, the pressed grid toggle).
   * It lives INSIDE overview_ui rather than being handed to the window: the app's
   * own header is part of `child` and zooms away with it, so this one has to be
   * drawn by the overview itself for the two to trade places continuously. */
  GtkWidget      *header;
  AdwWindowTitle *count_title;

  /* Current card geometry, recomputed from the allocation by AdwTabOverview's
   * sizing formula (overview_card_thumb_width). Cards scale with the window and
   * the card count instead of sitting at a fixed size. */
  int           thumb_w;
  int           thumb_h;
  /* Aspect (height/width) the current card textures were rendered at, captured
   * only when textures are genuinely (re)rendered; see overview_reload and
   * overview_appearance_changed_cb. Card height is derived from THIS on resize,
   * not from the live window, so the card box aspect always equals the texture
   * aspect and the content never distorts or letterboxes. Mirrors libadwaita's
   * get_tab_height, which takes the height from the cached paintable's own
   * aspect rather than from the window. */
  double        thumb_aspect;
  int           columns;
  guint         n_cards;
  guint         resize_idle;        /* deferred size-request update */

  /* ONE recycled VteTerminal renders every detached card in turn, rather than one
   * per card. It is the overlay's main child with START alignment so ordinary
   * layout hands it exactly its natural size: a widget must be both mapped AND
   * properly allocated before GTK will snapshot anything, and a forced allocation
   * is not enough because VTE lays its grid out in response to a real
   * size-allocate. The UI sits on top as an opaque overlay child, so it is never
   * seen. */
  GtkWidget    *thumb_term;
  GdkPaintable *thumb_paintable;

  /* Cards waiting to be rendered, nearest-visible first. Rendering is spread over
   * timeouts so opening the overview is never blocked by it. */
  GQueue       *pending;
  gboolean      rendering;

  /* The finished textures, PER GROUP, surviving every reload and every
   * open/close. This is what makes opening instant: a card whose group has ever
   * been rendered shows that texture in the same frame the grid is built, and
   * the offscreen pipeline only refreshes it afterwards, invisibly. It is the
   * same architecture as AdwTabOverview, whose cards are NOT live widgets but
   * cached textures (AdwTabPaintable's cached_paintable, adw-tab-view.c) that
   * persist while the overview is closed and are refreshed opportunistically.
   * Keys are group ids; values are owned GdkTextures. Entries for dead groups
   * are dropped by overview_reload. */
  GHashTable   *thumb_cache;

  /* Content signature of each cached texture, keyed the same as thumb_cache
   * (cache_key). A cheap hash of exactly what the thumbnail shows: per pane, the
   * session id, its grid, and the PEEK tail bytes that were rendered. Before an
   * open re-renders a still-cached card, queue_visible_renders re-peeks and skips
   * the render entirely when the signature is unchanged; this prevents cards
   * "settling" a beat after the zoom, and is the same policy AdwTabOverview
   * follows (refresh a thumbnail only when its contents actually invalidate).
   * Values are owned guint64*. Lifetime tracks thumb_cache exactly. */
  GHashTable   *thumb_sig;

  /* During an OPEN reload only: the group whose live-capture texture was just
   * stored (by epimone_overview_animate_open, right before overview_reload).
   * build_card keeps that card's fresh live texture instead of queuing a PEEK
   * refresh, so the active card does not twitch when a differently-produced
   * texture would otherwise land ~80 ms after the zoom settles. 0 at every
   * other reload; kill/restore/etc. still refresh the active card normally. */
  guint64       fresh_capture_group;

  /* One-shot pre-render after the window maps, so the cache is already warm the
   * FIRST time the overview opens. The overview_ui subtree (and with it the
   * recycled terminal) is permanently mapped whether or not the overview is
   * showing, so the pipeline can run while the user is still in their tabs. */
  guint         preload_timeout;
  gboolean      preloaded;

  /* Settle tracking for the card being rendered: the backstop timeout and the
   * quiet-period timeout armed by contents-changed. Whichever fires first wins, and
   * both are cancelled once the snapshot is taken. `fed` guards the quiet timer:
   * the reset/resize in render_start also emits contents-changed, and a quiet
   * period armed by THAT fires before render_feed has run, snapshotting an empty
   * grid (observed: the first queued card came out blank, and the still-pending
   * feed timeout then double-started the next card). Only a contents-changed
   * after feeding may arm the quiet timer, and the feed timeout is tracked so a
   * finish cancels it. */
  guint         settle_backstop;
  guint         settle_quiet;
  guint         feed_timeout;
  guint         pump_timeout;     /* the between-cards hop back to the main loop */
  gboolean      fed;
  gsize         fed_len;         /* bytes fed for the card being rendered */
  guint64       pane_sig;        /* signature of the pane peeked by render_feed,
                                  * folded into the card's signature when the pane
                                  * commits in render_finish */
  gulong        contents_handler;

  /* The card the OPEN right-click menu belongs to, snapshotted at popup time as
   * owned copies rather than borrowed from the card: a grid rebuild while the
   * menu is up destroys every card, and the menu actions must not chase freed
   * CardInfo. menu_group is 0 when no menu has been opened yet. */
  guint64       menu_group;
  guint         menu_panes;
  /* Whether that card's group was ATTACHED when the menu opened, and one of
   * its session ids: enough to find the live page again. Which items the menu
   * shows depends on this (Detach Tab and Move to New Window need a live tab;
   * Attach in New Window only makes sense without one). */
  gboolean      menu_attached;
  guint64       menu_session;
  /* The "card." action group, kept so the state-dependent items' enabled flags
   * can be set when a menu opens. Owned; dropped in dispose. */
  GSimpleActionGroup *card_actions;
  /* The header ⋮ "overview." action group, kept so the scoped kill items can be
   * enabled/disabled by their live counts each time the menu opens. Owned. */
  GSimpleActionGroup *overview_actions;
  char         *menu_title;      /* display title (custom name when set) */
  char         *menu_shell;      /* the shell-provided title */
  char         *menu_custom;     /* the custom name, NULL when automatic */
  char         *menu_procs;      /* every pane's foreground command, joined */
  char         *menu_age;
};

/* VTE parses fed bytes on its own timer, so a snapshot taken immediately after
 * vte_terminal_feed() catches an empty grid: real elapsed time has to pass. Rather
 * than guess how much, watch for VTE to report that the grid changed and snapshot a
 * beat after it goes quiet. SETTLE_MS survives only as a backstop for the case where
 * contents-changed never fires (an empty tail, for instance). */
#define SETTLE_QUIET_MS 12
#define SETTLE_MS 60

/* Bounded tail: a card shows a screenful, not a megabyte. */
#define PEEK_BYTES (48 * 1024)

/* How many times a card may be re-attempted before its placeholder is accepted.
 * A pane can be snapshotted before the recycled terminal has been allocated at
 * the grid just requested (most easily right after a window resize), and the
 * capture then yields nothing. Retrying is the fix; a bound stops a genuinely
 * unrenderable card from looping forever. */
#define RENDER_MAX_ATTEMPTS 4

/* Orphan (single-session safety-net) cards key their texture cache by this bit
 * OR'd with the session id. Group ids are small daemon counters that never reach
 * the top bit, so a sentinel key can never collide with a real group's. */
#define ORPHAN_CACHE_BIT (G_GUINT64_CONSTANT (1) << 63)

/* AdwTabOverview's transition, matched exactly: TRANSITION_DURATION 400 ms with
 * ADW_EASE, which is cubic-bezier(0.25, 0.1, 0.25, 1.0), CSS "ease"
 * (adw-tab-overview.c line 30, adw-easing.c ADW_EASE). The corner radius is
 * interpolated too, from the window's radius to a card's. */
#define TRANSITION_DURATION 400
#define THUMBNAIL_BORDER_RADIUS 12
#define WINDOW_BORDER_RADIUS 15

G_DEFINE_FINAL_TYPE (EpimoneOverview, epimone_overview, GTK_TYPE_WIDGET)

/* The card grid is one flow box now (attached cards first, then detached, in one
 * continuous grid). This helper survives so the card-walking loops keep their
 * shape; there is a single section, so @i is always 0. */
static inline GtkWidget *
overview_flow_at (EpimoneOverview *self, int i)
{
  (void) i;
  return self->flow;
}

/* What one card needs to know. Attached to the card widget so the filter and the
 * click handlers can read it back without re-querying the daemon. */
typedef struct {
  guint64    group_id;
  /* Texture-cache key. Equals group_id for a normal (group) card. An orphan
   * safety-net card (a live session no group card represents) uses a distinct
   * sentinel key so it never shares or clobbers a real group's cached texture. */
  guint64    cache_key;
  /* When non-zero, this card represents ONE detached session rather than a whole
   * group, and its kill control kills exactly this session, never the group id
   * (which for an orphan is its still-attached parent tab). 0 for normal cards. */
  guint64    kill_session;
  char      *haystack;         /* lowercased title + process names, for search */

  /* Thumbnail state. `picture` is the card's image; the stack switches between
   * it and the placeholder art shown before the render lands. */
  GtkWidget *picture;
  GtkWidget *stack;
  gboolean   rendered;

  /* The tab's whole arrangement: one entry per pane with its fractional rect
   * (epimone_layout_blob_geometry), and the dividers between them. A card is
   * rendered by walking `leaves` one at a time through the single recycled
   * terminal, keeping each pane's render node in `nodes`, then composing all of
   * them into one texture, so a split tab looks like the tab, not like one of
   * its panes. `cur_leaf` is how far through that walk the card is. */
  GPtrArray *leaves;           /* EpimoneLayoutLeaf*, owned */
  GPtrArray *seams;            /* EpimoneLayoutSeam*, owned */
  GPtrArray *nodes;            /* GskRenderNode*, owned, one per leaf (may hold NULL) */
  guint      cur_leaf;
  guint      attempts;         /* bounded retries; see RENDER_MAX_ATTEMPTS */

  /* Content signature accumulated while this card renders: reset in render_pump
   * when the card starts, folded one pane at a time in render_finish, and stored
   * in thumb_sig once the card composes. sig_panes counts panes folded so a
   * partial render is never mistaken for a complete signature. */
  guint64    sig;
  guint      sig_panes;
} CardInfo;

static void
card_nodes_clear (CardInfo *ci)
{
  if (ci->nodes == NULL)
    return;
  for (guint i = 0; i < ci->nodes->len; i++)
    {
      GskRenderNode *node = g_ptr_array_index (ci->nodes, i);

      if (node != NULL)
        gsk_render_node_unref (node);
    }
  g_ptr_array_set_size (ci->nodes, 0);
}

static void
card_info_free (gpointer data)
{
  CardInfo *ci = data;

  card_nodes_clear (ci);
  if (ci->nodes != NULL)
    g_ptr_array_unref (ci->nodes);
  if (ci->leaves != NULL)
    g_ptr_array_unref (ci->leaves);
  if (ci->seams != NULL)
    g_ptr_array_unref (ci->seams);
  g_free (ci->haystack);
  g_free (ci);
}

/* FNV-1a 64-bit, folded incrementally. Used only to tell "same thumbnail
 * content" from "changed" cheaply; not a security hash. */
#define SIG_FNV_OFFSET G_GUINT64_CONSTANT (14695981039346656037)
#define SIG_FNV_PRIME  G_GUINT64_CONSTANT (1099511628211)

static inline guint64
sig_fold_bytes (guint64 h, const guint8 *p, gsize n)
{
  for (gsize i = 0; i < n; i++)
    h = (h ^ p[i]) * SIG_FNV_PRIME;
  return h;
}

static inline guint64
sig_fold_u64 (guint64 h, guint64 v)
{
  guint8 b[8];

  for (int i = 0; i < 8; i++)
    b[i] = (guint8) (v >> (8 * i));
  return sig_fold_bytes (h, b, 8);
}

/* One pane's signature: its session, its grid, and the PEEK tail bytes the
 * thumbnail is drawn from. render_feed and card_current_sig MUST compute this
 * identically, so the skip check and the stored signature are comparable. */
static guint64
pane_signature (guint64 session, guint cols, guint rows,
                const guint8 *tail, gsize tail_len)
{
  guint64 h = SIG_FNV_OFFSET;

  h = sig_fold_u64 (h, session);
  h = sig_fold_u64 (h, ((guint64) cols << 32) | rows);
  h = sig_fold_u64 (h, (guint64) tail_len);
  if (tail != NULL && tail_len > 0)
    h = sig_fold_bytes (h, tail, tail_len);
  return h;
}

/* The card's CURRENT content signature, by re-peeking every pane exactly as the
 * render pipeline does. Returns FALSE (and leaves *out untouched) if the card
 * has no panes, so a card with nothing to compare is never skipped. The peeks
 * are the same control-socket round trips a render would make anyway; the point
 * is to skip the far heavier VTE feed + layout + snapshot when nothing changed. */
static gboolean
card_current_sig (CardInfo *ci, guint64 *out)
{
  guint64 h = SIG_FNV_OFFSET;

  if (ci->leaves == NULL || ci->leaves->len == 0)
    return FALSE;

  for (guint i = 0; i < ci->leaves->len; i++)
    {
      EpimoneLayoutLeaf *leaf = g_ptr_array_index (ci->leaves, i);
      g_autofree guint8 *tail = NULL;
      gsize tail_len = 0;
      guint64 total = 0;

      tail = epimone_client_peek_session (leaf->session, PEEK_BYTES,
                                          &tail_len, &total, NULL);
      h = sig_fold_u64 (h, pane_signature (leaf->session, leaf->cols,
                                           leaf->rows, tail, tail_len));
    }

  *out = h;
  return TRUE;
}

static void overview_reload (EpimoneOverview *self);
static void overview_appearance_changed_cb (gpointer data);

/* ------------------------------------------------------------------ *
 * formatting helpers
 * ------------------------------------------------------------------ */

/* "just now" / "12m" / "3h" / "5d" from a unix timestamp. */
static char *
format_age (gint64 created_at)
{
  gint64 secs = g_get_real_time () / G_USEC_PER_SEC - created_at;

  if (secs < 0)
    secs = 0;
  if (secs < 60)
    return g_strdup ("just now");
  if (secs < 3600)
    return g_strdup_printf ("%" G_GINT64_FORMAT "m", secs / 60);
  if (secs < 86400)
    return g_strdup_printf ("%" G_GINT64_FORMAT "h", secs / 3600);
  return g_strdup_printf ("%" G_GINT64_FORMAT "d", secs / 86400);
}

/* ------------------------------------------------------------------ *
 * the zoom transition
 *
 * Mechanism, transcribed from AdwTabOverview (adw-tab-overview.c
 * calculate_bounds + adw_tab_overview_snapshot):
 *
 * Nothing is re-laid-out and no paintable is substituted. Both children keep
 * their full-size allocation the whole time, and the LIVE child is drawn through
 * an interpolated transform, so what shrinks into the card is the real terminal,
 * still running and still repainting.
 *
 * Per frame, three things are interpolated together:
 *   - the visible WINDOW onto the child: a rect easing from the whole widget to
 *     just the tab view's rect. This is what slides the header and tab bar out of
 *     frame instead of shrinking them into the card.
 *   - the SCALE: from 1 to (card size / view size), per axis, ending on the
 *     same per-axis stretch the settled card texture was baked with, so the
 *     hand-off frame and the card are geometrically identical.
 *   - the POSITION: from the widget origin to the card's origin, driven not by
 *     raw progress but by how far the SIZE has already travelled
 *     (inverse_lerp), which is what keeps position and scale coupled so the rect
 *     reads as one object flying to the card.
 * Plus a corner radius easing window-radius -> card-radius, a shade layer over
 * the grid fading out as the grid takes over, and the card's own thumbnail
 * hidden so it is not drawn twice.
 * ------------------------------------------------------------------ */

static double
inverse_lerp (double a, double b, double t)
{
  return (t - a) / (b - a);
}

/* Compose the frame's geometry. @bounds is the whole widget, @transition is the
 * rect the child is drawn into, and @clip / @clip_scale say which part of the
 * child that rect shows and at what scale. */
static void
overview_calculate_bounds (EpimoneOverview *self,
                           graphene_rect_t *bounds,
                           graphene_rect_t *transition,
                           graphene_rect_t *clip,
                           graphene_size_t *clip_scale)
{
  GtkWidget *widget = GTK_WIDGET (self);
  graphene_rect_t view_bounds, anchor_bounds;

  graphene_rect_init (bounds, 0, 0,
                      gtk_widget_get_width (widget),
                      gtk_widget_get_height (widget));

  if (self->view == NULL ||
      !gtk_widget_compute_bounds (self->view, widget, &view_bounds))
    view_bounds = *bounds;
  if (self->anchor == NULL ||
      !gtk_widget_compute_bounds (self->anchor, widget, &anchor_bounds))
    graphene_rect_init (&anchor_bounds, 0, 0, 0, 0);

  /* The FULL view rect, deliberately NOT cropped to the card's aspect. The
   * settled card texture is the whole view stretched per-axis into the card
   * (render_compose scales each pane into its fractional rect), so the fly must
   * end on that same mapping: per-axis clip_scale below lands the full view on
   * the anchor rect at progress 1, pixel-matching the settled card.
   * AdwTabOverview crops here instead, legitimate for it only because its
   * settled thumbnail applies the SAME cover-crop (AdwTabPaintable's snapshot,
   * transform_thumbnail); fly and settle must agree, whichever mapping is
   * chosen. Cropping here while the card stretches causes a visible snap at
   * settle. */
  graphene_rect_interpolate (bounds, &view_bounds, self->progress, clip);

  graphene_size_init (clip_scale,
                      (float) adw_lerp (1.0,
                                        anchor_bounds.size.width / MAX (view_bounds.size.width, 1.0f),
                                        self->progress),
                      (float) adw_lerp (1.0,
                                        anchor_bounds.size.height / MAX (view_bounds.size.height, 1.0f),
                                        self->progress));

  graphene_size_init (&transition->size,
                      clip->size.width * clip_scale->width,
                      clip->size.height * clip_scale->height);
  graphene_point_init (&transition->origin,
                       (float) adw_lerp (0, anchor_bounds.origin.x,
                                         inverse_lerp (bounds->size.width,
                                                       anchor_bounds.size.width,
                                                       transition->size.width)),
                       (float) adw_lerp (0, anchor_bounds.origin.y,
                                         inverse_lerp (bounds->size.height,
                                                       anchor_bounds.size.height,
                                                       transition->size.height)));
}

/* Hide the anchor card's thumbnail while the transition owns it, so the card is
 * not drawn both in the grid and as the flying rect. AdwTabThumbnail does the
 * same (fade_out / fade_in). */
static void
overview_set_anchor (EpimoneOverview *self, GtkWidget *anchor)
{
  if (self->anchor == anchor)
    return;
  if (self->anchor != NULL)
    gtk_widget_set_opacity (self->anchor, 1.0);
  g_set_weak_pointer (&self->anchor, anchor);
  if (self->anchor != NULL)
    gtk_widget_set_opacity (self->anchor, 0.0);
}

static void
overview_set_pending_anchor (EpimoneOverview *self, GtkWidget *anchor)
{
  g_set_weak_pointer (&self->pending_anchor, anchor);
}

/* Which card depicts @group_id, or NULL. Used to anchor an exit that did not
 * come from clicking a card (Escape, the header toggle). */
static GtkWidget *
overview_find_anchor_for_group (EpimoneOverview *self, guint64 group_id)
{
  GtkWidget *child;

  if (group_id == 0)
    return NULL;

  for (int s = 0; s < 1; s++)
    for (child = gtk_widget_get_first_child (overview_flow_at (self, s));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
      {
        GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
        CardInfo *ci = card != NULL
          ? g_object_get_data (G_OBJECT (card), "epi-card") : NULL;

        if (ci != NULL && ci->group_id == group_id)
          return ci->stack;
      }
  return NULL;
}

/* Put the anchor card on screen before flying from it. AdwTabOverview does the
 * same on open (adw_tab_grid_try_focus_selected_tab): a card scrolled out of view
 * would otherwise give the transition an off-screen rect to start from. */
static void
overview_scroll_anchor_into_view (EpimoneOverview *self, guint64 group_id)
{
  GtkWidget *anchor = overview_find_anchor_for_group (self, group_id);
  GtkAdjustment *adj;
  graphene_rect_t bounds;
  double page, value;

  if (anchor == NULL || self->scroller == NULL)
    return;
  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroller));
  if (adj == NULL || !gtk_widget_compute_bounds (anchor, self->grid_box, &bounds))
    return;

  page = gtk_adjustment_get_page_size (adj);
  value = gtk_adjustment_get_value (adj);

  if (bounds.origin.y < value)
    value = bounds.origin.y;
  else if (bounds.origin.y + bounds.size.height > value + page)
    value = bounds.origin.y + bounds.size.height - page;
  else
    return;

  gtk_adjustment_set_value (adj, CLAMP (value,
                                        gtk_adjustment_get_lower (adj),
                                        MAX (gtk_adjustment_get_lower (adj),
                                             gtk_adjustment_get_upper (adj) - page)));
}

/* Input routing for the current state, mirroring set_overview_visible in
 * adw-tab-overview.c: whichever side is not showing must not take clicks or
 * focus, and during the transition neither does. */
static void
overview_update_targetable (EpimoneOverview *self)
{
  gboolean open = self->is_open;
  gboolean animating = self->animating;

  gtk_widget_set_can_target (self->overview_ui, open && !animating);
  gtk_widget_set_can_focus (self->overview_ui, open);
  if (self->child != NULL)
    {
      gtk_widget_set_can_target (self->child, !open && !animating);
      gtk_widget_set_can_focus (self->child, !open && !animating);
    }
}

static void
open_animation_value_cb (double value, EpimoneOverview *self)
{
  /* Only ever this: the geometry is recomputed per frame in snapshot. */
  self->progress = value;
  gtk_widget_queue_draw (GTK_WIDGET (self));
}

static void
open_animation_done_cb (EpimoneOverview *self)
{
  self->animating = FALSE;
  overview_set_anchor (self, NULL);
  overview_update_targetable (self);
  gtk_widget_queue_draw (GTK_WIDGET (self));

  /* THE single place focus returns to the terminal, and it happens here: after
   * the transition, not before it. epimone_window_hide_overview starts the
   * animation and returns; this is its completion. */
  if (!self->is_open && self->win != NULL)
    epimone_window_focus_active_terminal (self->win);
}

/* Start a transition to @open, flying to/from @anchor (which may be NULL for a
 * cross-fade). */
static void
overview_animate_to (EpimoneOverview *self, gboolean open, GtkWidget *anchor)
{
  GtkSettings *settings = gtk_widget_get_settings (GTK_WIDGET (self));
  gboolean animations = TRUE;

  if (settings != NULL)
    g_object_get (settings, "gtk-enable-animations", &animations, NULL);

  self->is_open = open;
  overview_set_anchor (self, anchor);
  /* No animation support, or nothing to fly to: fall back to a cross-fade, which
   * is what AdwTabOverview substitutes when motion is reduced. */
  self->crossfade = !animations;

  self->animating = TRUE;
  overview_update_targetable (self);

  if (!animations)
    {
      /* Land on the end state immediately rather than pretending to animate. */
      self->progress = open ? 1.0 : 0.0;
      adw_animation_skip (self->open_animation);
      open_animation_done_cb (self);
      return;
    }

  adw_timed_animation_set_value_from (ADW_TIMED_ANIMATION (self->open_animation),
                                      self->progress);
  adw_timed_animation_set_value_to (ADW_TIMED_ANIMATION (self->open_animation),
                                    open ? 1.0 : 0.0);
  adw_animation_play (self->open_animation);
}

/* Take the one-off cost of the first gsk_renderer_render_texture() before the
 * user ever opens the overview.
 *
 * Measured: the first compose of a session takes ~130 ms and every later one
 * 0.3-3 ms; the difference is GSK setting up its texture-render path, not
 * anything about the thumbnail. Paid on the first card, it lands right in the
 * middle of the opening animation and is most of why the last card arrives after
 * the animation has already finished. Paid at map time it costs nothing visible. */
static void
overview_warm_renderer (gpointer data)
{
  EpimoneOverview *self = data;
  GskRenderer *renderer;
  GtkSnapshot *snapshot;
  GskRenderNode *node;

  if (self->renderer_warm)
    return;
  renderer = gtk_native_get_renderer (gtk_widget_get_native (GTK_WIDGET (self)));
  if (renderer == NULL)
    return;

  snapshot = gtk_snapshot_new ();
  gtk_snapshot_append_color (snapshot, &(GdkRGBA) { 0, 0, 0, 1 },
                             &GRAPHENE_RECT_INIT (0, 0, 1, 1));
  node = gtk_snapshot_free_to_node (snapshot);
  if (node != NULL)
    {
      GdkTexture *tex = gsk_renderer_render_texture (renderer, node, NULL);

      if (tex != NULL)
        g_object_unref (tex);
      gsk_render_node_unref (node);
      self->renderer_warm = TRUE;
    }
}

/* Fill the texture cache BEFORE the overview is ever opened. The overview_ui
 * subtree (including the recycled offscreen terminal) is permanently mapped
 * regardless of whether the overview is showing, so the whole pipeline can run
 * while the user is still looking at their tabs. The first open then draws
 * every card from cache in its first frame instead of walking the queue during
 * the transition. */
static void
overview_preload_cb (gpointer data)
{
  EpimoneOverview *self = data;

  self->preload_timeout = 0;
  if (self->is_open || self->animating)
    return;   /* the open already reloaded; nothing to pre-warm */
  g_debug ("thumb: pre-rendering the cache before first open");
  overview_reload (self);
}

static void
epimone_overview_map (GtkWidget *widget)
{
  EpimoneOverview *self = EPIMONE_OVERVIEW (widget);

  GTK_WIDGET_CLASS (epimone_overview_parent_class)->map (widget);
  /* Off the critical path: an idle after the window is up. */
  g_idle_add_once (overview_warm_renderer, widget);

  if (!self->preloaded)
    {
      self->preloaded = TRUE;
      /* After startup has settled: restoring a session's tabs is heavier than
       * this and gets the first claim on the main loop. */
      self->preload_timeout = g_timeout_add_once (600, overview_preload_cb, self);
    }
}

static void
epimone_overview_snapshot (GtkWidget *widget, GtkSnapshot *snapshot)
{
  EpimoneOverview *self = EPIMONE_OVERVIEW (widget);

  if (!self->animating)
    {
      /* Settled: draw exactly one of the two. */
      gtk_widget_snapshot_child (widget,
                                 self->is_open ? self->overview_ui : self->child,
                                 snapshot);

      /* The offscreen render host lives inside overview_ui, and GTK only
       * refreshes a widget's render nodes when it is actually drawn: a
       * GtkWidgetPaintable over a widget in the undrawn subtree captures
       * NOTHING. So while the pipeline is running with the overview closed
       * (the startup pre-render, a refresh finishing after a close), the
       * hidden side is drawn into a throwaway snapshot purely so its nodes
       * exist. AdwTabView does precisely this for its thumbnails ("We don't
       * want to actually draw the child, but we do need it to redraw so that
       * it can be displayed by its paintable", adw-tab-view.c snapshot). */
      if (!self->is_open && self->rendering)
        {
          GtkSnapshot *throwaway = gtk_snapshot_new ();

          gtk_widget_snapshot_child (widget, self->overview_ui, throwaway);
          g_object_unref (throwaway);
        }
      return;
    }

  if (self->crossfade || self->anchor == NULL)
    {
      /* Animations off, or no card to fly to (an empty overview, or a card that
       * has not been allocated yet): a plain cross-fade is the honest fallback,
       * and it is what AdwTabOverview does for reduced motion. */
      gtk_snapshot_push_cross_fade (snapshot, self->progress);
      gtk_widget_snapshot_child (widget, self->child, snapshot);
      gtk_snapshot_pop (snapshot);
      gtk_widget_snapshot_child (widget, self->overview_ui, snapshot);
      gtk_snapshot_pop (snapshot);
      return;
    }

  {
    graphene_rect_t bounds, transition, clip;
    graphene_size_t clip_scale, corner;
    GskRoundedRect rect;
    GdkRGBA shade = { 0.0f, 0.0f, 0.0f, 0.0f };

    overview_calculate_bounds (self, &bounds, &transition, &clip, &clip_scale);

    graphene_size_init (&corner,
                        (float) adw_lerp (WINDOW_BORDER_RADIUS,
                                          THUMBNAIL_BORDER_RADIUS, self->progress),
                        (float) adw_lerp (WINDOW_BORDER_RADIUS,
                                          THUMBNAIL_BORDER_RADIUS, self->progress));
    gsk_rounded_rect_init (&rect, &transition,
                           &corner, &corner, &corner, &corner);

    /* The grid sits underneath for the whole transition. */
    gtk_widget_snapshot_child (widget, self->overview_ui, snapshot);

    /* Shade over the grid, strongest while the terminal still covers it and gone
     * by the time the grid is fully out. libadwaita's own shade_color values:
     * RGB(0 0 6 / 25%) dark, 7% light (_colors.scss), picked by the palette's own
     * light/dark decision rather than by a style lookup, since epimone-chrome.c
     * already drives the colour scheme from the palette. */
    shade.red = 0.0f;
    shade.green = 0.0f;
    shade.blue = 6.0f / 255.0f;
    shade.alpha = (float) ((adw_style_manager_get_dark (adw_style_manager_get_default ())
                             ? 0.25 : 0.07)
                           * (1.0 - self->progress));
    gtk_snapshot_append_color (snapshot, &shade, &bounds);

    /* The child, clipped to the interpolated rounded rect and transformed so the
     * tab view's area lands exactly on it. Drawn as a WIDGET, not a texture, so
     * the terminal is live throughout. */
    gtk_snapshot_push_rounded_clip (snapshot, &rect);
    gtk_snapshot_translate (snapshot, &transition.origin);
    gtk_snapshot_scale (snapshot, clip_scale.width, clip_scale.height);
    gtk_snapshot_translate (snapshot,
                            &GRAPHENE_POINT_INIT (-clip.origin.x, -clip.origin.y));
    gtk_widget_snapshot_child (widget, self->child, snapshot);
    gtk_snapshot_pop (snapshot);
  }
}

/* ------------------------------------------------------------------ *
 * card geometry
 * ------------------------------------------------------------------ */

/* AdwTabOverview's card sizing, transcribed from adw-tab-grid.c
 * (get_n_columns + get_tab_width, libadwaita 1.9). The behaviour it encodes:
 *
 *   - the natural card width eases from 200 px on a 360 px grid up to 360 px on
 *     a 2560 px grid (ease-out-cubic), and the column count is how many of
 *     those fit, clamped to [2 .. cards] with a hard cap of 8;
 *   - the grid spends 100% of the width when narrow easing down to 85% when
 *     wide;
 *   - when the column count is limited by the CARD COUNT rather than the width
 *     (fewer cards than would fit), the row is additionally scaled by
 *     0.5 + 0.5 * n / n_fit, which is why a single card takes about half
 *     the window rather than all of it;
 *   - what remains is divided among the columns and clamped to [100 .. 500]
 *     logical px.
 *
 * The card width therefore GROWS as tabs close and as the window widens, which
 * is what keeps the thumbnail text legible; the render path was never the
 * limit, a fixed card width was. SPACING here is the flow box's 18 px gap
 * where AdwTabGrid uses 5; the mechanism is otherwise identical. */
#define OVERVIEW_SPACING            18
#define OVERVIEW_MIN_COLUMNS        2
#define OVERVIEW_MAX_COLUMNS        8
#define OVERVIEW_MIN_THUMB_WIDTH    100
#define OVERVIEW_MAX_THUMB_WIDTH    500
#define OVERVIEW_SMALL_GRID_WIDTH   360.0
#define OVERVIEW_LARGE_GRID_WIDTH   2560.0
#define OVERVIEW_SMALL_NAT_WIDTH    200.0
#define OVERVIEW_LARGE_NAT_WIDTH    360.0
#define OVERVIEW_LARGE_GRID_USE     0.85
#define OVERVIEW_SINGLE_CARD_USE    0.5

static int
overview_card_thumb_width (int    avail,
                           guint  n_cards,
                           int   *out_columns)
{
  double t = CLAMP (((double) avail - OVERVIEW_SMALL_GRID_WIDTH) /
                    (OVERVIEW_LARGE_GRID_WIDTH - OVERVIEW_SMALL_GRID_WIDTH),
                    0.0, 1.0);
  double eased = 1.0 - pow (1.0 - t, 3);   /* ease-out-cubic */
  double nat = OVERVIEW_SMALL_NAT_WIDTH +
               (OVERVIEW_LARGE_NAT_WIDTH - OVERVIEW_SMALL_NAT_WIDTH) * eased;
  double max_n = CLAMP ((double) MAX (n_cards, 1u), 1.0, OVERVIEW_MAX_COLUMNS);
  double n_fit = CLAMP (ceil (avail / nat),
                        OVERVIEW_MIN_COLUMNS, OVERVIEW_MAX_COLUMNS);
  double n = CLAMP (ceil (avail / nat), MIN (OVERVIEW_MIN_COLUMNS, max_n), max_n);
  double total = avail * (1.0 - (1.0 - OVERVIEW_LARGE_GRID_USE) * eased);
  int width;

  /* n / n_fit is 1 when the width is the limit; below 1 when the card count is,
   * which shrinks the row so one lone card doesn't span the whole window. */
  total *= OVERVIEW_SINGLE_CARD_USE +
           (1.0 - OVERVIEW_SINGLE_CARD_USE) * n / n_fit;

  width = (int) ceil ((total - OVERVIEW_SPACING * (n + 1)) / n);

  if (out_columns != NULL)
    *out_columns = (int) n;

  return CLAMP (width, OVERVIEW_MIN_THUMB_WIDTH, OVERVIEW_MAX_THUMB_WIDTH);
}

/* The card aspect (height/width) to render textures at: the overview area's own
 * aspect, clamped so a half-tiled tall or very wide window does not produce
 * absurd portrait/letterbox cards. Captured into self->thumb_aspect only at
 * render time (overview_reload / overview_appearance_changed_cb), so a later
 * resize does not change the aspect the textures were baked at. Falls back to
 * 0.6 before the overview has a real allocation (e.g. the startup preload). */
static double
overview_render_aspect (EpimoneOverview *self)
{
  int width = gtk_widget_get_width (GTK_WIDGET (self));
  int height = gtk_widget_get_height (GTK_WIDGET (self));
  double aspect = width > 0 ? (double) height / (double) width : 0.6;

  return CLAMP (aspect, 0.4, 0.9);
}

/* Recompute self->thumb_w/h/columns for @width x @height of the overview area.
 * Width and column count reflow with the window (the faithful get_tab_width
 * port); height comes from self->thumb_aspect (the texture's baked aspect),
 * NOT from @height. Pure computation (no widget mutation, so it is safe from
 * inside size_allocate); returns TRUE when the geometry changed. */
static gboolean
overview_update_geometry (EpimoneOverview *self,
                          int              width,
                          int              height)
{
  int avail, columns, w, h;
  double aspect;

  if (width <= 0 || self->n_cards == 0)
    return FALSE;

  avail = MAX (width - 2 * OVERVIEW_SPACING, 1);   /* the flow box's margins */
  w = overview_card_thumb_width (avail, self->n_cards, &columns);
  columns = MAX (columns, 1);

  /* Height from the TEXTURE's aspect (captured at render time), NOT the live
   * window's. On resize this keeps the card box aspect equal to the texture the
   * card is showing, so the FILL'd texture maps 1:1 with no distortion or
   * letterbox: the card scales as a rigid unit. Deriving it from the live
   * window instead would let the box aspect drift away from the frozen
   * texture on every resize, shifting the content. Mirrors libadwaita's
   * get_tab_height. @height is unused but kept in the signature as the
   * natural "available area" input. */
  (void) height;
  aspect = self->thumb_aspect;
  h = (int) round (w * aspect);

  if (w == self->thumb_w && h == self->thumb_h && columns == self->columns)
    return FALSE;

  self->thumb_w = w;
  self->thumb_h = h;
  self->columns = columns;
  return TRUE;
}

/* Push the current geometry onto the flow box and every card's art stack. The
 * stack is the one widget whose size request drives the card; the picture and
 * placeholder fill it. */
static void
overview_apply_card_sizes (EpimoneOverview *self)
{
  GtkWidget *child;

  for (int s = 0; s < 1; s++)
    {
      gtk_flow_box_set_max_children_per_line (
        GTK_FLOW_BOX (overview_flow_at (self, s)), (guint) self->columns);

      for (child = gtk_widget_get_first_child (overview_flow_at (self, s));
           child != NULL;
           child = gtk_widget_get_next_sibling (child))
        {
          GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
          CardInfo *ci;

          if (card == NULL)
            continue;
          ci = g_object_get_data (G_OBJECT (card), "epi-card");
          if (ci != NULL && ci->stack != NULL)
            gtk_widget_set_size_request (ci->stack, self->thumb_w, self->thumb_h);
        }
    }
}

/* "bash, bash, bash" -> "bash x3"; "hashcat, bash, bash" -> "hashcat, bash x2".
 * Runs of the same command collapse so the line says what is running rather than
 * repeating one word across the card. */
static char *
summarise_processes (const char *joined)
{
  g_auto (GStrv) parts = NULL;
  g_autoptr (GPtrArray) names = NULL;   /* borrowed const char* */
  g_autoptr (GArray) counts = NULL;
  GString *out;

  if (joined == NULL || joined[0] == '\0')
    return g_strdup ("no panes");

  parts = g_strsplit (joined, ", ", -1);
  names = g_ptr_array_new ();
  counts = g_array_new (FALSE, TRUE, sizeof (guint));

  for (guint i = 0; parts[i] != NULL; i++)
    {
      gboolean found = FALSE;

      for (guint j = 0; j < names->len; j++)
        {
          if (g_strcmp0 (g_ptr_array_index (names, j), parts[i]) == 0)
            {
              g_array_index (counts, guint, j)++;
              found = TRUE;
              break;
            }
        }
      if (!found)
        {
          guint one = 1;

          g_ptr_array_add (names, parts[i]);
          g_array_append_val (counts, one);
        }
    }

  out = g_string_new (NULL);
  for (guint j = 0; j < names->len; j++)
    {
      guint n = g_array_index (counts, guint, j);

      /* Three distinct commands is as much as a card's width can carry. */
      if (j == 3)
        {
          g_string_append (out, ", \xe2\x80\xa6");
          break;
        }
      if (out->len > 0)
        g_string_append (out, ", ");
      g_string_append (out, (const char *) g_ptr_array_index (names, j));
      if (n > 1)
        g_string_append_printf (out, " \xc3\x97%u", n);
    }

  return g_string_free (out, FALSE);
}

/* Is @cmd the name of a plain shell? Decides what the card's caption names:
 * "bash ×3" identifies nothing when every pane is sitting at a prompt, while a
 * pane running hashcat or vim is exactly the detail that tells sessions apart,
 * so shells are dropped from the caption and everything else is kept.
 *
 * "Is a shell" = the basename of an entry in /etc/shells (read once), unioned
 * with a built-in fallback list so an unreadable /etc/shells does not turn
 * every idle prompt into a labelled process. Comparison is against /proc comm
 * names, which carry no path and no login-shell "-" prefix; the prefix is
 * stripped anyway in case one ever appears. */
static gboolean
command_is_shell (const char *cmd)
{
  static GHashTable *shells;

  if (shells == NULL)
    {
      static const char *fallback[] = {
        "sh", "bash", "zsh", "fish", "dash", "ksh", "mksh", "tcsh", "csh",
      };
      g_autofree char *text = NULL;

      shells = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
      for (guint i = 0; i < G_N_ELEMENTS (fallback); i++)
        g_hash_table_add (shells, g_strdup (fallback[i]));

      if (g_file_get_contents ("/etc/shells", &text, NULL, NULL))
        {
          g_auto (GStrv) lines = g_strsplit (text, "\n", -1);

          for (guint i = 0; lines[i] != NULL; i++)
            {
              const char *line = g_strstrip (lines[i]);

              /* Only absolute paths count; comments and blanks do not. */
              if (line[0] != '/')
                continue;
              g_hash_table_add (shells, g_path_get_basename (line));
            }
        }
    }

  if (cmd == NULL)
    return TRUE;
  if (cmd[0] == '-')
    cmd++;
  return g_hash_table_contains (shells, cmd);
}

/* ------------------------------------------------------------------ *
 * card actions
 * ------------------------------------------------------------------ */

/* THE single exit. Every route out of the overview (Escape, the header toggle,
 * clicking a card, killing the last card, restoring) calls this, and it is the
 * only thing that swaps the stack back and restores terminal focus. Nothing
 * else in this file grabs focus. */
static void
overview_exit (EpimoneOverview *self)
{
  if (self->win != NULL)
    epimone_window_hide_overview (self->win);
}

/* Escape leaves the overview. An AdwDialog would give this for free; as an
 * ordinary widget in a stack it needs an explicit controller. */
static gboolean
overview_key_cb (GtkEventControllerKey *controller, guint keyval, guint keycode,
                 GdkModifierType state, gpointer user_data)
{
  (void) controller; (void) keycode; (void) state;

  if (keyval == GDK_KEY_Escape)
    {
      overview_exit (EPIMONE_OVERVIEW (user_data));
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

/* A card was activated, by click or by Enter on the focused card, both of which
 * GtkFlowBox funnels through child-activated.
 *
 * The card is deliberately NOT a GtkButton: nesting the close control's
 * GtkButton inside a card button breaks the close control outright. GtkButton's
 * click gesture runs in the capture phase, so the outer button swallows the
 * click and the inner one never fires, silently restoring/selecting the tab
 * instead of killing anything. libadwaita hits the same requirement and solves
 * it the same way: AdwTabThumbnail is a plain GtkWidget driven by gestures,
 * with the close GtkButton as a sibling overlay rather than a descendant of a
 * button (adw-tab-thumbnail.ui). Activation therefore comes from the flow box,
 * which also gives keyboard activation for free. */
static void
card_flow_activated_cb (GtkFlowBox      *box,
                        GtkFlowBoxChild *child,
                        gpointer         user_data)
{
  EpimoneOverview *self = user_data;
  GtkWidget *card = gtk_flow_box_child_get_child (child);
  CardInfo *ci = card != NULL
    ? g_object_get_data (G_OBJECT (card), "epi-card") : NULL;

  (void) box;
  if (ci == NULL || self->win == NULL)
    return;

  /* Anchor the way back on the card the user actually pressed, which is what
   * makes the tab appear to come out of that specific card. */
  overview_set_pending_anchor (self, ci->stack);

  /* One restore path, the same win.restore-group the D-Bus trigger used: it
   * already handles an already-on-screen group by selecting its tab, prunes dead
   * leaves, refuses when every pane is gone, and refuses on an instance mismatch. */
  gtk_widget_activate_action (GTK_WIDGET (self->win), "win.restore-group",
                              "t", ci->group_id);
  overview_exit (self);
}

/* Kill one session, logging (not propagating) a refusal: every kill in the
 * overview is best-effort per member. */
static void
overview_kill_session (guint64 sid)
{
  GError *err = NULL;

  if (!epimone_client_kill_session (sid, &err))
    {
      g_warning ("epimone: could not kill session %" G_GUINT64_FORMAT ": %s",
                 sid, err != NULL ? err->message : "unknown error");
      g_clear_error (&err);
    }
}

/* Second stage of the per-card kill, after the user has confirmed. */
static void
kill_confirmed_cb (AdwAlertDialog *dialog, GAsyncResult *result, gpointer user_data)
{
  EpimoneOverview *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (dialog, result);
  guint64 gid;
  guint64 sid;
  g_autoptr (GPtrArray) groups = NULL;

  if (g_strcmp0 (response, "kill") != 0)
    return;

  /* An orphan card's dialog carries a session id, not a group id: kill exactly
   * that session and nothing else. */
  sid = (guint64) GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog), "epi-sid"));
  if (sid != 0)
    {
      overview_kill_session (sid);
      overview_reload (self);
      return;
    }

  gid = (guint64) GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog), "epi-gid"));
  if (gid == 0)
    return;

  /* Kill every member. The daemon drops the group itself once the last member
   * goes (see the group lifetime rule in epimone-protocol.h), so there is
   * nothing to clean up here. */
  groups = epimone_client_list_groups (NULL, NULL);
  if (groups != NULL)
    {
      for (guint i = 0; i < groups->len; i++)
        {
          EpiGroupInfo *info = g_ptr_array_index (groups, i);

          if (info->id != gid)
            continue;
          for (guint m = 0; m < info->members->len; m++)
            overview_kill_session (g_array_index (info->members, guint64, m));
        }
    }

  overview_reload (self);
}

/* The one confirmation both kill routes (the card's × and the context menu's
 * Kill Session) funnel through; there is no path that kills without it.
 *
 * It must say exactly what is about to die: a mis-click here can end a job
 * that has been running for hours, so the prompt names the session, how many
 * panes go with it, what is actually running in them, and how long it has
 * been alive. */
static void
overview_confirm_kill (EpimoneOverview *self,
                       guint64          gid,
                       const char      *title,
                       const char      *procs,
                       const char      *age,
                       guint            panes)
{
  AdwAlertDialog *dialog;
  g_autofree char *body = NULL;

  if (gid == 0)
    return;

  body = g_strdup_printf ("“%s” has been running for %s.\n\n"
                          "Running now: %s\n"
                          "This ends %u pane%s and every process in them.\n\n"
                          "Detaching a tab leaves the programs running. Killing "
                          "stops them, and cannot be undone.",
                          title != NULL ? title : "This session",
                          age != NULL ? age : "some time",
                          procs != NULL ? procs : "unknown",
                          panes, panes == 1 ? "" : "s");

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new ("Kill this session?", NULL));
  adw_alert_dialog_set_body (dialog, body);
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel", "kill", "_Kill", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "kill", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  g_object_set_data (G_OBJECT (dialog), "epi-gid",
                     GUINT_TO_POINTER ((guint) gid));

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) kill_confirmed_cb, self);
}

/* Like overview_confirm_kill, but for a SINGLE detached session (an orphan
 * safety-net card). Stores the session id, not a group id, so kill_confirmed_cb
 * kills just this session and never touches its group's other, still-attached
 * panes. */
static void
overview_confirm_kill_session (EpimoneOverview *self,
                               guint64          sid,
                               const char      *title,
                               const char      *procs,
                               const char      *age)
{
  AdwAlertDialog *dialog;
  g_autofree char *body = NULL;

  if (sid == 0)
    return;

  body = g_strdup_printf ("“%s” has been running for %s.\n\n"
                          "Running now: %s\n"
                          "This ends this one detached session and every "
                          "process in it.\n\n"
                          "Detaching leaves the programs running. Killing "
                          "stops them, and cannot be undone.",
                          title != NULL ? title : "This session",
                          age != NULL ? age : "some time",
                          procs != NULL ? procs : "unknown");

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new ("Kill this session?", NULL));
  adw_alert_dialog_set_body (dialog, body);
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel", "kill", "_Kill", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "kill", ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  g_object_set_data (G_OBJECT (dialog), "epi-sid",
                     GUINT_TO_POINTER ((guint) sid));

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) kill_confirmed_cb, self);
}

static void
card_kill_cb (GtkButton *button, gpointer user_data)
{
  EpimoneOverview *self = user_data;
  GtkWidget *box = g_object_get_data (G_OBJECT (button), "epi-box");
  CardInfo *ci = box != NULL
    ? g_object_get_data (G_OBJECT (box), "epi-card") : NULL;

  if (ci == NULL)
    return;

  /* An orphan card kills exactly its own session, never its group id (which is
   * the still-attached parent tab). A group card kills the whole group. */
  if (ci->kill_session != 0)
    {
      overview_confirm_kill_session (self, ci->kill_session,
                                     g_object_get_data (G_OBJECT (box), "epi-title"),
                                     g_object_get_data (G_OBJECT (box), "epi-procs"),
                                     g_object_get_data (G_OBJECT (box), "epi-age"));
      return;
    }

  overview_confirm_kill (self, ci->group_id,
                         g_object_get_data (G_OBJECT (box), "epi-title"),
                         g_object_get_data (G_OBJECT (box), "epi-procs"),
                         g_object_get_data (G_OBJECT (box), "epi-age"),
                         GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (box),
                                                              "epi-panes")));
}

/* ------------------------------------------------------------------ *
 * the header's session-actions menu
 * ------------------------------------------------------------------ */

/* Classify every daemon group against this window and tally what each scoped
 * kill would end. Counts are computed fresh from the daemon so the menu's
 * enabled state and every dialog agree with reality at the moment they are
 * asked for. Any out-param may be NULL.
 *
 *   - detached: groups with no attached member (shared across all windows).
 *   - this window: groups whose tab lives in THIS window's tab view, derived
 *     live so "Move to New Window" needs no bookkeeping here.
 *   - total: every session the daemon holds.
 *
 * "*_sessions" tally member sessions; "*_groups" tally groups (tabs/cards). */

/* The session ids that currently have a client attached, as a set, from a
 * LIST result (NULL-tolerant). Caller unrefs. */
static GHashTable *
attached_sid_set (GPtrArray *sessions)
{
  GHashTable *set = g_hash_table_new (g_direct_hash, g_direct_equal);

  if (sessions != NULL)
    for (guint i = 0; i < sessions->len; i++)
      {
        EpiSessionInfo *si = g_ptr_array_index (sessions, i);

        if (si->attached)
          g_hash_table_add (set, GUINT_TO_POINTER ((guint) si->id));
      }
  return set;
}

static void
overview_tally (EpimoneOverview *self,
                guint *det_sessions, guint *det_groups,
                guint *win_sessions, guint *win_groups,
                guint *total_sessions)
{
  g_autoptr (GPtrArray) groups = epimone_client_list_groups (NULL, NULL);
  g_autoptr (GPtrArray) sessions = epimone_client_list_sessions (NULL);
  g_autoptr (GHashTable) attached_by_sid = attached_sid_set (sessions);
  g_autoptr (GHashTable) mine =
    g_hash_table_new (g_direct_hash, g_direct_equal);

  if (det_sessions) *det_sessions = 0;
  if (det_groups) *det_groups = 0;
  if (win_sessions) *win_sessions = 0;
  if (win_groups) *win_groups = 0;
  if (total_sessions) *total_sessions = sessions != NULL ? sessions->len : 0;

  if (self->win != NULL)
    {
      g_autoptr (GArray) ids =
        epimone_window_dup_attached_group_ids (self->win);

      for (guint i = 0; i < ids->len; i++)
        g_hash_table_add (mine,
          GUINT_TO_POINTER ((guint) g_array_index (ids, guint64, i)));
    }

  if (groups != NULL)
    for (guint i = 0; i < groups->len; i++)
      {
        EpiGroupInfo *info = g_ptr_array_index (groups, i);
        gboolean attached = FALSE;

        for (guint m = 0; m < info->members->len; m++)
          if (g_hash_table_contains (attached_by_sid,
                GUINT_TO_POINTER ((guint) g_array_index (info->members, guint64, m))))
            { attached = TRUE; break; }

        if (!attached)
          {
            if (det_groups) (*det_groups)++;
            if (det_sessions) *det_sessions += info->members->len;
          }
        if (g_hash_table_contains (mine, GUINT_TO_POINTER ((guint) info->id)))
          {
            if (win_groups) (*win_groups)++;
            if (win_sessions) *win_sessions += info->members->len;
          }
      }
}

/* Enable each scoped kill only when it has something to end; zero-count
 * entries are disabled. Recomputed every time the menu opens. */
static void
overview_menu_update_state (EpimoneOverview *self)
{
  guint det = 0, win = 0, total = 0;
  GAction *a;

  if (self->overview_actions == NULL)
    return;

  overview_tally (self, &det, NULL, &win, NULL, &total);

  a = g_action_map_lookup_action (G_ACTION_MAP (self->overview_actions),
                                  "kill-detached");
  if (G_IS_SIMPLE_ACTION (a))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (a), det > 0);
  a = g_action_map_lookup_action (G_ACTION_MAP (self->overview_actions),
                                  "kill-in-this-window");
  if (G_IS_SIMPLE_ACTION (a))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (a), win > 0);
  a = g_action_map_lookup_action (G_ACTION_MAP (self->overview_actions),
                                  "kill-all");
  if (G_IS_SIMPLE_ACTION (a))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (a), total > 0);
}

/* Kill every detached group's sessions, daemon-wide. Detached groups are
 * shared across windows, so this is deliberately not window-scoped. Attached
 * tabs anywhere are untouched. Runs mid-overview: the killed groups' cards
 * vanish and the grid reloads, but neither the window nor the overview closes. */
static void
overview_kill_detached_execute (EpimoneOverview *self)
{
  g_autoptr (GPtrArray) groups = epimone_client_list_groups (NULL, NULL);
  g_autoptr (GPtrArray) sessions = epimone_client_list_sessions (NULL);
  g_autoptr (GHashTable) attached_by_sid = attached_sid_set (sessions);

  if (groups != NULL)
    for (guint i = 0; i < groups->len; i++)
      {
        EpiGroupInfo *info = g_ptr_array_index (groups, i);
        gboolean attached = FALSE;

        for (guint m = 0; m < info->members->len; m++)
          if (g_hash_table_contains (attached_by_sid,
                GUINT_TO_POINTER ((guint) g_array_index (info->members, guint64, m))))
            { attached = TRUE; break; }
        if (attached)
          continue;

        for (guint m = 0; m < info->members->len; m++)
          overview_kill_session (g_array_index (info->members, guint64, m));
      }

  overview_reload (self);
}

static void
kill_detached_confirmed_cb (AdwAlertDialog *dialog, GAsyncResult *result,
                            gpointer user_data)
{
  EpimoneOverview *self = user_data;

  if (g_strcmp0 (adw_alert_dialog_choose_finish (dialog, result), "kill") == 0)
    overview_kill_detached_execute (self);
}

/* overview.kill-detached: the count is computed here, at open time, and names
 * exactly what dies and what does not. */
static void
overview_kill_detached_cb (GSimpleAction *action, GVariant *param,
                           gpointer user_data)
{
  EpimoneOverview *self = user_data;
  AdwAlertDialog *dialog;
  g_autofree char *heading = NULL;
  g_autofree char *button = NULL;
  guint det = 0;

  (void) action; (void) param;

  overview_tally (self, &det, NULL, NULL, NULL, NULL);
  if (det == 0)
    return;

  heading = g_strdup_printf ("Kill %u detached session%s?",
                             det, det == 1 ? "" : "s");
  button = g_strdup_printf ("Kill %u", det);

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, NULL));
  adw_alert_dialog_set_body (dialog,
    "Only sessions with no tab in any window. Open tabs are untouched.");
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel", "kill", button, NULL);
  adw_alert_dialog_set_response_appearance (dialog, "kill",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) kill_detached_confirmed_cb, self);
}

/* Kill the sessions of every group attached in THIS window. Other windows and
 * detached groups are untouched. The kill-all guard, scoped: the replacement
 * tab is created before this window's tabs die, so notify::n-pages never sees an
 * empty tab view and cannot close the window. */
static void
overview_kill_window_execute (EpimoneOverview *self)
{
  g_autoptr (GArray) my_ids = NULL;
  g_autoptr (GPtrArray) groups = NULL;
  g_autoptr (GArray) doomed = g_array_new (FALSE, FALSE, sizeof (guint64));
  g_autoptr (GHashTable) mine = g_hash_table_new (g_direct_hash, g_direct_equal);

  if (self->win == NULL)
    return;

  /* Snapshot this window's group ids and their sessions BEFORE the replacement
   * tab exists, so the fresh tab's group can never land on the kill list. */
  my_ids = epimone_window_dup_attached_group_ids (self->win);
  for (guint i = 0; i < my_ids->len; i++)
    g_hash_table_add (mine,
      GUINT_TO_POINTER ((guint) g_array_index (my_ids, guint64, i)));

  groups = epimone_client_list_groups (NULL, NULL);
  if (groups != NULL)
    for (guint i = 0; i < groups->len; i++)
      {
        EpiGroupInfo *info = g_ptr_array_index (groups, i);

        if (!g_hash_table_contains (mine, GUINT_TO_POINTER ((guint) info->id)))
          continue;
        for (guint m = 0; m < info->members->len; m++)
          {
            guint64 sid = g_array_index (info->members, guint64, m);

            g_array_append_val (doomed, sid);
          }
      }

  epimone_window_add_tab (self->win);   /* replacement first: the guard */

  for (guint i = 0; i < doomed->len; i++)
    overview_kill_session (g_array_index (doomed, guint64, i));

  overview_exit (self);
}

static void
kill_window_confirmed_cb (AdwAlertDialog *dialog, GAsyncResult *result,
                          gpointer user_data)
{
  EpimoneOverview *self = user_data;

  if (g_strcmp0 (adw_alert_dialog_choose_finish (dialog, result), "kill") == 0)
    overview_kill_window_execute (self);
}

/* overview.kill-in-this-window. */
static void
overview_kill_window_cb (GSimpleAction *action, GVariant *param,
                         gpointer user_data)
{
  EpimoneOverview *self = user_data;
  AdwAlertDialog *dialog;
  g_autofree char *heading = NULL;
  g_autofree char *button = NULL;
  guint win = 0;

  (void) action; (void) param;

  overview_tally (self, NULL, NULL, &win, NULL, NULL);
  if (win == 0)
    return;

  heading = g_strdup_printf ("Kill %u session%s in this window?",
                             win, win == 1 ? "" : "s");
  button = g_strdup_printf ("Kill %u", win);

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, NULL));
  adw_alert_dialog_set_body (dialog,
    "Only this window's tabs. Other windows and detached sessions are untouched.");
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel", "kill", button, NULL);
  adw_alert_dialog_set_response_appearance (dialog, "kill",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) kill_window_confirmed_cb, self);
}

/* Second stage of Kill All Sessions, after the user has confirmed.
 *
 * Order is what keeps the app alive: killing attached sessions collapses
 * every pane, each emptied page closes its tab, and a window whose tab view
 * empties CLOSES (epimone_window_notify_n_pages_cb), for every window at
 * once, which would end the process. A cleanup action must not be a quit
 * action, so THIS window gets its replacement tab while its old tabs still
 * exist; other windows' tabs die and those windows close, which is the
 * honest reading of "kill everything" started from here. */
static void
overview_kill_all_execute (EpimoneOverview *self)
{
  g_autoptr (GPtrArray) sessions = NULL;

  /* Snapshot the doomed ids BEFORE the replacement tab exists, so the fresh
   * session can never be on the list. */
  sessions = epimone_client_list_sessions (NULL);

  if (self->win != NULL)
    epimone_window_add_tab (self->win);

  if (sessions != NULL)
    for (guint i = 0; i < sessions->len; i++)
      overview_kill_session (((EpiSessionInfo *)
                              g_ptr_array_index (sessions, i))->id);

  /* Out through the single exit, immediately: every card in the grid now
   * depicts a dead session, so there is nothing to stay for. The panes
   * collapse behind the closing animation as their sockets hit EOF, the
   * daemon reaps each group with its last member, and the exit's completion
   * lands focus on the replacement tab's terminal. */
  overview_exit (self);
}

static void
kill_all_confirmed_cb (AdwAlertDialog *dialog, GAsyncResult *result,
                       gpointer user_data)
{
  EpimoneOverview *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (dialog, result);

  if (g_strcmp0 (response, "kill") != 0)
    return;

  overview_kill_all_execute (self);
}

/* overview.kill-all: everything the daemon holds, attached and detached
 * alike; deliberately not detached-only, which would appear to do nothing
 * when every session is attached. Same dialog conventions as
 * overview_confirm_kill: say exactly what dies, and nothing happens until
 * the destructive response is chosen. */
static void
overview_kill_all_cb (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneOverview *self = user_data;
  AdwAlertDialog *dialog;
  g_autofree char *heading = NULL;
  g_autofree char *button = NULL;
  guint total = 0;

  (void) action;
  (void) param;

  /* N = the daemon total, across every window plus detached. */
  overview_tally (self, NULL, NULL, NULL, NULL, &total);
  if (total == 0)
    return;   /* nothing to kill; confirming a no-op would be noise */

  heading = g_strdup_printf ("Kill all %u session%s?", total, total == 1 ? "" : "s");
  button = g_strdup_printf ("Kill all %u", total);

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new (heading, NULL));
  adw_alert_dialog_set_body (dialog,
    "Everything, across all windows, including detached sessions and running "
    "processes.");
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel",
                                  "kill", button, NULL);
  adw_alert_dialog_set_response_appearance (dialog, "kill",
                                            ADW_RESPONSE_DESTRUCTIVE);
  adw_alert_dialog_set_default_response (dialog, "cancel");
  adw_alert_dialog_set_close_response (dialog, "cancel");

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) kill_all_confirmed_cb, self);
}

static void
new_tab_cb (GtkButton *button, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  (void) button;
  if (self->win != NULL)
    gtk_widget_activate_action (GTK_WIDGET (self->win), "win.new-tab", NULL);
  overview_exit (self);
}

/* ------------------------------------------------------------------ *
 * the card context menu
 *
 * Right-click on a card. Three items, all acting on the state snapshotted into
 * self->menu_* when the menu opened (see the struct note): the same activation
 * as clicking the card body, Set Title, and the same kill-with-confirmation
 * as the card's × control. The actions live in one "card" group inserted on
 * the flow box; each card's popover resolves them by walking up from its
 * parent card.
 * ------------------------------------------------------------------ */

/* Same as activating the card through the flow box: restore a detached group,
 * select an attached one; win.restore-group already does the right one. The
 * anchor is re-looked-up by group id so a reload since popup cannot leave a
 * dangling card pointer. */
static void
card_action_activate_cb (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  (void) action; (void) param;
  if (self->menu_group == 0 || self->win == NULL)
    return;

  overview_set_pending_anchor (self,
                               overview_find_anchor_for_group (self,
                                                               self->menu_group));
  gtk_widget_activate_action (GTK_WIDGET (self->win), "win.restore-group",
                              "t", self->menu_group);
  overview_exit (self);
}

static void
rename_response_cb (AdwAlertDialog *dialog, GAsyncResult *result,
                    gpointer user_data)
{
  EpimoneOverview *self = user_data;
  const char *response = adw_alert_dialog_choose_finish (dialog, result);
  GtkWidget *entry;
  guint64 gid;
  g_autofree char *name = NULL;
  GError *err = NULL;

  if (g_strcmp0 (response, "rename") != 0)
    return;

  gid = (guint64) GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (dialog),
                                                       "epi-gid"));
  entry = adw_alert_dialog_get_extra_child (dialog);
  if (gid == 0 || entry == NULL)
    return;

  name = g_strdup (gtk_editable_get_text (GTK_EDITABLE (entry)));
  g_strstrip (name);

  /* Empty (or all-whitespace) means back to automatic, shell-provided titles. */
  if (!epimone_layout_rename_group (gid, name[0] != '\0' ? name : NULL, &err))
    {
      g_warning ("epimone: could not rename group %" G_GUINT64_FORMAT ": %s",
                 gid, err != NULL ? err->message : "unknown error");
      g_clear_error (&err);
    }

  /* Rebuild the captions. For an attached tab the reload reads the LIVE page
   * title, so the new name shows before the debounced blob sync has landed. */
  overview_reload (self);
}

static void
card_action_rename_cb (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneOverview *self = user_data;
  AdwAlertDialog *dialog;
  GtkWidget *entry;

  (void) action; (void) param;
  if (self->menu_group == 0)
    return;

  dialog = ADW_ALERT_DIALOG (adw_alert_dialog_new ("Set Title", NULL));
  adw_alert_dialog_set_body (dialog,
                             "Leave empty to use the automatic title.");

  entry = gtk_entry_new ();
  gtk_editable_set_text (GTK_EDITABLE (entry), self->menu_custom ?: "");
  /* The automatic title as the placeholder, so what "empty" returns to is
   * visible while choosing. */
  if (self->menu_shell != NULL && self->menu_shell[0] != '\0')
    gtk_entry_set_placeholder_text (GTK_ENTRY (entry), self->menu_shell);
  gtk_entry_set_activates_default (GTK_ENTRY (entry), TRUE);
  adw_alert_dialog_set_extra_child (dialog, entry);

  /* Response IDs keep the "rename" spelling; they are code identifiers
   * (rename_response_cb checks them), not user-visible text. */
  adw_alert_dialog_add_responses (dialog, "cancel", "_Cancel",
                                  "rename", "_Set", NULL);
  adw_alert_dialog_set_response_appearance (dialog, "rename",
                                            ADW_RESPONSE_SUGGESTED);
  adw_alert_dialog_set_default_response (dialog, "rename");
  adw_alert_dialog_set_close_response (dialog, "cancel");
  g_object_set_data (G_OBJECT (dialog), "epi-gid",
                     GUINT_TO_POINTER ((guint) self->menu_group));

  adw_alert_dialog_choose (dialog, GTK_WIDGET (self), NULL,
                           (GAsyncReadyCallback) rename_response_cb, self);
}

/* The live page for the card the menu was opened on, or NULL when its group is
 * detached (nothing on screen to act on). Looked up by session id rather than
 * held as a pointer, so a grid rebuild while the menu is up cannot dangle. */
static EpimonePage *
card_menu_live_page (EpimoneOverview *self)
{
  if (self->win == NULL || self->menu_session == 0)
    return NULL;
  return epimone_window_find_page_for_session (self->win, self->menu_session);
}

/* Attach in New Window (detached cards): the same restore the card body does,
 * pointed at a NEW window instead of this one. One restore path, one blob
 * format, one set of guards. */
static void
card_action_attach_new_window_cb (GSimpleAction *action, GVariant *param,
                                  gpointer user_data)
{
  EpimoneOverview *self = user_data;
  GtkApplication *app;
  GtkWidget *window;
  GError *err = NULL;

  (void) action; (void) param;
  if (self->menu_group == 0 || self->win == NULL)
    return;
  app = gtk_window_get_application (GTK_WINDOW (self->win));
  if (app == NULL)
    return;

  window = epimone_window_new (ADW_APPLICATION (app));
  if (!epimone_layout_restore_group (EPIMONE_WINDOW (window), self->menu_group,
                                     &err))
    {
      g_warning ("epimone: could not restore group %" G_GUINT64_FORMAT
                 " into a new window: %s", self->menu_group,
                 err != NULL ? err->message : "unknown error");
      g_clear_error (&err);
      /* Nothing was built, so do not leave an empty window behind. */
      gtk_window_destroy (GTK_WINDOW (window));
      return;
    }
  gtk_window_present (GTK_WINDOW (window));

  /* Then exactly what a normal Attach does: out through the single exit. */
  overview_set_pending_anchor (self,
                               overview_find_anchor_for_group (self,
                                                               self->menu_group));
  overview_exit (self);
}

/* Move to New Window (attached cards): the tab menu's transfer, verbatim;
 * epimone_window_move_page_to_new_window keeps the sessions attached. */
static void
card_action_move_new_window_cb (GSimpleAction *action, GVariant *param,
                                gpointer user_data)
{
  EpimoneOverview *self = user_data;
  EpimonePage *page = card_menu_live_page (self);

  (void) action; (void) param;
  if (page == NULL)
    return;
  epimone_window_move_page_to_new_window (page);
  overview_exit (self);
}

/* Detach Tab (attached cards): an ordinary detach. The card stays in the grid
 * and re-renders as detached, so the overview is not exited; the point of
 * doing it from here is to keep working in the overview. */
static void
card_action_close_tab_cb (GSimpleAction *action, GVariant *param,
                          gpointer user_data)
{
  EpimoneOverview *self = user_data;
  EpimonePage *page = card_menu_live_page (self);

  (void) action; (void) param;
  if (page == NULL)
    return;
  epimone_window_close_page (page);
  /* Rebuild so the card picks up its new detached state and badge. */
  overview_reload (self);
}

static void
card_action_kill_cb (GSimpleAction *action, GVariant *param, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  (void) action; (void) param;
  overview_confirm_kill (self, self->menu_group, self->menu_title,
                         self->menu_procs, self->menu_age, self->menu_panes);
}

/* GTK does not hand focus back when a popover closes; left to its fallback it
 * lands on the first focusable widget, which is the exact mechanism of the
 * spurious-tab bug (see the new-tab button notes in epimone-window.c). NOT the
 * terminal; epimone_window_hide_overview stays the single exit that restores
 * terminal focus. The overview's toolbar view is focusable and inert, so after
 * a dismissal the Escape controller keeps seeing keys while Enter activates
 * nothing. Skipped when the menu closed because an item is exiting the
 * overview, so the close transition's focus hand-off is not fought. */
static void
card_menu_closed_cb (GtkPopover *popover, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  (void) popover;
  if (self->is_open && self->overview_ui != NULL)
    gtk_widget_grab_focus (self->overview_ui);
}

/* A menu item that is shown only while its action is enabled. */
static void
overview_menu_append_state_item (GMenu      *section,
                                 const char *label,
                                 const char *action)
{
  GMenuItem *item = g_menu_item_new (label, action);

  g_menu_item_set_attribute (item, "hidden-when", "s", "action-disabled");
  g_menu_append_item (section, item);
  g_object_unref (item);
}

/* Enable exactly the items the card's state supports, which is what decides
 * which of them the menu shows. */
static void
overview_menu_set_state (EpimoneOverview *self, gboolean attached)
{
  static const struct { const char *name; gboolean when_attached; } items[] = {
    { "attach-new-window", FALSE },
    { "move-new-window",   TRUE  },
    { "close-tab",         TRUE  },
  };

  if (self->card_actions == NULL)
    return;

  for (guint i = 0; i < G_N_ELEMENTS (items); i++)
    {
      GAction *action = g_action_map_lookup_action (G_ACTION_MAP (self->card_actions),
                                                    items[i].name);

      if (G_IS_SIMPLE_ACTION (action))
        g_simple_action_set_enabled (G_SIMPLE_ACTION (action),
                                     items[i].when_attached == attached);
    }
}

/* Right-click on a card: snapshot what the menu items need, then pop the
 * card's menu (created lazily, parented to the card box so it is destroyed
 * with the card and positioned in its coordinates). */
static void
card_menu_popup_for_box (EpimoneOverview *self, GtkWidget *box,
                         double x, double y)
{
  CardInfo *ci = g_object_get_data (G_OBJECT (box), "epi-card");
  GtkWidget *menu = g_object_get_data (G_OBJECT (box), "epi-menu");

  if (ci == NULL)
    return;

  self->menu_group = ci->group_id;
  self->menu_panes = GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (box),
                                                          "epi-panes"));
  /* Which state's items to show, and a session to find the live page by. The
   * whole snapshot is BY VALUE (ids and owned strings, never a widget or
   * CardInfo pointer) precisely so a grid rebuild while the menu is up cannot
   * leave an action chasing freed memory; that is the reason this pattern
   * exists. */
  self->menu_attached =
    g_object_get_data (G_OBJECT (box), "epi-attached") != NULL &&
    GPOINTER_TO_UINT (g_object_get_data (G_OBJECT (box), "epi-attached")) != 0;
  self->menu_session = 0;
  if (ci->leaves != NULL && ci->leaves->len > 0)
    {
      EpimoneLayoutLeaf *leaf = g_ptr_array_index (ci->leaves, 0);

      self->menu_session = leaf->session;
    }
  g_free (self->menu_title);
  self->menu_title = g_strdup (g_object_get_data (G_OBJECT (box), "epi-title"));
  g_free (self->menu_shell);
  self->menu_shell = g_strdup (g_object_get_data (G_OBJECT (box), "epi-shell"));
  g_free (self->menu_custom);
  self->menu_custom = g_strdup (g_object_get_data (G_OBJECT (box), "epi-custom"));
  g_free (self->menu_procs);
  self->menu_procs = g_strdup (g_object_get_data (G_OBJECT (box), "epi-procs"));
  g_free (self->menu_age);
  self->menu_age = g_strdup (g_object_get_data (G_OBJECT (box), "epi-age"));

  if (menu == NULL)
    {
      GMenu *model = g_menu_new ();
      GMenu *section = g_menu_new ();

      /* The first item is the card-body click. One label for both states:
       * the Detached badge on the thumbnail already says which state the card
       * is in, so the menu does not repeat it. No ellipses: the same
       * convention as the pane menu's Kill Pane, whose confirmation dialog is
       * not announced either. */
      g_menu_append (section, "_Attach", "card.activate");
      /* The two new-window items are one slot in two states: whichever does
       * not apply is DISABLED and therefore hidden, the same
       * hidden-when="action-disabled" pair the tab menu's Pin/Unpin uses. A
       * detached group has no live tab to move, and an attached one is
       * already somewhere, so exactly one of these ever shows. */
      overview_menu_append_state_item (section, "Attach in New _Window",
                                       "card.attach-new-window");
      overview_menu_append_state_item (section, "_Move to New Window",
                                       "card.move-new-window");
      /* Same name as the pane menu's item: same dialog, same custom-title
       * mechanism. No ellipsis, per the app-wide convention (no ellipsis on
       * menu items, anywhere). */
      g_menu_append (section, "Set Title", "card.rename");
      g_menu_append_section (model, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      section = g_menu_new ();
      /* Close DETACHES, as everywhere else; only meaningful with a live tab. */
      overview_menu_append_state_item (section, "_Detach Tab", "card.close-tab");
      g_menu_append (section, "_Kill this session", "card.kill");
      g_menu_append_section (model, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      menu = gtk_popover_menu_new_from_model (G_MENU_MODEL (model));
      g_object_unref (model);
      gtk_popover_set_has_arrow (GTK_POPOVER (menu), FALSE);
      gtk_widget_set_halign (menu, GTK_ALIGN_START);
      gtk_widget_set_parent (menu, box);
      g_signal_connect (menu, "closed", G_CALLBACK (card_menu_closed_cb), self);
      g_object_set_data (G_OBJECT (box), "epi-menu", menu);
    }

  /* Just before the menu reads them, as the tab menu does for Pin/Unpin. */
  overview_menu_set_state (self, self->menu_attached);

  gtk_popover_set_pointing_to (GTK_POPOVER (menu),
                               &(const GdkRectangle) { (int) x, (int) y, 1, 1 });
  gtk_popover_popup (GTK_POPOVER (menu));
}

static void
card_right_click_cb (GtkGestureClick *gesture, int n_press, double x, double y,
                     gpointer user_data)
{
  EpimoneOverview *self = user_data;
  GtkWidget *box =
    gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));

  (void) n_press;
  if (g_object_get_data (G_OBJECT (box), "epi-card") == NULL)
    return;
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
  card_menu_popup_for_box (self, box, x, y);
}

/* ------------------------------------------------------------------ *
 * offscreen thumbnail rendering
 * ------------------------------------------------------------------ */

static void render_pump (EpimoneOverview *self);
static void render_start (gpointer data);

/* Cancel whichever settle timers are outstanding. */
static void
settle_cancel (EpimoneOverview *self)
{
  g_clear_handle_id (&self->settle_backstop, g_source_remove);
  g_clear_handle_id (&self->settle_quiet, g_source_remove);
  g_clear_handle_id (&self->feed_timeout, g_source_remove);
  self->fed = FALSE;
}

/* Abandon all in-flight rendering: stop every timer, empty the queue, and clear
 * the busy flag.
 *
 * MUST be called before the cards are destroyed. The queue holds BORROWED
 * CardInfo pointers, owned by the card widgets, and each scheduled timer is
 * about to dereference the head of that queue, so a reload that rebuilds the
 * grid while a render is in flight leaves the pipeline walking freed memory.
 * Not a theoretical hazard: symptoms range from blank thumbnails ("0/4 panes"
 * composed from garbage state) and cards that never render (`rendering` stays
 * set, and queue_visible_renders only pumps when it is clear) to an outright
 * abort. Open, close before the thumbnails finish, reopen is enough to hit
 * it. */
static void
render_abort (EpimoneOverview *self)
{
  /* The next pass cannot assume the terminal is still at the last grid. */
  self->last_cols = 0;
  self->last_rows = 0;
  settle_cancel (self);
  g_clear_handle_id (&self->pump_timeout, g_source_remove);
  if (self->pending != NULL)
    g_queue_clear (self->pending);
  self->rendering = FALSE;
}

/* The physical-pixel size a card's thumbnail is composed at: the card box times
 * the display scale factor.
 *
 * Composing at the output resolution is what keeps the text legible. The scale
 * is applied to the NODE TREE rather than to a finished bitmap, so GSK
 * rasterises glyphs at that resolution instead of downsampling already-rendered
 * text (the mechanism AdwTabOverview gets for free from GtkWidgetPaintable).
 * Composing at exactly the card size also means the finished texture maps 1:1
 * onto the card, with nothing to up- or downscale at paint time. */
static void
overview_thumb_pixel_size (EpimoneOverview *self, int *out_w, int *out_h,
                           int *out_scale)
{
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkSurface *surface = native != NULL ? gtk_native_get_surface (native) : NULL;
  int scale = surface != NULL ? gdk_surface_get_scale_factor (surface) : 1;

  if (scale < 1)
    scale = 1;
  *out_scale = scale;
  *out_w = MAX (1, self->thumb_w * scale);
  *out_h = MAX (1, self->thumb_h * scale);
}

/* Compose every captured pane into one texture and hand it to the card.
 *
 * Each pane's node is in its own terminal-local coordinates, so it is translated
 * to its fractional rect within the card and scaled to fill it. The scale is
 * per-axis: the grids were derived from the same fractions
 * (epimone_layout_blob_geometry), so the two axes agree to within a rounded
 * cell and filling the rect exactly is what avoids seams of background showing
 * through between panes. */
static void
render_compose (EpimoneOverview *self, CardInfo *ci)
{
  GskRenderer *renderer =
    gtk_native_get_renderer (gtk_widget_get_native (GTK_WIDGET (self)));
  GtkSnapshot *snapshot;
  GskRenderNode *node;
  int W, H, scale;
  gint64 t0 = g_get_monotonic_time ();
  guint drawn = 0;

  if (renderer == NULL || ci->nodes == NULL || ci->nodes->len == 0)
    {
      g_debug ("thumb: group %" G_GUINT64_FORMAT " compose bailed renderer=%s nodes=%u",
               ci->group_id, renderer ? "yes" : "NULL",
               ci->nodes != NULL ? ci->nodes->len : 0);
      card_nodes_clear (ci);
      return;
    }

  overview_thumb_pixel_size (self, &W, &H, &scale);
  snapshot = gtk_snapshot_new ();

  /* Behind the panes: the same pure black the pane dividers sit on in a live tab
   * (see EPIMONE_CSS in main.c), so any rounding gap reads as divider rather
   * than as a bright band. */
  gtk_snapshot_append_color (snapshot,
                             &(GdkRGBA) { 0.0f, 0.0f, 0.0f, 1.0f },
                             &GRAPHENE_RECT_INIT (0, 0, (float) W, (float) H));

  for (guint i = 0; i < ci->nodes->len && i < ci->leaves->len; i++)
    {
      GskRenderNode *leaf_node = g_ptr_array_index (ci->nodes, i);
      EpimoneLayoutLeaf *leaf = g_ptr_array_index (ci->leaves, i);
      graphene_rect_t bounds;
      float rx, ry, rw, rh;

      if (leaf_node == NULL)
        continue;

      gsk_render_node_get_bounds (leaf_node, &bounds);
      if (bounds.size.width <= 0 || bounds.size.height <= 0)
        continue;

      rx = (float) (leaf->x * W);
      ry = (float) (leaf->y * H);
      rw = (float) (leaf->w * W);
      rh = (float) (leaf->h * H);

      gtk_snapshot_push_clip (snapshot, &GRAPHENE_RECT_INIT (rx, ry, rw, rh));
      gtk_snapshot_translate (snapshot, &GRAPHENE_POINT_INIT (rx, ry));
      gtk_snapshot_scale (snapshot, rw / bounds.size.width,
                          rh / bounds.size.height);
      gtk_snapshot_append_node (snapshot, leaf_node);
      gtk_snapshot_pop (snapshot);
      drawn++;
    }

  /* Dividers, one physical pixel wide: the live divider is 2 logical px, which
   * at card scale would be a fat band, so a hairline in the same colour
   * (main.c's rgba(255,255,255,0.5)) is the honest reduction. */
  if (ci->seams != NULL)
    {
      const GdkRGBA seam_rgba = { 1.0f, 1.0f, 1.0f, 0.5f };

      for (guint i = 0; i < ci->seams->len; i++)
        {
          EpimoneLayoutSeam *seam = g_ptr_array_index (ci->seams, i);
          graphene_rect_t r;

          if (seam->vertical)
            r = GRAPHENE_RECT_INIT ((float) (seam->x * W) - 0.5f,
                                    (float) (seam->y * H),
                                    1.0f, (float) (seam->h * H));
          else
            r = GRAPHENE_RECT_INIT ((float) (seam->x * W),
                                    (float) (seam->y * H) - 0.5f,
                                    (float) (seam->w * W), 1.0f);
          gtk_snapshot_append_color (snapshot, &seam_rgba, &r);
        }
    }

  if (drawn == 0)
    {
      /* Nothing usable was captured, so there is no thumbnail to deliver; an
       * empty rectangle would read as a broken card. Put the card BACK on the
       * queue rather than leaving it on its placeholder with nothing scheduled to
       * try again: "silently keeps the placeholder" was itself the bug that made
       * cards look permanently unloaded. */
      gtk_snapshot_free_to_node (snapshot);
      card_nodes_clear (ci);
      ci->cur_leaf = 0;
      if (ci->attempts < RENDER_MAX_ATTEMPTS)
        {
          ci->attempts++;
          g_debug ("thumb: group %" G_GUINT64_FORMAT " captured no pane content; "
                   "re-queueing (attempt %u)", ci->group_id, ci->attempts);
          g_queue_push_tail (self->pending, ci);
        }
      else
        g_warning ("epimone: could not render a thumbnail for session group %"
                   G_GUINT64_FORMAT " after %d attempts; it keeps its placeholder",
                   ci->group_id, RENDER_MAX_ATTEMPTS);
      return;
    }

  node = gtk_snapshot_free_to_node (snapshot);
  if (node != NULL)
    {
      GdkTexture *tex = gsk_renderer_render_texture (renderer, node, NULL);

      if (tex != NULL)
        {
          gtk_picture_set_paintable (GTK_PICTURE (ci->picture),
                                     GDK_PAINTABLE (tex));
          gtk_stack_set_visible_child_name (GTK_STACK (ci->stack), "picture");
          ci->rendered = TRUE;
          /* Into the cache, replacing whatever this group showed before: the
           * next reload starts from this texture instead of a placeholder. */
          g_hash_table_insert (self->thumb_cache,
                               GSIZE_TO_POINTER ((gsize) ci->cache_key),
                               g_object_ref (tex));
          g_debug ("thumb: group %" G_GUINT64_FORMAT " composed %u/%u pane(s) "
                   "into %dx%d texture for a %dx%d card (scale %d) in %.1f ms",
                   ci->group_id, drawn, ci->leaves->len,
                   gdk_texture_get_width (tex), gdk_texture_get_height (tex),
                   self->thumb_w, self->thumb_h, scale,
                   (g_get_monotonic_time () - t0) / 1000.0);
          g_object_unref (tex);
        }
      else
        g_debug ("thumb: group %" G_GUINT64_FORMAT " render_texture gave NULL",
                 ci->group_id);
      gsk_render_node_unref (node);
    }

  /* The nodes have served their purpose; a card can hold several full-size pane
   * node trees, so they go as soon as the texture exists. */
  card_nodes_clear (ci);
}

/* One pane has settled: capture it, then either move to the next pane of the
 * same card or compose the card. */
static void
render_finish (gpointer data)
{
  EpimoneOverview *self = data;
  CardInfo *ci = g_queue_peek_head (self->pending);
  int w, h;

  settle_cancel (self);

  if (ci == NULL)
    {
      g_debug ("thumb: render_finish found an empty queue; chain ends");
      self->rendering = FALSE;
      return;
    }

  w = gtk_widget_get_width (self->thumb_term);
  h = gtk_widget_get_height (self->thumb_term);

  if (w <= 0 || h <= 0)
    {
      /* The terminal has not been allocated at this pane's grid yet, so there is
       * nothing to capture. Recording a NULL node here is what used to strand a
       * card on its placeholder for good: every pane came back empty, the compose
       * found nothing to draw, and nothing ever re-queued the card. Give the
       * layout another frame and try this same pane again instead. */
      if (ci->attempts < RENDER_MAX_ATTEMPTS)
        {
          ci->attempts++;
          g_debug ("thumb: group %" G_GUINT64_FORMAT " pane %u had no allocation "
                   "(%dx%d); retrying (attempt %u)", ci->group_id, ci->cur_leaf + 1,
                   w, h, ci->attempts);
          self->pump_timeout = g_timeout_add_once (16, render_start, self);
          return;
        }
      /* Out of attempts: fall through and record the miss, so the card finishes
       * rather than spinning. */
    }

  if (w > 0 && h > 0 && ci->nodes != NULL)
    {
      GtkSnapshot *snapshot = gtk_snapshot_new ();

      /* Captured unscaled, in the pane's own coordinates: render_compose applies
       * the placement and scale once it knows every pane's bounds. */
      gdk_paintable_snapshot (self->thumb_paintable, snapshot, w, h);
      g_ptr_array_add (ci->nodes, gtk_snapshot_free_to_node (snapshot));
    }
  else if (ci->nodes != NULL)
    {
      g_ptr_array_add (ci->nodes, NULL);
    }

  /* This pane is committed: fold its signature (from render_feed's peek) into
   * the card's. Runs once per pane; the retry path above returns before here. */
  ci->sig = sig_fold_u64 (ci->sig, self->pane_sig);
  ci->sig_panes++;

  ci->cur_leaf++;
  if (ci->leaves != NULL && ci->cur_leaf < ci->leaves->len)
    {
      /* Same card, next pane. Back to the main loop so the grid stays
       * responsive between panes as well as between cards. */
      self->pump_timeout = g_timeout_add_once (1, render_start, self);
      return;
    }

  g_queue_pop_head (self->pending);
  render_compose (self, ci);

  /* On a successful compose (render_compose set ci->rendered and cached the
   * texture) record the signature of exactly what was rendered, so the next
   * open can skip re-rendering this card while its content is unchanged. Only
   * when every pane contributed, so a partial render never poses as complete. */
  if (ci->rendered && ci->leaves != NULL && ci->sig_panes == ci->leaves->len)
    {
      guint64 *v = g_new (guint64, 1);

      *v = ci->sig;
      g_hash_table_insert (self->thumb_sig,
                           GSIZE_TO_POINTER ((gsize) ci->cache_key), v);
    }

  /* Anything that went wrong leaves the placeholder showing, which is the
   * required fallback rather than an empty card. */
  render_pump (self);
}

/* Render one PANE of the card at the head of the queue, in two steps: set the
 * grid and let a layout cycle run, then PEEK and feed it. Which pane is decided
 * by ci->cur_leaf; render_finish advances it. */

/* Which pane the card is currently on, or NULL. */
static EpimoneLayoutLeaf *
render_current_leaf (CardInfo *ci)
{
  if (ci == NULL || ci->leaves == NULL || ci->cur_leaf >= ci->leaves->len)
    return NULL;
  return g_ptr_array_index (ci->leaves, ci->cur_leaf);
}

/* Second step: the grid has been re-laid-out, so now fill it. */
static void
render_feed (gpointer data)
{
  EpimoneOverview *self = data;
  CardInfo *ci = g_queue_peek_head (self->pending);
  EpimoneLayoutLeaf *leaf = render_current_leaf (ci);
  g_autofree guint8 *tail = NULL;
  gsize tail_len = 0;
  guint64 total = 0;

  self->feed_timeout = 0;

  if (leaf == NULL)
    {
      g_debug ("thumb: render_feed found no pane to draw; stopping");
      self->rendering = FALSE;
      return;
    }

  tail = epimone_client_peek_session (leaf->session, PEEK_BYTES,
                                      &tail_len, &total, NULL);
  /* Signature of exactly this pane's rendered content; render_finish folds it
   * into the card's signature when the pane commits. Same computation as
   * card_current_sig so a later open can compare and skip an unchanged card. */
  self->pane_sig = pane_signature (leaf->session, leaf->cols, leaf->rows,
                                   tail, tail_len);
  g_debug ("thumb: group %" G_GUINT64_FORMAT " pane %u/%u session %"
           G_GUINT64_FORMAT " peek=%" G_GSIZE_FORMAT " bytes (ring holds %"
           G_GUINT64_FORMAT ") grid %ux%u rect %.2f,%.2f %.2fx%.2f",
           ci->group_id, ci->cur_leaf + 1, ci->leaves->len, leaf->session,
           tail_len, total, leaf->cols, leaf->rows,
           leaf->x, leaf->y, leaf->w, leaf->h);

  if (tail != NULL && tail_len > 0)
    vte_terminal_feed (VTE_TERMINAL (self->thumb_term),
                       (const char *) tail, (gssize) tail_len);

  /* PEEK's total_size earns exactly one thing: a hint that the thumbnail is the tail
   * of something longer, not the whole session. A leading ellipsis row costs nothing
   * and reads correctly at card size, where any richer indicator would not. */
  if (total > (guint64) tail_len)
    vte_terminal_feed (VTE_TERMINAL (self->thumb_term), "\033[H\033[2K⋯\r\n", -1);

  /* Snapshot once VTE has been quiet for a moment, or at the backstop, whichever
   * comes first. Only from here on may contents-changed arm the quiet timer. */
  self->fed = TRUE;
  self->fed_len = tail_len;
  self->settle_backstop = g_timeout_add_once (SETTLE_MS, render_finish, self);
}

/* First step: geometry only. The grid is set and a layout cycle is allowed to run
 * before anything is fed, because the snapshot has to happen at the pane's real
 * allocation. Doing both in one step raced: the contents-changed quiet timer could
 * fire before the resize had been allocated, and the thumbnail came out with the
 * previous pane's shape (observed 720x432 for a 90x28 grid; queue_resize itself
 * is fine, measured 810x504 once a layout pass runs). */
static void
render_start (gpointer data)
{
  EpimoneOverview *self = data;
  CardInfo *ci = g_queue_peek_head (self->pending);
  EpimoneLayoutLeaf *leaf = render_current_leaf (ci);

  self->pump_timeout = 0;

  if (leaf == NULL)
    {
      g_debug ("thumb: render_start found no pane to draw (queue=%u); stopping",
               g_queue_get_length (self->pending));
      self->rendering = FALSE;
      return;
    }

  vte_terminal_reset (VTE_TERMINAL (self->thumb_term), TRUE, TRUE);

  self->fed = FALSE;

  if (leaf->cols > 0 && leaf->rows > 0 &&
      (leaf->cols != self->last_cols || leaf->rows != self->last_rows))
    {
      vte_terminal_set_size (VTE_TERMINAL (self->thumb_term),
                             (glong) leaf->cols, (glong) leaf->rows);
      /* The host does not measure this child, so a change in its natural size does
       * not invalidate the parent's layout by itself; without this the terminal
       * keeps the previous pane's allocation and the thumbnail comes out the wrong
       * shape. */
      gtk_widget_queue_resize (self->thumb_term);
      self->last_cols = leaf->cols;
      self->last_rows = leaf->rows;

      /* One frame for the allocation to land, then fill it. Tracked, so a finish
       * cannot leave it pending to feed-and-double-start the NEXT pane. */
      self->feed_timeout = g_timeout_add_once (16, render_feed, self);
      return;
    }

  /* Same grid as the pane just drawn, so the terminal is ALREADY allocated
   * correctly and there is nothing to wait for. Skipping the frame saves 16 ms on
   * every such pane, which is most of them within one tab; a card's panes usually
   * share a grid. If the allocation turns out to be missing anyway, render_finish
   * retries rather than producing a blank card. */
  render_feed (self);
}

/* The quiet period elapsed. That alone does not prove the fed bytes landed:
 * VTE parses on its own timer, so the contents-changed that armed this quiet
 * can still be the one from render_feed's reset, arriving after `fed` was set,
 * with the actual content still unparsed (observed: one card of two came out
 * blank that way). If content was fed but the grid is still empty, this was
 * the reset's quiet: decline, and let a later contents-changed or the
 * backstop finish the card. */
static void
render_finish_quiet (gpointer data)
{
  EpimoneOverview *self = data;

  self->settle_quiet = 0;

  if (self->fed_len > 0)
    {
      g_autofree char *text =
        vte_terminal_get_text_format (VTE_TERMINAL (self->thumb_term),
                                      VTE_FORMAT_TEXT);

      if (text == NULL || g_strstrip (text)[0] == '\0')
        return;
    }

  render_finish (data);
}

/* VTE says the grid changed: (re)start the short quiet period. A burst of changes
 * keeps pushing it out, so the snapshot lands just after the feed has settled
 * instead of after a fixed guess. */
static void
thumb_contents_changed_cb (VteTerminal *term, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  (void) term;
  /* Ignore the churn from render_start's reset/resize: a quiet period armed
   * before the feed snapshots an empty grid. */
  if (!self->rendering || !self->fed)
    return;
  g_clear_handle_id (&self->settle_quiet, g_source_remove);
  self->settle_quiet = g_timeout_add_once (SETTLE_QUIET_MS,
                                           render_finish_quiet, self);
}

/* Name every card and whether it has a thumbnail yet. Kept for debugging
 * stranded placeholders: per-event counters can all read clean while the grid
 * is visibly wrong, and only a per-card roll-call shows which card never
 * arrived. */
static void
render_report (EpimoneOverview *self, const char *when)
{
  GtkWidget *child;
  GString *out = g_string_new (NULL);

  for (int s = 0; s < 1; s++)
    for (child = gtk_widget_get_first_child (overview_flow_at (self, s));
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    {
      GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
      CardInfo *ci = card != NULL
        ? g_object_get_data (G_OBJECT (card), "epi-card") : NULL;

      if (ci == NULL)
        continue;
      g_string_append_printf (out, " [g%" G_GUINT64_FORMAT " %s leaves=%u vis=%s]",
                              ci->group_id,
                              ci->rendered ? "OK" : "PLACEHOLDER",
                              ci->leaves != NULL ? ci->leaves->len : 0,
                              ci->stack != NULL
                                ? gtk_stack_get_visible_child_name (GTK_STACK (ci->stack))
                                : "?");
    }
  g_debug ("thumb: %s:%s", when, out->str);
  g_string_free (out, TRUE);
}

/* Is any card currently showing its placeholder art (rather than a texture,
 * cached or fresh)? Decides how urgent the pipeline is. */
static gboolean
overview_any_placeholder (EpimoneOverview *self)
{
  GtkWidget *child;

  for (int s = 0; s < 1; s++)
    for (child = gtk_widget_get_first_child (overview_flow_at (self, s));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
      {
        GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
        CardInfo *ci = card != NULL
          ? g_object_get_data (G_OBJECT (card), "epi-card") : NULL;

        if (ci != NULL && ci->stack != NULL &&
            g_strcmp0 (gtk_stack_get_visible_child_name (GTK_STACK (ci->stack)),
                       "placeholder") == 0)
          return TRUE;
      }
  return FALSE;
}

static void
render_pump (EpimoneOverview *self)
{
  guint delay = 1;

  if (g_queue_is_empty (self->pending))
    {
      self->rendering = FALSE;
      render_report (self, "queue drained");
      return;
    }
  self->rendering = TRUE;
  {
    CardInfo *ci = g_queue_peek_head (self->pending);

    if (ci != NULL)
      {
        ci->cur_leaf = 0;
        ci->sig = SIG_FNV_OFFSET;
        ci->sig_panes = 0;
        card_nodes_clear (ci);
      }
  }

  /* When every card already has a texture (cache hit), what remains is an
   * invisible refresh, so it must not compete with the zoom transition for
   * main-loop time. A placeholder anywhere means the user is looking at
   * missing art, and filling it outranks animation smoothness. */
  if (self->animating && !overview_any_placeholder (self))
    delay = TRANSITION_DURATION + 80;

  /* Back to the main loop between cards so the grid stays scrollable. */
  self->pump_timeout = g_timeout_add_once (delay, render_start, self);
}

/* Is this card near enough to the viewport to be worth rendering?
 *
 * GtkFlowBox does not virtualise, so every card exists and is mapped whether or
 * not it is on screen; laziness has to be decided here rather than relying on map.
 * A margin of one viewport height is included so scrolling finds them ready. */
static gboolean
card_is_near_viewport (EpimoneOverview *self, GtkWidget *card)
{
  GtkAdjustment *adj;
  double top, bottom;
  graphene_rect_t bounds;

  if (self->scroller == NULL)
    return TRUE;
  adj = gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (self->scroller));
  if (adj == NULL)
    return TRUE;

  if (!gtk_widget_compute_bounds (card, self->grid_box, &bounds))
    return TRUE;

  top = gtk_adjustment_get_value (adj) - gtk_adjustment_get_page_size (adj);
  bottom = gtk_adjustment_get_value (adj) + 2 * gtk_adjustment_get_page_size (adj);
  return bounds.origin.y + bounds.size.height >= top && bounds.origin.y <= bottom;
}

/* Queue every not-yet-rendered detached card that is near the viewport. */
static void
queue_visible_renders (EpimoneOverview *self)
{
  GtkWidget *child;

  for (int s = 0; s < 1; s++)
    for (child = gtk_widget_get_first_child (overview_flow_at (self, s));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
      {
        GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
        CardInfo *ci;

        if (card == NULL)
          continue;
        ci = g_object_get_data (G_OBJECT (card), "epi-card");
        if (ci == NULL || ci->rendered || ci->leaves == NULL || ci->leaves->len == 0)
          continue;
        if (g_queue_find (self->pending, ci) != NULL)
          continue;
        if (!card_is_near_viewport (self, child))
          continue;

        /* Skip the re-render when this card already has a cached texture whose
         * signature matches the current content. This is the fix for cards
         * "settling" a beat after the zoom: a still-cached card whose panes have
         * not changed is left exactly as it is instead of being re-rendered into
         * a texture that lands (and visibly swaps) after the animation. Only when
         * BOTH a texture and a stored signature exist; a placeholder card has
         * neither and must always render. The peeks card_current_sig makes are
         * the same round trips a render would do; the win is skipping the heavy
         * feed/layout/snapshot behind them. */
        {
          guint64 *stored = g_hash_table_lookup (
            self->thumb_sig, GSIZE_TO_POINTER ((gsize) ci->cache_key));
          guint64 now = 0;

          if (stored != NULL &&
              g_hash_table_contains (self->thumb_cache,
                                     GSIZE_TO_POINTER ((gsize) ci->cache_key)) &&
              card_current_sig (ci, &now) && now == *stored)
            {
              ci->rendered = TRUE;
              g_debug ("thumb: group %" G_GUINT64_FORMAT " unchanged "
                       "(sig %016" G_GINT64_MODIFIER "x); skipping re-render",
                       ci->group_id, now);
              continue;
            }
        }

        g_queue_push_tail (self->pending, ci);
      }

  g_debug ("thumb: %u card(s) queued for render (rendering was %d)",
           g_queue_get_length (self->pending), self->rendering);
  render_report (self, "after queueing");
  if (!self->rendering)
    render_pump (self);
}

static void
scrolled_cb (GtkAdjustment *adj, gpointer user_data)
{
  (void) adj;
  queue_visible_renders (user_data);
}

/* ------------------------------------------------------------------ *
 * card construction
 * ------------------------------------------------------------------ */

/* Layout for the thumbnail holder: contribute nothing to the parent's size
 * request, and give the child every pixel of whatever the card box turns out to
 * be. See the call site in build_card for why both halves matter. */
static void
thumb_holder_measure (GtkWidget      *widget,
                      GtkOrientation  orientation,
                      int             for_size,
                      int            *minimum,
                      int            *natural,
                      int            *minimum_baseline,
                      int            *natural_baseline)
{
  (void) widget; (void) orientation; (void) for_size;

  *minimum = 0;
  *natural = 0;
  if (minimum_baseline != NULL)
    *minimum_baseline = -1;
  if (natural_baseline != NULL)
    *natural_baseline = -1;
}

static void
thumb_holder_allocate (GtkWidget *widget, int width, int height, int baseline)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);

  if (child != NULL)
    gtk_widget_allocate (child, width, height, baseline, NULL);
}

/* The caption labels' holder: report NO width of its own (the thumbnail's size
 * request is what decides the card's width), but the child's real height. The
 * full shell title goes through here unshortened: an ellipsized GtkLabel's
 * NATURAL width is still the whole string, and the homogeneous flow box sizes
 * every cell from natural widths, so a long title would inflate the whole grid
 * (same failure mode as the oversized paintable; see thumb_holder_measure).
 * Zero width + full-width allocate (thumb_holder_allocate, shared) pins the
 * caption to the card and lets the label's own ellipsis do the cutting. */
static void
caption_holder_measure (GtkWidget      *widget,
                        GtkOrientation  orientation,
                        int             for_size,
                        int            *minimum,
                        int            *natural,
                        int            *minimum_baseline,
                        int            *natural_baseline)
{
  GtkWidget *child = gtk_widget_get_first_child (widget);

  (void) for_size;

  if (orientation == GTK_ORIENTATION_HORIZONTAL || child == NULL)
    {
      *minimum = 0;
      *natural = 0;
    }
  else
    gtk_widget_measure (child, orientation, -1, minimum, natural, NULL, NULL);
  if (minimum_baseline != NULL)
    *minimum_baseline = -1;
  if (natural_baseline != NULL)
    *natural_baseline = -1;
}

/* One caption line, pinned to the card's width (see caption_holder_measure). */
static GtkWidget *
caption_holder_new (GtkWidget *label)
{
  GtkWidget *holder = adw_bin_new ();

  adw_bin_set_child (ADW_BIN (holder), label);
  gtk_widget_set_layout_manager (holder,
                                 gtk_custom_layout_new (NULL,
                                                        caption_holder_measure,
                                                        thumb_holder_allocate));
  gtk_widget_set_can_focus (holder, FALSE);
  gtk_widget_set_can_target (holder, FALSE);
  return holder;
}

/* The space a thumbnail will occupy while it renders: a framed placeholder
 * holding a dim glyph. It fills the card's art stack, whose size request is the
 * computed card geometry, so the grid metrics do not change when the real
 * thumbnail arrives. */
static GtkWidget *
build_placeholder_art (void)
{
  GtkWidget *frame = gtk_frame_new (NULL);
  GtkWidget *icon = gtk_image_new_from_icon_name ("utilities-terminal-symbolic");

  gtk_widget_add_css_class (frame, "card");
  gtk_image_set_pixel_size (GTK_IMAGE (icon), 48);
  gtk_widget_add_css_class (icon, "dim-label");
  gtk_widget_set_halign (icon, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (icon, GTK_ALIGN_CENTER);
  gtk_frame_set_child (GTK_FRAME (frame), icon);
  return frame;
}

static GtkWidget *
build_card (EpimoneOverview *self,
            EpiGroupInfo    *info,
            const char      *title,         /* display: custom name when set */
            const char      *shell_title,   /* always the shell-provided one */
            const char      *custom_title,  /* user name, NULL when automatic */
            gboolean         attached,
            const char      *processes,     /* every pane's foreground command */
            const char      *active,        /* the non-shell ones, NULL if none */
            guint            pane_count,
            guint64          focused_session,
            guint            cols,
            guint            rows,
            GHashTable      *alive_by_sid,   /* set of alive session ids */
            guint64          orphan_session) /* 0 = group card; else this session */
{
  /* A plain box, not a GtkButton; see card_flow_activated_cb for why nesting
   * the kill control inside a button breaks it. The flow box child provides
   * activation, hover and focus.
   *
   * Box spacing 0 so the title and meta caption lines stay grouped. The
   * thumbnail-to-title gap is instead a 6px top margin on the title holder
   * alone (see where title_holder is built below). */
  GtkWidget *box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  GtkWidget *overlay = gtk_overlay_new ();
  GtkWidget *kill_button;
  GtkWidget *title_label;
  GtkWidget *meta_label;
  CardInfo *ci;
  GtkWidget *card_picture = NULL, *card_stack = NULL;
  g_autofree char *age = format_age (info->created_at);
  g_autofree char *meta = NULL;
  g_autofree char *hay = NULL;

  /* --- the art (placeholder until a thumbnail lands), kill control overlaid --- */
  {
    GtkWidget *stack = gtk_stack_new ();
    GtkWidget *ph = build_placeholder_art ();
    GtkWidget *pic = gtk_picture_new ();

    /* Reveal a freshly rendered thumbnail with a short crossfade instead of an
     * instant pop. On first open every card starts on the placeholder and each
     * texture lands a beat later (a real PEEK+feed+compose per card, serialized);
     * render_compose's gtk_stack_set_visible_child_name(stack, "picture") then
     * crossfades rather than cutting, so the grid fills in as a soft progressive
     * reveal rather than top-to-bottom pops. Purely presentation; the texture is
     * produced exactly as before. Two behaviours fall out of GtkStack's own rules
     * and are relied on here: a cache hit sets "picture" at BUILD time, before the
     * stack is mapped, and GtkStack does not animate an unmapped transition, so
     * reopening on already-rendered cards is instant with no fade; and a later
     * content refresh keeps the visible child at "picture", so the set is a no-op
     * and nothing re-fades. Only the genuine placeholder->picture first reveal,
     * which happens once the overview is on screen, animates. */
    gtk_stack_set_transition_type (GTK_STACK (stack),
                                   GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration (GTK_STACK (stack), 180);

    /* FILL (AdwTabThumbnail's picture is content-fit:fill too).
     * render_compose builds the texture at the card's pixel size at render
     * time, and card height derives from self->thumb_aspect, the texture's
     * baked aspect (overview_update_geometry), so on resize the card box
     * keeps that exact aspect. FILL therefore maps the texture 1:1 with no
     * distortion and no letterbox: the card scales as a rigid unit and the
     * content never shifts. FILL is only correct because the box cannot
     * drift to a different aspect than the texture; do not swap it for
     * CONTAIN without also changing how the card height is derived. */
    gtk_picture_set_content_fit (GTK_PICTURE (pic), GTK_CONTENT_FIT_FILL);
    gtk_picture_set_can_shrink (GTK_PICTURE (pic), TRUE);

    /* The picture goes in a holder that reports NO size of its own and hands its
     * child the whole allocation.
     *
     * Both halves are needed. GtkPicture reports its paintable's intrinsic size
     * as its NATURAL size and can-shrink only lowers the minimum, so a texture
     * larger than the card would inflate the card through the homogeneous flow
     * box (measured: a 722 px paintable produced one full-width card per row).
     * A GtkScrolledWindow wrapper does block that propagation, but a scrolled
     * window exists precisely to give its child the child's own natural size
     * and let it overflow, so the thumbnail lays out at texture size in the
     * top-left corner of a larger box instead of filling it. A measure that
     * returns zero fixes the first problem, and an allocate that passes the
     * full width and height fixes the second. AdwTabGrid solves it the same
     * way: it imposes the tab size on the thumbnail rather than asking it
     * (adw-tab-grid.c allocate_tab). */
    {
      GtkWidget *holder = adw_bin_new ();

      adw_bin_set_child (ADW_BIN (holder), pic);
      gtk_widget_set_layout_manager (holder,
                                     gtk_custom_layout_new (NULL,
                                                            thumb_holder_measure,
                                                            thumb_holder_allocate));
      gtk_widget_set_overflow (holder, GTK_OVERFLOW_HIDDEN);
      gtk_widget_set_can_focus (holder, FALSE);
      gtk_widget_set_can_target (holder, FALSE);
      gtk_widget_add_css_class (holder, "card");
      gtk_widget_add_css_class (holder, "epi-thumb-backing");
      gtk_stack_add_named (GTK_STACK (stack), holder, "picture");
    }
    gtk_stack_add_named (GTK_STACK (stack), ph, "placeholder");
    gtk_stack_set_visible_child_name (GTK_STACK (stack), "placeholder");
    /* The hook for the selected-tab outline (epimone-chrome.c,
     * `.epi-card-current .epi-thumb`): the stack IS the thumbnail's bounds
     * (placeholder or picture alike), so the outline hugs the thumbnail edge
     * and never wraps the caption. */
    gtk_widget_add_css_class (stack, "epi-thumb");
    /* The one size request on the card's art: the current computed geometry.
     * overview_apply_card_sizes updates it in place when the window resizes. */
    gtk_widget_set_size_request (stack, self->thumb_w, self->thumb_h);
    gtk_overlay_set_child (GTK_OVERLAY (overlay), stack);

    card_picture = pic;
    card_stack = stack;
  }

  /* Always-visible close control: a 24 px translucent circle inset in the
   * thumbnail corner, with a hit target comfortably larger than the visible
   * circle (the circle is drawn on the button's image node by
   * epimone-chrome.c, .epi-thumb-close). Always visible; libadwaita only
   * fades its equivalent during the open/close transition, never on hover.
   *
   * Plain × glyph: it reads clearly at this size, where symbolic icons blur.
   *
   * Note: × means "detach" elsewhere in Epimone but this control KILLS the
   * session, so it is gated behind a confirmation dialog that names the
   * session, its uptime, what is running in it and how many panes die with
   * it; nothing happens until that is accepted. The tooltip has no ellipsis,
   * matching the context menu's Kill items. */
  kill_button = gtk_button_new_from_icon_name ("window-close-symbolic");
  gtk_widget_add_css_class (kill_button, "epi-thumb-close");
  gtk_widget_set_halign (kill_button, GTK_ALIGN_END);
  gtk_widget_set_valign (kill_button, GTK_ALIGN_START);
  /* Never a focus target, same as every other overview control. */
  gtk_widget_set_can_focus (kill_button, FALSE);
  gtk_widget_set_focus_on_click (kill_button, FALSE);
  gtk_widget_set_tooltip_text (kill_button, "Kill this session");
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), kill_button);

  /* Detached state as a badge ON the thumbnail, opposite the kill control, rather
   * than as a row of coloured text under it.
   *
   * Only DETACHED gets a badge. Attached is the unremarkable state (it is what
   * every visible tab is), so marking it would put a label on nearly every card
   * and drown the one case that matters. A detached card is the thing the user
   * closed and may want back, so it is the one that earns ink; the absence of a
   * badge means attached, and the card's tooltip says which either way. */
  if (!attached)
    {
      GtkWidget *badge = gtk_label_new ("Detached");

      gtk_widget_add_css_class (badge, "epi-thumb-badge");
      gtk_widget_set_halign (badge, GTK_ALIGN_START);
      gtk_widget_set_valign (badge, GTK_ALIGN_END);   /* bottom-left corner */
      gtk_widget_set_margin_start (badge, 6);
      gtk_widget_set_margin_bottom (badge, 6);
      gtk_widget_set_can_target (badge, FALSE);
      gtk_overlay_add_overlay (GTK_OVERLAY (overlay), badge);
    }

  gtk_box_append (GTK_BOX (box), overlay);

  /* --- the text: one title line, one dimmed line. Nothing else. The state
   * lives on the thumbnail as a badge, and the process list and age share the
   * single dimmed line, so a card is a picture with a caption. --- */
  /* The FULL title ("user@host: ~/some/project"), cut at the card's edge with
   * END elision. Deliberately no prefix-stripping or middle-elision: that
   * leaves cards reading as a bare "~"; the elided remainder is on the card's
   * tooltip instead. Width is capped by the holder, not max-width-chars, so
   * the cut lands exactly at the card edge at every card size. Short titles
   * still read centred (the label's default 0.5 xalign inside the full-width
   * allocation). */
  title_label = gtk_label_new (title);
  gtk_label_set_ellipsize (GTK_LABEL (title_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (title_label), TRUE);
  {
    /* Gap between the thumbnail and the title: 6px, the spacing libadwaita's
     * own overview card uses (adw-tab-thumbnail.ui, contents box
     * spacing="6"). The card box here stays spacing 0 so the title and meta
     * lines remain grouped, so the 6px lands as a top margin on the title
     * holder alone. */
    GtkWidget *title_holder = caption_holder_new (title_label);

    gtk_widget_set_margin_top (title_holder, 6);
    gtk_box_append (GTK_BOX (box), title_holder);
  }

  /* One dimmed line: pane count, what is actually RUNNING, age. Shells are
   * left out ("bash ×3" identifies nothing when every pane is a prompt),
   * while a non-shell foreground process is exactly the detail that says
   * which session this is. Idle: "3 panes · 12m"; busy: "3 panes · hashcat
   * · 12m". */
  {
    GString *m = g_string_new (NULL);

    g_string_append_printf (m, "%u pane%s", pane_count,
                            pane_count == 1 ? "" : "s");
    if (active != NULL && active[0] != '\0')
      {
        g_autofree char *procs = summarise_processes (active);

        g_string_append_printf (m, " \xc2\xb7 %s", procs);
      }
    g_string_append_printf (m, " \xc2\xb7 %s", age);
    meta = g_string_free (m, FALSE);
  }
  meta_label = gtk_label_new (meta);
  gtk_label_set_ellipsize (GTK_LABEL (meta_label), PANGO_ELLIPSIZE_END);
  gtk_label_set_single_line_mode (GTK_LABEL (meta_label), TRUE);
  gtk_widget_add_css_class (meta_label, "caption");
  gtk_widget_add_css_class (meta_label, "dim-label");
  gtk_box_append (GTK_BOX (box), caption_holder_new (meta_label));

  /* GtkFlowBox with homogeneous=TRUE stretches its children to fill each row, so a
   * card would grow to width/children-per-row regardless of what it asked for
   * (measured: 274 px in a 1200 px grid, ~590 px in a 900 px one with two per row).
   * CENTER/START pins it to its natural size (the computed card geometry), so
   * the sizing formula stays the only thing that decides it. */
  gtk_widget_add_css_class (box, "epi-card");
  gtk_widget_set_halign (box, GTK_ALIGN_CENTER);
  gtk_widget_set_valign (box, GTK_ALIGN_START);
  /* Hovering a card shows the FULL title: the only place the part the caption
   * elided can be read. No "switch to" / "bring back" hint in the tooltip;
   * attached-vs-detached is the badge's job. */
  gtk_widget_set_tooltip_text (box, title);

  ci = g_new0 (CardInfo, 1);
  ci->group_id = info->id;
  /* Orphan cards get a sentinel cache key (never a real group's) and kill only
   * their own session; group cards cache by group id and kill by group. */
  ci->cache_key = orphan_session != 0 ? (ORPHAN_CACHE_BIT | orphan_session)
                                      : info->id;
  ci->kill_session = orphan_session;
  ci->picture = card_picture;
  ci->stack = card_stack;
  ci->nodes = g_ptr_array_new ();
  /* Both titles: a renamed card stays findable by what its shell says too. */
  hay = g_strdup_printf ("%s %s %s", title, shell_title ?: "", processes);
  ci->haystack = g_utf8_strdown (hay, -1);

  /* Attached and detached both render offscreen from PEEK, and both draw from
   * the texture cache first.
   *
   * Why not live GtkWidgetPaintables for attached tabs: the tab view itself
   * stays mapped under this hosting, but AdwTabView keeps every NON-SELECTED
   * page's bin child-visible FALSE (i.e. unmapped), and GTK snapshots nothing
   * for an unmapped widget. The API that maps them for an overview
   * (adw_tab_view_open_overview) is private and not even exported from the
   * library. AdwTabOverview's cards are not live widgets anyway; they are
   * cached textures (AdwTabPaintable). So: the SELECTED page is captured live
   * into the cache when the overview opens or closes (it is the one mapped
   * page), and every other card starts from its cached texture with the PEEK
   * pipeline refreshing it invisibly. */

  /* The tab's whole arrangement, so the card composes every pane. A blob too old
   * to carry a tree (or one that will not parse) leaves an empty set, handled by
   * the reconciliation below. */
  if (!epimone_layout_blob_geometry (info->blob, info->blob_len,
                                     &ci->leaves, &ci->seams))
    {
      ci->leaves = g_ptr_array_new_with_free_func (g_free);
      ci->seams = g_ptr_array_new_with_free_func (g_free);
    }

  /* Reconcile the blob's leaves against the daemon's LIVE membership. The blob is
   * the GUI's last-saved tree and can name sessions that have since exited or
   * moved to another group: stale leaves whose PEEK yields nothing, which
   * produce empty placeholder cards. The daemon's member list is the truth
   * for WHICH sessions this card shows; the blob supplies only their layout. So:
   * drop any leaf that is not a live member of this group, and if nothing valid
   * survives, fall back to a single live member (preferring an alive one over the
   * possibly-stale focused pane). A live member that the blob simply omits is not
   * forced in here (it would have no layout and would overlap the real panes);
   * overview_reload's safety-net pass gives it its own card instead. */
  {
    GHashTable *group_alive = g_hash_table_new (g_direct_hash, g_direct_equal);

    for (guint m = 0; m < info->members->len; m++)
      {
        guint64 sid = g_array_index (info->members, guint64, m);

        if (alive_by_sid == NULL ||
            g_hash_table_contains (alive_by_sid, GUINT_TO_POINTER ((guint) sid)))
          g_hash_table_add (group_alive, GUINT_TO_POINTER ((guint) sid));
      }

    for (guint i = ci->leaves->len; i-- > 0; )
      {
        EpimoneLayoutLeaf *leaf = g_ptr_array_index (ci->leaves, i);

        if (!g_hash_table_contains (group_alive,
                                    GUINT_TO_POINTER ((guint) leaf->session)))
          g_ptr_array_remove_index (ci->leaves, i);   /* freed via g_free */
      }

    if (ci->leaves->len == 0)
      {
        guint64 sid = 0;

        if (focused_session != 0 &&
            g_hash_table_contains (group_alive,
                                   GUINT_TO_POINTER ((guint) focused_session)))
          sid = focused_session;
        else
          for (guint m = 0; m < info->members->len; m++)
            {
              guint64 cand = g_array_index (info->members, guint64, m);

              if (g_hash_table_contains (group_alive,
                                         GUINT_TO_POINTER ((guint) cand)))
                { sid = cand; break; }
            }

        if (sid != 0)
          {
            EpimoneLayoutLeaf *only = g_new0 (EpimoneLayoutLeaf, 1);

            only->session = sid;
            only->w = only->h = 1.0;
            only->cols = cols;
            only->rows = rows;
            g_ptr_array_add (ci->leaves, only);
          }
        else
          g_warning ("epimone: session group %" G_GUINT64_FORMAT " has no live "
                     "panes to render a thumbnail from; its card keeps the "
                     "placeholder", info->id);
      }

    g_hash_table_destroy (group_alive);
  }

  /* A version 1 blob records no grid at all, so nothing above could derive one.
   * The measured grid of the live focused pane is the best available answer, and
   * for a single-pane tab it is exactly right. */
  for (guint i = 0; i < ci->leaves->len; i++)
    {
      EpimoneLayoutLeaf *leaf = g_ptr_array_index (ci->leaves, i);

      if (leaf->cols == 0 || leaf->rows == 0)
        {
          leaf->cols = cols > 0 ? cols : 80;
          leaf->rows = rows > 0 ? rows : 24;
        }
    }

  /* The cache, before the card is ever seen: a group that has been rendered
   * before starts from that texture in the same frame the grid is built. A size
   * mismatch after a window resize is fine; the picture FILLs, and the queued
   * refresh replaces the texture without ever showing the placeholder.
   * ci->rendered normally stays FALSE so the refresh is queued. */
  {
    GdkTexture *cached = g_hash_table_lookup (self->thumb_cache,
                                              GSIZE_TO_POINTER ((gsize) ci->cache_key));

    if (cached != NULL)
      {
        gtk_picture_set_paintable (GTK_PICTURE (card_picture),
                                   GDK_PAINTABLE (cached));
        gtk_stack_set_visible_child_name (GTK_STACK (card_stack), "picture");

        /* The active card whose live texture was just captured for THIS open
         * (fresh_capture_group) keeps that texture: mark it rendered so no PEEK
         * refresh is queued to swap it ~80 ms after the zoom settles. Group
         * cards only (orphans key their cache by a sentinel, so `cached` above
         * is never their live capture). The flag is set only during the open
         * reload and cleared right after, so every other reload still refreshes
         * this card. */
        if (orphan_session == 0 && info->id != 0 &&
            info->id == self->fresh_capture_group)
          ci->rendered = TRUE;
      }
  }

  g_object_set_data_full (G_OBJECT (box), "epi-card", ci, card_info_free);
  /* Everything the kill confirmation and the context menu name lives on the
   * box: which session, what is running in it, how many panes, how old, both
   * titles and the state. The kill button reaches it through "epi-box". */
  g_object_set_data_full (G_OBJECT (box), "epi-title", g_strdup (title), g_free);
  g_object_set_data_full (G_OBJECT (box), "epi-shell", g_strdup (shell_title),
                          g_free);
  g_object_set_data_full (G_OBJECT (box), "epi-custom", g_strdup (custom_title),
                          g_free);
  g_object_set_data_full (G_OBJECT (box), "epi-procs", g_strdup (processes),
                          g_free);
  g_object_set_data_full (G_OBJECT (box), "epi-age", g_strdup (age), g_free);
  g_object_set_data (G_OBJECT (box), "epi-panes", GUINT_TO_POINTER (pane_count));
  g_object_set_data (G_OBJECT (box), "epi-attached",
                     GUINT_TO_POINTER (attached ? 1u : 0u));
  g_object_set_data (G_OBJECT (kill_button), "epi-box", box);

  g_signal_connect (kill_button, "clicked", G_CALLBACK (card_kill_cb), self);

  /* The context menu, on the SECONDARY button only and in the default bubble
   * phase, so it cannot touch either of the primary-button paths: the flow
   * box's child-activated (card body) or the × button's own capture-phase
   * click gesture.
   *
   * Skipped for orphan cards: their menu actions are group-scoped (Detach /
   * Rename / Kill act on the group id) and an orphan's group is its still-
   * attached parent tab, so those items would be wrong or destructive. An orphan
   * card keeps just the two safe affordances: the × kills its own session, the
   * body navigates to the parent tab. */
  if (orphan_session == 0)
    {
      GtkGesture *rclick = gtk_gesture_click_new ();

      gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (rclick),
                                     GDK_BUTTON_SECONDARY);
      g_signal_connect (rclick, "pressed", G_CALLBACK (card_right_click_cb), self);
      gtk_widget_add_controller (box, GTK_EVENT_CONTROLLER (rclick));
    }

  return box;
}

/* ------------------------------------------------------------------ *
 * populate
 * ------------------------------------------------------------------ */

static void
flow_clear (GtkWidget *flow)
{
  GtkWidget *child;

  while ((child = gtk_widget_get_first_child (flow)) != NULL)
    gtk_flow_box_remove (GTK_FLOW_BOX (flow), child);
}

/* The id of the group @sid belongs to, or 0 if none holds it. Used only to give
 * an orphan safety-net card a navigation target (its parent tab); never for the
 * card's cache key or kill, which stay session-scoped. */
static guint64
group_id_of_session (GPtrArray *groups, guint64 sid)
{
  for (guint i = 0; i < groups->len; i++)
    {
      EpiGroupInfo *info = g_ptr_array_index (groups, i);

      for (guint m = 0; m < info->members->len; m++)
        if (g_array_index (info->members, guint64, m) == sid)
          return info->id;
    }
  return 0;
}

/* A group is attached if any of its member sessions is attached to a tab. */
static gboolean
group_is_attached (EpiGroupInfo *info, GHashTable *attached_by_sid)
{
  for (guint m = 0; m < info->members->len; m++)
    if (g_hash_table_lookup (attached_by_sid,
          GUINT_TO_POINTER ((guint) g_array_index (info->members, guint64, m))))
      return TRUE;
  return FALSE;
}

static void
overview_reload (EpimoneOverview *self)
{
  gint64 t_reload = g_get_monotonic_time ();
  g_autoptr (GPtrArray) groups = NULL;
  g_autoptr (GPtrArray) sessions = NULL;
  GHashTable *attached_by_sid;   /* sid -> attached flag */
  GHashTable *pid_by_sid;        /* sid -> child pid */
  GHashTable *alive;             /* set of alive session ids (si->alive) */
  GHashTable *shown;             /* set of session ids some card renders */
  g_autoptr (GHashTable) mine =
    g_hash_table_new (g_direct_hash, g_direct_equal);   /* this window's groups */
  guint64 current_group =
    self->win != NULL ? epimone_window_get_active_group (self->win) : 0;
  GError *err = NULL;
  guint attached_cards = 0, detached_cards = 0;

  /* A genuine (re)render: re-capture the aspect the new textures will be baked
   * at from the current window, so card height tracks it until the next render.
   * (A plain resize does NOT come through here, so it cannot change the aspect.) */
  self->thumb_aspect = overview_render_aspect (self);

  /* Before flow_clear destroys the cards, and therefore their CardInfo and the
   * widgets the transition anchors name. */
  render_abort (self);
  overview_set_anchor (self, NULL);
  overview_set_pending_anchor (self, NULL);
  flow_clear (self->flow);

  groups = epimone_client_list_groups (NULL, &err);
  if (groups == NULL)
    {
      g_warning ("epimone: could not list sessions for the overview: %s",
                 err != NULL ? err->message : "unknown error");
      g_clear_error (&err);
      gtk_stack_set_visible_child_name (GTK_STACK (self->stack), "empty");
      return;
    }

  sessions = epimone_client_list_sessions (NULL);
  attached_by_sid = g_hash_table_new (g_direct_hash, g_direct_equal);
  pid_by_sid = g_hash_table_new (g_direct_hash, g_direct_equal);
  /* The daemon's alive set, the source of truth for WHICH sessions exist. Note
   * list_sessions can carry a dead session (si->alive == FALSE), so membership
   * here is gated on si->alive, not mere presence. */
  alive = g_hash_table_new (g_direct_hash, g_direct_equal);
  /* Every live session a card actually renders, so the safety-net pass can spot
   * any that no card shows. */
  shown = g_hash_table_new (g_direct_hash, g_direct_equal);
  if (sessions != NULL)
    {
      for (guint i = 0; i < sessions->len; i++)
        {
          EpiSessionInfo *si = g_ptr_array_index (sessions, i);

          g_hash_table_insert (attached_by_sid, GUINT_TO_POINTER ((guint) si->id),
                               GUINT_TO_POINTER (si->attached ? 1u : 0u));
          g_hash_table_insert (pid_by_sid, GUINT_TO_POINTER ((guint) si->id),
                               GUINT_TO_POINTER (si->pid));
          if (si->alive)
            g_hash_table_add (alive, GUINT_TO_POINTER ((guint) si->id));
        }
    }

  /* This window's attached tabs, derived live from its tab view (no cached
   * window-to-group map anywhere), so Move to New Window is automatically
   * right. */
  if (self->win != NULL)
    {
      g_autoptr (GArray) ids = epimone_window_dup_attached_group_ids (self->win);

      for (guint i = 0; i < ids->len; i++)
        g_hash_table_add (mine,
          GUINT_TO_POINTER ((guint) g_array_index (ids, guint64, i)));
    }

  /* What THIS window shows: its own attached tabs, plus every detached group. A
   * group attached in another window appears nowhere here. Count first so the
   * card geometry sizes for the real number of cards, not the daemon-wide total. */
  for (guint i = 0; i < groups->len; i++)
    {
      EpiGroupInfo *info = g_ptr_array_index (groups, i);

      if (g_hash_table_contains (mine, GUINT_TO_POINTER ((guint) info->id)))
        attached_cards++;
      else if (!group_is_attached (info, attached_by_sid))
        detached_cards++;
    }

  /* Geometry before any card is built: build_card reads self->thumb_w/h. At the
   * first-ever open the overview widget has no allocation yet, so fall back to
   * the window, whose content area the overview is about to fill. */
  self->n_cards = attached_cards + detached_cards;
  {
    int w = gtk_widget_get_width (GTK_WIDGET (self));
    int h = gtk_widget_get_height (GTK_WIDGET (self));

    if (w <= 0 && self->win != NULL)
      {
        w = gtk_widget_get_width (GTK_WIDGET (self->win));
        h = gtk_widget_get_height (GTK_WIDGET (self->win));
      }
    overview_update_geometry (self, w, h);
    gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (self->flow),
                                            (guint) MAX (self->columns, 1));
  }

  /* One grid, attached cards first then detached. Attached cards are appended to
   * `flow` as they are built; detached ones (and the orphan safety-net cards) are
   * held here and appended after the loop, so the single flow reads attached-then-
   * detached with no section break. */
  g_autoptr (GPtrArray) detached_pending = g_ptr_array_new ();

  for (guint i = 0; i < groups->len; i++)
    {
      EpiGroupInfo *info = g_ptr_array_index (groups, i);
      gboolean attached = group_is_attached (info, attached_by_sid);
      gboolean mine_group =
        g_hash_table_contains (mine, GUINT_TO_POINTER ((guint) info->id));
      g_autofree char *title = NULL;
      g_autofree char *custom = NULL;
      g_autoptr (GString) procs = g_string_new (NULL);
      g_autoptr (GString) active = g_string_new (NULL);
      GtkWidget *card;
      guint64 focused = 0;
      guint cols = 0, rows = 0;

      /* Attached in another window: its card lives in that window's overview.
       * Its sessions ARE represented (there), so record them as shown so the
       * safety-net pass below does not duplicate them into this window. */
      if (!mine_group && attached)
        {
          for (guint m = 0; m < info->members->len; m++)
            g_hash_table_add (shown,
              GUINT_TO_POINTER ((guint) g_array_index (info->members, guint64, m)));
          continue;
        }

      /* The blob knows the tab title, the focused pane and, from version 2,
       * that pane's grid: the seed the whole tab's grid is derived from. */
      epimone_layout_blob_peek (info->blob, info->blob_len, &title, &focused,
                                &cols, &rows);
      custom = epimone_layout_blob_dup_custom_title (info->blob, info->blob_len);

      for (guint m = 0; m < info->members->len; m++)
        {
          guint64 sid = g_array_index (info->members, guint64, m);
          gpointer pidp = g_hash_table_lookup (pid_by_sid,
                                               GUINT_TO_POINTER ((guint) sid));
          g_autofree char *cmd = NULL;

          if (pidp != NULL)
            cmd = epimone_client_foreground_command (GPOINTER_TO_UINT (pidp));
          if (procs->len > 0)
            g_string_append (procs, ", ");
          g_string_append (procs, cmd != NULL ? cmd : "?");

          /* The caption's process list: only what is NOT a plain shell. */
          if (cmd != NULL && !command_is_shell (cmd))
            {
              if (active->len > 0)
                g_string_append (active, ", ");
              g_string_append (active, cmd);
            }
        }

      /* An attached group has live widgets: measure the focused pane's real
       * grid rather than trusting the blob's last-synced one. */
      if (attached && self->win != NULL && focused != 0)
        {
          EpimonePage *page =
            epimone_window_find_page_for_session (self->win, focused);

          if (page != NULL)
            {
              epimone_page_get_focused_grid (page, &cols, &rows);
              /* The page's LIVE custom name, not the blob's: a rename is
               * visible here before the debounced blob sync has written it. */
              g_free (custom);
              custom = g_strdup (epimone_page_get_custom_title (page));
            }
        }

      card = build_card (self, info,
                         custom != NULL ? custom
                                        : (title != NULL ? title : "Terminal"),
                         title != NULL ? title : "Terminal",
                         custom,
                         attached,
                         procs->len > 0 ? procs->str : "no panes",
                         active->len > 0 ? active->str : NULL,
                         info->members->len,
                         focused, cols, rows,
                         alive, 0 /* not an orphan card */);
      /* The tab entered from: its THUMBNAIL carries a faint palette-derived
       * outline (epimone-chrome.c, `.epi-card-current .epi-thumb`), not an
       * accent ring around the whole card. */
      if (info->id == current_group)
        gtk_widget_add_css_class (card, "epi-card-current");
      /* Attached cards go straight into the grid; detached ones wait so they all
       * land after the attached block (single continuous grid, attached first). */
      if (mine_group)
        gtk_flow_box_append (GTK_FLOW_BOX (self->flow), card);
      else
        g_ptr_array_add (detached_pending, card);

      /* Record which live sessions this card actually renders (post-
       * reconciliation), so the safety-net pass can find any that no card
       * shows. */
      {
        CardInfo *built = g_object_get_data (G_OBJECT (card), "epi-card");

        if (built != NULL && built->leaves != NULL)
          for (guint l = 0; l < built->leaves->len; l++)
            g_hash_table_add (shown, GUINT_TO_POINTER ((guint)
              ((EpimoneLayoutLeaf *) g_ptr_array_index (built->leaves, l))->session));
      }
    }

  /* Safety net: every session the daemon reports ALIVE must appear on exactly
   * one card. A pane detached out of a still-attached tab stays filed under that
   * (attached) group, so it is neither in the group's card (which shows the
   * attached panes) nor in any detached-group card; without this it would appear
   * nowhere. Give each still-unrepresented alive session its own single-pane
   * "Detached" card. Deduped by `shown` (populated above, including sessions
   * shown in other windows' overviews), so nothing is ever doubled. These are
   * orphan cards: the × kills only that session, the body navigates to the parent
   * tab; no group is created or reassigned. This is display only. */
  if (sessions != NULL)
    for (guint i = 0; i < sessions->len; i++)
      {
        EpiSessionInfo *si = g_ptr_array_index (sessions, i);
        EpiGroupInfo orphan;
        GArray *one;
        g_autofree char *cmd = NULL;
        GtkWidget *card;

        if (!si->alive ||
            g_hash_table_contains (shown, GUINT_TO_POINTER ((guint) si->id)))
          continue;

        cmd = epimone_client_foreground_command (si->pid);

        one = g_array_new (FALSE, FALSE, sizeof (guint64));
        g_array_append_val (one, si->id);

        orphan.id = group_id_of_session (groups, si->id);   /* navigation only */
        orphan.created_at = si->created_at;
        orphan.blob = NULL;
        orphan.blob_len = 0;
        orphan.members = one;

        card = build_card (self, &orphan,
                           cmd != NULL ? cmd : "Terminal",
                           cmd != NULL ? cmd : "Terminal",
                           NULL,
                           FALSE,   /* detached: earns the "Detached" badge */
                           cmd != NULL ? cmd : "no panes",
                           (cmd != NULL && !command_is_shell (cmd)) ? cmd : NULL,
                           1,
                           si->id, 0, 0,
                           alive, si->id /* orphan card for this session */);
        g_ptr_array_add (detached_pending, card);
        detached_cards++;
        g_hash_table_add (shown, GUINT_TO_POINTER ((guint) si->id));
        g_array_unref (one);
      }

  /* Flush the detached cards into the one grid, after the attached block. */
  for (guint i = 0; i < detached_pending->len; i++)
    gtk_flow_box_append (GTK_FLOW_BOX (self->flow),
                         g_ptr_array_index (detached_pending, i));

  g_hash_table_destroy (attached_by_sid);
  g_hash_table_destroy (pid_by_sid);
  g_hash_table_destroy (alive);
  g_hash_table_destroy (shown);

  /* Drop cached textures for groups that no longer exist. Orphan cards' sentinel
   * keys (top bit set) match no group id, so they are pruned here too; orphans
   * simply re-render on the next open, which is fine. */
  {
    GHashTableIter it;
    gpointer key;

    g_hash_table_iter_init (&it, self->thumb_cache);
    while (g_hash_table_iter_next (&it, &key, NULL))
      {
        guint64 gid = (guint64) GPOINTER_TO_SIZE (key);
        gboolean found = FALSE;

        for (guint i = 0; i < groups->len && !found; i++)
          found = ((EpiGroupInfo *) g_ptr_array_index (groups, i))->id == gid;
        if (!found)
          {
            /* Its signature goes with it: keyed identically, kept in lockstep. */
            g_hash_table_remove (self->thumb_sig, key);
            g_hash_table_iter_remove (&it);
          }
      }
  }

  gtk_stack_set_visible_child_name (GTK_STACK (self->stack),
                                    (attached_cards + detached_cards) > 0
                                      ? "cards" : "empty");

  /* Header: this window's attached tab count, then the daemon-wide detached
   * count. With no detached groups the tail is dropped, leaving just the
   * sessions figure (as AdwTabOverview shows "%u Tab(s)"). */
  if (self->count_title != NULL)
    {
      guint n = attached_cards, m = detached_cards;
      g_autofree char *count = NULL;

      if (n + m == 0)
        count = g_strdup ("No Sessions");
      else if (m == 0)
        count = g_strdup_printf (n == 1 ? "%u Session" : "%u Sessions", n);
      else
        count = g_strdup_printf (n == 1 ? "%u Session · %u Detached"
                                        : "%u Sessions · %u Detached", n, m);

      adw_window_title_set_title (self->count_title, count);
    }

  g_debug ("thumb: reload built %u card(s) in %.1f ms",
           attached_cards + detached_cards,
           (g_get_monotonic_time () - t_reload) / 1000.0);

  /* Detached cards refresh when the overview opens. Only the ones near the
   * viewport are queued; scrolling queues the rest. */
  queue_visible_renders (self);
}

/* ------------------------------------------------------------------ *
 * search
 * ------------------------------------------------------------------ */

static gboolean
filter_card (GtkFlowBoxChild *child, gpointer user_data)
{
  EpimoneOverview *self = user_data;
  const char *needle = gtk_editable_get_text (GTK_EDITABLE (self->search_entry));
  GtkWidget *card;
  CardInfo *ci;
  g_autofree char *folded = NULL;

  if (needle == NULL || needle[0] == '\0')
    return TRUE;

  card = gtk_flow_box_child_get_child (child);
  if (card == NULL)
    return TRUE;
  ci = g_object_get_data (G_OBJECT (card), "epi-card");
  if (ci == NULL || ci->haystack == NULL)
    return TRUE;

  folded = g_utf8_strdown (needle, -1);
  return strstr (ci->haystack, folded) != NULL;
}

static void
search_changed_cb (GtkEditable *entry, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  (void) entry;
  gtk_flow_box_invalidate_filter (GTK_FLOW_BOX (self->flow));
}

/* ------------------------------------------------------------------ *
 * construction
 * ------------------------------------------------------------------ */

/* Does @widget (or any descendant) show exactly @label? */
static gboolean
overview_menu_shows_label (GtkWidget *widget, const char *label)
{
  if (GTK_IS_LABEL (widget) &&
      g_strcmp0 (gtk_label_get_text (GTK_LABEL (widget)), label) == 0)
    return TRUE;

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    if (overview_menu_shows_label (child, label))
      return TRUE;
  return FALSE;
}

/* Give the menu item titled @label the .epi-destructive class, whose
 * palette-adaptive red lives in epimone-chrome.c. GMenu carries no attribute
 * for destructive styling, so the item's model button is found by its label,
 * the same walk-the-internals precedent as the accelerator-label reordering.
 * Run from the popover's "map", because GtkPopoverMenu may build its items
 * lazily; adding an already-present class is a no-op. */
static void
overview_menu_tag_destructive (GtkWidget *widget, const char *label)
{
  if (g_strcmp0 (gtk_widget_get_css_name (widget), "modelbutton") == 0 &&
      overview_menu_shows_label (widget, label))
    {
      gtk_widget_add_css_class (widget, "epi-destructive");
      return;
    }

  for (GtkWidget *child = gtk_widget_get_first_child (widget);
       child != NULL;
       child = gtk_widget_get_next_sibling (child))
    overview_menu_tag_destructive (child, label);
}

static void
overview_menu_map_cb (GtkWidget *popover, gpointer user_data)
{
  EpimoneOverview *self = user_data;

  /* Counts settled at open time: disable whatever has nothing to kill. */
  overview_menu_update_state (self);
  overview_menu_tag_destructive (popover, "Kill detached sessions");
  overview_menu_tag_destructive (popover, "Kill sessions in this window");
  overview_menu_tag_destructive (popover, "Kill all sessions");
}

/* Configure the one card grid: the search filter, activation and the "card."
 * action group, so every card behaves the same wherever it sits in the grid. */
static void
overview_setup_flow (EpimoneOverview *self, GtkWidget *flow)
{
  gtk_flow_box_set_homogeneous (GTK_FLOW_BOX (flow), TRUE);
  gtk_flow_box_set_selection_mode (GTK_FLOW_BOX (flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_min_children_per_line (GTK_FLOW_BOX (flow), 1);
  /* max-children-per-line follows the computed column count
   * (overview_update_geometry): the formula sizes cards for exactly n columns,
   * and without the cap a wide window would pack an extra card per row. */
  gtk_flow_box_set_max_children_per_line (GTK_FLOW_BOX (flow),
                                          OVERVIEW_MAX_COLUMNS);
  gtk_flow_box_set_row_spacing (GTK_FLOW_BOX (flow), OVERVIEW_SPACING);
  gtk_flow_box_set_column_spacing (GTK_FLOW_BOX (flow), OVERVIEW_SPACING);
  gtk_widget_set_valign (flow, GTK_ALIGN_START);
  /* CENTER, so the grid block sits centred with dead space split evenly, the
   * same effect as AdwTabGrid's centred row offset. */
  gtk_widget_set_halign (flow, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_start (flow, OVERVIEW_SPACING);
  gtk_widget_set_margin_end (flow, OVERVIEW_SPACING);
  gtk_flow_box_set_filter_func (GTK_FLOW_BOX (flow), filter_card, self, NULL);
  /* Cards are activated through the flow box, not by being buttons themselves
   * (see card_flow_activated_cb). This covers both a click and Enter on a
   * focused card. */
  g_signal_connect (flow, "child-activated",
                    G_CALLBACK (card_flow_activated_cb), self);
  /* Each card's popover resolves "card." by walking up to its flow box, so the
   * shared action group is inserted on it. */
  if (self->card_actions != NULL)
    gtk_widget_insert_action_group (flow, "card",
                                    G_ACTION_GROUP (self->card_actions));
}

static void
epimone_overview_init (EpimoneOverview *self)
{
  GtkWidget *overlay;
  GtkWidget *empty;
  GtkWidget *content;
  GtkWidget *scroller;
  GtkWidget *emptypage;
  GtkWidget *bottom;
  GtkWidget *new_tab;
  GtkEventController *keys;

  /* Sensible defaults until the first real allocation feeds the formula. The
   * aspect (146/260) matches the default thumb size and is overwritten by the
   * first genuine render (overview_reload / overview_appearance_changed_cb). */
  self->thumb_w = 260;
  self->thumb_h = 146;
  self->thumb_aspect = 146.0 / 260.0;
  self->columns = OVERVIEW_MAX_COLUMNS;

  /* ---- the overview-mode header bar ----
   *
   * Shown by the window IN PLACE of its normal header while the overview is
   * up, carrying only a search toggle and a live session count. No new-tab
   * button, no hamburger. The window sinks the floating ref when it adopts
   * the bar as a top bar. */
  self->header = adw_header_bar_new ();

  self->search_button = gtk_toggle_button_new ();
  gtk_button_set_icon_name (GTK_BUTTON (self->search_button), "edit-find-symbolic");
  gtk_widget_set_tooltip_text (self->search_button, "Search Sessions");
  /* Never the fallback focus target, for the same reason the window's "+" is not. */
  gtk_widget_set_focus_on_click (self->search_button, FALSE);
  gtk_widget_set_can_focus (self->search_button, FALSE);
  adw_header_bar_pack_start (ADW_HEADER_BAR (self->header), self->search_button);

  /* Nothing else on the left, and deliberately no grid toggle: the app's own
   * header (which carries the grid toggle) is inside the overview's child,
   * and the child is not drawn while the overview is open, so the toggle is
   * covered on entry and back on exit with no code.
   *
   * That leaves the pointer exits: clicking any card (including the one
   * arrived from) and the New Tab button, both of which select a tab and
   * close the overview. Escape and the shortcut remain. */

  self->count_title = ADW_WINDOW_TITLE (adw_window_title_new ("Sessions", NULL));
  /* Tabular figures, so the count does not wobble as it changes; the same
   * class AdwTabOverview puts on its title. */
  gtk_widget_add_css_class (GTK_WIDGET (self->count_title), "numeric");
  adw_header_bar_set_title_widget (ADW_HEADER_BAR (self->header),
                                   GTK_WIDGET (self->count_title));

  /* ---- the session-actions menu, right of the title before the window
   * controls. On the RIGHT deliberately: it keeps the destructive actions
   * away from the search toggle on the left, and it matches the main window,
   * whose hamburger also sits top-right. Items run least to most
   * destructive, with Kill all sessions separated and styled destructive so
   * it takes a deliberate reach. "Kill sessions older than" is present but
   * DISABLED (grey, non-activatable) until it is implemented, so the menu's
   * final shape is settled now and wiring it later changes nothing around
   * it. ---- */
  {
    GSimpleActionGroup *group = g_simple_action_group_new ();
    GSimpleAction *action;
    GMenu *model = g_menu_new ();
    GMenu *section = g_menu_new ();
    GtkWidget *menu_button = gtk_menu_button_new ();
    GtkPopover *popover;

    action = g_simple_action_new ("kill-detached", NULL);
    g_signal_connect (action, "activate",
                      G_CALLBACK (overview_kill_detached_cb), self);
    g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
    g_object_unref (action);

    action = g_simple_action_new ("kill-in-this-window", NULL);
    g_signal_connect (action, "activate",
                      G_CALLBACK (overview_kill_window_cb), self);
    g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
    g_object_unref (action);

    action = g_simple_action_new ("kill-older", NULL);
    g_simple_action_set_enabled (action, FALSE);   /* still a stub */
    g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
    g_object_unref (action);

    action = g_simple_action_new ("kill-all", NULL);
    g_signal_connect (action, "activate", G_CALLBACK (overview_kill_all_cb),
                      self);
    g_action_map_add_action (G_ACTION_MAP (group), G_ACTION (action));
    g_object_unref (action);

    /* Kept, so the scoped kills' enabled flags can be recomputed from live
     * counts each time the menu opens (overview_menu_update_state). Dropped in
     * dispose. */
    self->overview_actions = group;
    gtk_widget_insert_action_group (GTK_WIDGET (self), "overview",
                                    G_ACTION_GROUP (group));

    /* No ellipsis on "Kill sessions older than" even though it will open a
     * threshold dialog one day: the convention is no ellipsis on menu items,
     * anywhere. */
    g_menu_append (section, "Kill detached sessions", "overview.kill-detached");
    g_menu_append (section, "Kill sessions in this window",
                   "overview.kill-in-this-window");
    g_menu_append (section, "Kill sessions older than", "overview.kill-older");
    g_menu_append_section (model, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    section = g_menu_new ();
    g_menu_append (section, "Kill all sessions", "overview.kill-all");
    g_menu_append_section (model, NULL, G_MENU_MODEL (section));
    g_object_unref (section);

    gtk_menu_button_set_icon_name (GTK_MENU_BUTTON (menu_button),
                                   "view-more-symbolic");
    gtk_widget_set_tooltip_text (menu_button, "Session actions");
    gtk_menu_button_set_menu_model (GTK_MENU_BUTTON (menu_button),
                                    G_MENU_MODEL (model));
    g_object_unref (model);

    /* Never a focus target, same treatment as the search toggle here and the
     * "+"/hamburger in the main header. The focusable part of a
     * GtkMenuButton is the GtkToggleButton it wraps, so that is what gets
     * can-focus FALSE (see the hamburger's note in epimone-window.c). */
    gtk_widget_set_focus_on_click (menu_button, FALSE);
    gtk_widget_set_can_focus (menu_button, FALSE);
    {
      GtkWidget *toggle = gtk_widget_get_first_child (menu_button);

      if (GTK_IS_TOGGLE_BUTTON (toggle))
        gtk_widget_set_can_focus (toggle, FALSE);
    }

    adw_header_bar_pack_end (ADW_HEADER_BAR (self->header), menu_button);

    popover = gtk_menu_button_get_popover (GTK_MENU_BUTTON (menu_button));
    if (popover != NULL)
      {
        /* GTK drops focus to NULL when a popover closes and then falls back
         * to the first focusable widget: the spurious-tab mechanism. Land
         * focus where the card context menu's closed handler already does:
         * the inert overview_ui, so Enter right after dismissing activates
         * nothing and Escape keeps reaching the overview's controller.
         * epimone_window_hide_overview stays the only terminal-focus exit. */
        g_signal_connect (popover, "closed",
                          G_CALLBACK (card_menu_closed_cb), self);
        /* On map, not at construction: GtkPopoverMenu may build and
         * re-render its items lazily (see the accel-label fix in
         * epimone-shortcuts.c, which discovered this), and the destructive
         * class must survive that. Tagging is idempotent. */
        g_signal_connect (popover, "map",
                          G_CALLBACK (overview_menu_map_cb), self);
      }
  }

  /* ---- the visible UI ---- */
  content = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  /* Opaque (it hides the offscreen terminal below) and painted the TITLEBAR
   * tone by epimone-chrome.c, not the body tone: with the header showing the
   * same colour, the overview reads as one continuous surface. The overview
   * is the one deliberate exception to the header-steps-off-the-body rule. */
  gtk_widget_add_css_class (content, "epi-overview");
  gtk_widget_set_hexpand (content, TRUE);
  gtk_widget_set_vexpand (content, TRUE);

  /* Structured exactly as AdwTabOverview's search row (adw-tab-overview.ui):
   * a GtkSearchBar whose child is an AdwClamp (maximum-size 400) around the
   * GtkSearchEntry, so the field reads as a contained pill inset from the
   * window edges instead of an edge-to-edge strip.
   *
   * The "inline" style class is load-bearing: a bare GtkSearchBar paints its
   * inner box headerbar_bg with an inset bottom hairline
   * (`searchbar > revealer > box` in libadwaita), and on backdrop swaps that
   * fill to --headerbar-backdrop-color, which epimone-chrome.c points at the
   * TERMINAL background, not the overview's titlebar tone, so an inactive
   * window would show the bar as a full-width mismatched band with a dark
   * line under it. libadwaita's own `searchbar.inline` rule zeroes the
   * background and box-shadow in both states. */
  self->search_bar = gtk_search_bar_new ();
  gtk_widget_add_css_class (self->search_bar, "inline");
  self->search_entry = gtk_search_entry_new ();
  gtk_widget_set_hexpand (self->search_entry, TRUE);
  g_object_set (self->search_entry, "placeholder-text", "Search tabs", NULL);
  {
    GtkWidget *clamp = adw_clamp_new ();

    gtk_widget_set_hexpand (clamp, TRUE);
    adw_clamp_set_maximum_size (ADW_CLAMP (clamp), 400);
    adw_clamp_set_child (ADW_CLAMP (clamp), self->search_entry);
    gtk_search_bar_set_child (GTK_SEARCH_BAR (self->search_bar), clamp);
  }
  gtk_search_bar_connect_entry (GTK_SEARCH_BAR (self->search_bar),
                                GTK_EDITABLE (self->search_entry));
  g_object_bind_property (self->search_button, "active",
                          self->search_bar, "search-mode-enabled",
                          G_BINDING_BIDIRECTIONAL);
  g_signal_connect (self->search_entry, "changed",
                    G_CALLBACK (search_changed_cb), self);
  gtk_box_append (GTK_BOX (content), self->search_bar);

  self->stack = gtk_stack_new ();
  gtk_widget_set_vexpand (self->stack, TRUE);

  /* The card context menu's actions, once for every card: each card's popover
   * resolves "card." by walking up to its flow box. Which card they act on is
   * the self->menu_* snapshot taken when the menu opened. Built before the flow
   * box so overview_setup_flow can insert it. */
  {
    static const GActionEntry card_entries[] = {
      { .name = "activate", .activate = card_action_activate_cb },
      { .name = "rename",   .activate = card_action_rename_cb },
      { .name = "kill",     .activate = card_action_kill_cb },
      /* State-dependent: enabled (and so shown) per card in
       * overview_menu_set_state. */
      { .name = "attach-new-window", .activate = card_action_attach_new_window_cb },
      { .name = "move-new-window",   .activate = card_action_move_new_window_cb },
      { .name = "close-tab",         .activate = card_action_close_tab_cb },
    };

    self->card_actions = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (self->card_actions),
                                     card_entries,
                                     G_N_ELEMENTS (card_entries), self);
  }

  /* One continuous grid in the scroller (attached cards, then detached).
   * grid_box is the scrollable content and the reference every scroll-position
   * computation measures against. */
  self->flow = gtk_flow_box_new ();
  overview_setup_flow (self, self->flow);

  self->grid_box = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_widget_set_margin_top (self->grid_box, 12);
  gtk_widget_set_margin_bottom (self->grid_box, 12);
  gtk_box_append (GTK_BOX (self->grid_box), self->flow);

  scroller = gtk_scrolled_window_new ();
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scroller),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (scroller), self->grid_box);
  gtk_stack_add_named (GTK_STACK (self->stack), scroller, "cards");
  self->scroller = scroller;
  g_signal_connect (gtk_scrolled_window_get_vadjustment (GTK_SCROLLED_WINDOW (scroller)),
                    "value-changed", G_CALLBACK (scrolled_cb), self);

  emptypage = adw_status_page_new ();
  adw_status_page_set_icon_name (ADW_STATUS_PAGE (emptypage), "utilities-terminal-symbolic");
  adw_status_page_set_title (ADW_STATUS_PAGE (emptypage), "No Sessions");
  adw_status_page_set_description (ADW_STATUS_PAGE (emptypage),
                                   "Detached tabs keep running and appear here. "
                                   "Open a tab to get started.");
  gtk_stack_add_named (GTK_STACK (self->stack), emptypage, "empty");
  gtk_box_append (GTK_BOX (content), self->stack);

  /* New Tab, bottom centre. */
  bottom = gtk_box_new (GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_margin_top (bottom, 6);
  gtk_widget_set_margin_bottom (bottom, 14);
  new_tab = gtk_button_new_with_mnemonic ("New _Tab");
  gtk_widget_add_css_class (new_tab, "pill");
  gtk_widget_add_css_class (new_tab, "suggested-action");
  gtk_widget_set_halign (new_tab, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand (new_tab, TRUE);
  g_signal_connect (new_tab, "clicked", G_CALLBACK (new_tab_cb), self);
  gtk_box_append (GTK_BOX (bottom), new_tab);
  gtk_box_append (GTK_BOX (content), bottom);

  /* ---- hosting the recycled offscreen terminal (measured design) ----
   *
   * The requirement is a terminal that is mapped AND allocated at its natural size
   * (VTE lays its grid out from a real size-allocate, and GTK will not snapshot an
   * unmapped or unallocated widget) while contributing NOTHING to visible layout and
   * never being seen.
   *
   * The arrangement that satisfies all of it:
   *   main child      : an empty box, so the overlay has a trivial base
   *   overlay child 1 : the terminal, measure=FALSE -> contributes zero to the
   *                     overlay's size, and with halign/valign START is allocated
   *                     its natural size anyway
   *   overlay child 2 : the UI, measure=TRUE -> drives the size, and being added
   *                     later is drawn ON TOP, hiding the terminal
   *
   * Hidden by stacking order, not opacity: opacity anywhere in the ancestor chain
   * makes the snapshot come back blank. Measured: overlay natural tracked the UI
   * exactly (363x18 vs 363x18) while the terminal was allocated 898x540 and
   * rendered 242 distinct colours. */
  overlay = gtk_overlay_new ();
  empty = gtk_box_new (GTK_ORIENTATION_VERTICAL, 0);
  gtk_overlay_set_child (GTK_OVERLAY (overlay), empty);

  self->thumb_term = vte_terminal_new ();
  epimone_terminals_apply_appearance (self->thumb_term);
  vte_terminal_set_scrollback_lines (VTE_TERMINAL (self->thumb_term), 2000);
  /* NEVER audible. This terminal only ever REPLAYS recorded ring history, and
   * a session's tail usually contains a BEL (a completion beep from an hour
   * ago is enough; see the replay-suppression note in epimone-page.c). With
   * the bell left on, every open of the overview, the startup pre-render and
   * every background refresh re-rang those stale bells; and because this
   * terminal is deliberately outside the live-terminal registry, the
   * Preferences audible-bell switch never reached it either, so the sound
   * survived turning the setting off. apply_appearance re-applies the user's
   * bell setting, so the pin must follow it here and in
   * overview_appearance_changed_cb (same pattern as the scrollback pin). */
  vte_terminal_set_audible_bell (VTE_TERMINAL (self->thumb_term), FALSE);
  gtk_widget_set_halign (self->thumb_term, GTK_ALIGN_START);
  gtk_widget_set_valign (self->thumb_term, GTK_ALIGN_START);
  gtk_widget_set_can_target (self->thumb_term, FALSE);
  gtk_widget_set_can_focus (self->thumb_term, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), self->thumb_term);
  /* measure defaults to FALSE for overlay children; stated for the record. */
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (overlay), self->thumb_term, FALSE);

  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), content);
  gtk_overlay_set_measure_overlay (GTK_OVERLAY (overlay), content, TRUE);

  self->contents_handler =
    g_signal_connect (self->thumb_term, "contents-changed",
                      G_CALLBACK (thumb_contents_changed_cb), self);
  self->thumb_paintable = gtk_widget_paintable_new (self->thumb_term);
  self->pending = g_queue_new ();
  self->thumb_cache = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                             NULL, g_object_unref);
  self->thumb_sig = g_hash_table_new_full (g_direct_hash, g_direct_equal,
                                           NULL, g_free);

  /* The overview's own toolbar view: its header bar on top, the grid (and the
   * hidden render host) as content. This is what makes the header trade places
   * continuously with the app's: the app header belongs to `child` and zooms
   * away with it, while this one is drawn by the overview. FLAT because header
   * and content are the same tone here (.epi-overview), so a raised shadow would
   * draw a seam across one continuous surface. */
  {
    GtkWidget *ui = adw_toolbar_view_new ();

    adw_toolbar_view_set_top_bar_style (ADW_TOOLBAR_VIEW (ui), ADW_TOOLBAR_FLAT);
    adw_toolbar_view_add_top_bar (ADW_TOOLBAR_VIEW (ui), self->header);
    adw_toolbar_view_set_content (ADW_TOOLBAR_VIEW (ui), overlay);
    gtk_widget_add_css_class (ui, "epi-overview");
    /* Focusable, as the overview's resting place for keyboard focus: it keeps
     * focus INSIDE the overview (so the Escape controller sees keys) while
     * being inert; Enter on it activates nothing, which is what a dismissed
     * card menu must return to (see card_menu_closed_cb). It can never take
     * focus while the overview is closed: overview_update_targetable turns
     * can-focus off for the whole subtree then. */
    gtk_widget_set_focusable (ui, TRUE);
    self->overview_ui = ui;
    gtk_widget_set_parent (ui, GTK_WIDGET (self));
  }

  /* 400 ms / ADW_EASE, matching AdwTabOverview exactly. */
  {
    AdwAnimationTarget *target =
      adw_callback_animation_target_new ((AdwAnimationTargetFunc) open_animation_value_cb,
                                         self, NULL);

    self->open_animation = adw_timed_animation_new (GTK_WIDGET (self), 0, 0,
                                                    TRANSITION_DURATION, target);
    adw_timed_animation_set_easing (ADW_TIMED_ANIMATION (self->open_animation),
                                    ADW_EASE);
    g_signal_connect_swapped (self->open_animation, "done",
                              G_CALLBACK (open_animation_done_cb), self);
  }

  overview_update_targetable (self);

  keys = gtk_event_controller_key_new ();
  g_signal_connect (keys, "key-pressed", G_CALLBACK (overview_key_cb), self);
  gtk_widget_add_controller (GTK_WIDGET (self), keys);

  /* The recycled terminal took the appearance once, above; this keeps it (and
   * the texture cache) tracking every later palette/font change. Removed in
   * dispose. */
  epimone_terminals_add_appearance_listener (overview_appearance_changed_cb, self);
}

/* ------------------------------------------------------------------ *
 * resize tracking
 * ------------------------------------------------------------------ */

/* No resize-driven re-render: a card is a fixed snapshot scaled by its
 * GtkPicture (see overview_resize_apply_cb and the FILL note in build_card),
 * so a window resize only re-lays-out card boxes. A settle-debounced re-render
 * here would re-peek the live ring and make thumbnails reflow to the reflowed
 * live shell. Re-renders happen only on open, kill and content-change,
 * through their own paths. */

/* The palette or font changed (epimone_terminals_set_colors / _set_font). The
 * recycled offscreen terminal is deliberately outside the live-terminal
 * registry, so the broadcast that just re-skinned every visible terminal never
 * reached it: re-apply the appearance here, or every card rendered from now
 * on keeps the OLD palette. And everything already rendered IS in the old
 * palette: the per-group texture cache survives reloads by design, so those
 * textures must go, along with any in-flight render whose per-pane nodes were
 * captured under the old colors. Cards currently showing a stale texture keep
 * it until the replacement lands (no flash back to the placeholder); with the
 * overview closed the pipeline runs hidden and the next open draws entirely in
 * the new palette. */
static void
overview_appearance_changed_cb (gpointer data)
{
  EpimoneOverview *self = data;
  GtkWidget *child;

  /* A genuine re-render (new palette/font): the textures are all being rebuilt,
   * so re-capture the baked aspect from the current window, same as
   * overview_reload. */
  self->thumb_aspect = overview_render_aspect (self);

  if (self->thumb_term != NULL)
    {
      epimone_terminals_apply_appearance (self->thumb_term);
      /* apply_appearance resets scrollback and the audible bell to the user's
       * settings; the recycled renderer keeps its own deep tail and stays
       * silent. It replays history, it never rings (matches init). */
      vte_terminal_set_scrollback_lines (VTE_TERMINAL (self->thumb_term), 2000);
      vte_terminal_set_audible_bell (VTE_TERMINAL (self->thumb_term), FALSE);
    }

  render_abort (self);
  g_hash_table_remove_all (self->thumb_cache);
  /* Colours changed, so every cached texture is stale and so is every signature
   * (the tail bytes are unchanged but the pixels are not); drop both and let
   * every card re-render, which also re-stores fresh signatures. */
  g_hash_table_remove_all (self->thumb_sig);

  for (int s = 0; s < 1; s++)
    for (child = gtk_widget_get_first_child (overview_flow_at (self, s));
         child != NULL;
         child = gtk_widget_get_next_sibling (child))
      {
        GtkWidget *card = gtk_flow_box_child_get_child (GTK_FLOW_BOX_CHILD (child));
        CardInfo *ci;

        if (card == NULL)
          continue;
        ci = g_object_get_data (G_OBJECT (card), "epi-card");
        if (ci != NULL && ci->leaves != NULL && ci->leaves->len > 0)
          {
            ci->rendered = FALSE;
            ci->cur_leaf = 0;
            card_nodes_clear (ci);
          }
      }

  g_debug ("thumb: appearance changed; cache dropped, re-rendering every card");
  queue_visible_renders (self);
}

/* Deferred out of size_allocate: setting size requests from inside an allocate
 * pass invalidates the layout mid-flight. */
static void
overview_resize_apply_cb (gpointer data)
{
  EpimoneOverview *self = data;

  self->resize_idle = 0;
  overview_apply_card_sizes (self);

  /* Deliberately NO re-render here. A card is a snapshot: the offscreen grid is
   * fixed at build time and the card box keeps the texture's baked aspect
   * (overview_update_geometry uses self->thumb_aspect), so resizing only
   * rescales the card box and the GtkPicture (CONTENT_FIT_FILL) scales the
   * existing texture to match, with no distortion and no letterbox.
   * Re-rendering on resize would re-peek the live daemon ring, and because the
   * live terminals under the open overview have already reflowed to the
   * narrower window, that pulls in the collapsed prompt and makes every
   * thumbnail visibly reflow/elide as the window is dragged. Cards must stay
   * stable: they are scaled images, not live previews. Open, kill and
   * content-change still re-render through their own paths. */
}

/* Both children get the whole area, always. Neither is ever unmapped or given a
 * reduced allocation: the transition needs the child drawable at full size at
 * every moment, and keeping the terminal's allocation constant also means opening
 * the overview never reflows the user's shell. */
static void
epimone_overview_measure (GtkWidget      *widget,
                          GtkOrientation  orientation,
                          int             for_size,
                          int            *minimum,
                          int            *natural,
                          int            *minimum_baseline,
                          int            *natural_baseline)
{
  EpimoneOverview *self = EPIMONE_OVERVIEW (widget);
  int min = 0, nat = 0;
  GtkWidget *children[2];

  children[0] = self->child;
  children[1] = self->overview_ui;

  for (guint i = 0; i < G_N_ELEMENTS (children); i++)
    {
      int child_min = 0, child_nat = 0;

      if (children[i] == NULL)
        continue;
      gtk_widget_measure (children[i], orientation, for_size,
                          &child_min, &child_nat, NULL, NULL);
      min = MAX (min, child_min);
      nat = MAX (nat, child_nat);
    }

  *minimum = min;
  *natural = nat;
  if (minimum_baseline != NULL)
    *minimum_baseline = -1;
  if (natural_baseline != NULL)
    *natural_baseline = -1;
}

static void
epimone_overview_size_allocate (GtkWidget *widget,
                                int        width,
                                int        height,
                                int        baseline)
{
  EpimoneOverview *self = EPIMONE_OVERVIEW (widget);

  if (self->child != NULL)
    gtk_widget_allocate (self->child, width, height, baseline, NULL);
  if (self->overview_ui != NULL)
    gtk_widget_allocate (self->overview_ui, width, height, baseline, NULL);

  /* This is where card scaling tracks the window, the behaviour AdwTabGrid gets
   * from recomputing its layout in its own size_allocate. */
  if (self->n_cards > 0 &&
      overview_update_geometry (self, width, height) &&
      self->resize_idle == 0)
    self->resize_idle = g_idle_add_once (overview_resize_apply_cb, self);
}

static void
epimone_overview_dispose (GObject *object)
{
  EpimoneOverview *self = EPIMONE_OVERVIEW (object);

  /* Before anything the callback touches is torn down. Dispose can run twice;
   * remove is a no-op when the pair is already gone. */
  epimone_terminals_remove_appearance_listener (overview_appearance_changed_cb,
                                                self);

  /* The queue holds borrowed CardInfo pointers owned by the card widgets, so it is
   * emptied rather than freed element-wise. */
  render_abort (self);
  g_clear_weak_pointer (&self->anchor);
  g_clear_weak_pointer (&self->pending_anchor);
  g_clear_handle_id (&self->resize_idle, g_source_remove);
  g_clear_handle_id (&self->preload_timeout, g_source_remove);
  g_clear_pointer (&self->thumb_cache, g_hash_table_unref);
  g_clear_pointer (&self->thumb_sig, g_hash_table_unref);
  if (self->thumb_term != NULL && self->contents_handler != 0)
    {
      g_clear_signal_handler (&self->contents_handler, self->thumb_term);
      self->thumb_term = NULL;
    }
  if (self->pending != NULL)
    {
      g_queue_clear (self->pending);
      g_queue_free (self->pending);
      self->pending = NULL;
    }
  g_clear_object (&self->thumb_paintable);
  g_clear_object (&self->card_actions);
  g_clear_object (&self->overview_actions);
  g_clear_object (&self->open_animation);
  g_clear_pointer (&self->menu_title, g_free);
  g_clear_pointer (&self->menu_shell, g_free);
  g_clear_pointer (&self->menu_custom, g_free);
  g_clear_pointer (&self->menu_procs, g_free);
  g_clear_pointer (&self->menu_age, g_free);

  /* A plain GtkWidget owns its children by parenting, so both must go by hand. */
  if (self->child != NULL)
    {
      gtk_widget_unparent (self->child);
      self->child = NULL;
    }
  if (self->overview_ui != NULL)
    {
      gtk_widget_unparent (self->overview_ui);
      self->overview_ui = NULL;
    }

  G_OBJECT_CLASS (epimone_overview_parent_class)->dispose (object);
}

static void
epimone_overview_class_init (EpimoneOverviewClass *klass)
{
  GtkWidgetClass *widget_class = GTK_WIDGET_CLASS (klass);

  G_OBJECT_CLASS (klass)->dispose = epimone_overview_dispose;
  widget_class->map = epimone_overview_map;
  widget_class->measure = epimone_overview_measure;
  widget_class->size_allocate = epimone_overview_size_allocate;
  widget_class->snapshot = epimone_overview_snapshot;
}

GtkWidget *
epimone_overview_new (EpimoneWindow *win)
{
  EpimoneOverview *self = g_object_new (EPIMONE_TYPE_OVERVIEW, NULL);

  self->win = win;
  return GTK_WIDGET (self);
}

void
epimone_overview_set_content (EpimoneOverview *self,
                              GtkWidget       *child,
                              GtkWidget       *view)
{
  g_return_if_fail (EPIMONE_IS_OVERVIEW (self));
  g_return_if_fail (child == NULL || GTK_IS_WIDGET (child));

  if (self->child != NULL)
    gtk_widget_unparent (self->child);

  self->child = child;
  self->view = view;

  if (child != NULL)
    {
      /* Inserted BEFORE overview_ui so the grid is the later sibling; the draw
       * order is decided explicitly in snapshot either way. */
      gtk_widget_insert_before (child, GTK_WIDGET (self), self->overview_ui);
    }
  overview_update_targetable (self);
}

gboolean
epimone_overview_get_open (EpimoneOverview *self)
{
  g_return_val_if_fail (EPIMONE_IS_OVERVIEW (self), FALSE);
  return self->is_open;
}

/* Capture the LIVE tab view into @group_id's cache entry, synchronously.
 *
 * Called at open and at close, when the view is mapped and showing exactly the
 * tab that @group_id's card depicts. It is one widget-paintable snapshot plus
 * one texture render (~1-3 ms warm), so the active tab's card is pixel-current
 * the instant the grid appears, with no VTE feed, no settle wait and no queue
 * for this one card. The remaining attached tabs cannot be captured this way
 * (AdwTabView keeps non-selected pages unmapped, and the API that maps them
 * is private); they refresh through the PEEK pipeline instead. */
/* Returns TRUE only when a texture was actually rendered and cached for
 * @group_id this call; FALSE on any early bail (unmapped view, no allocation,
 * render failure). The caller uses that to decide whether the card may keep
 * this fresh texture instead of a PEEK refresh; a stale cache entry from a
 * previous open must NOT count, which is why this reports THIS call's
 * outcome. */
static gboolean
overview_capture_live_group (EpimoneOverview *self, guint64 group_id)
{
  GskRenderer *renderer;
  GtkNative *native = gtk_widget_get_native (GTK_WIDGET (self));
  GdkPaintable *paintable;
  GtkSnapshot *snapshot;
  GskRenderNode *node;
  GdkTexture *tex;
  int vw, vh, W, H, scale;
  gint64 t0 = g_get_monotonic_time ();

  if (group_id == 0 || self->view == NULL || native == NULL ||
      !gtk_widget_get_mapped (self->view))
    return FALSE;
  renderer = gtk_native_get_renderer (native);
  if (renderer == NULL)
    return FALSE;

  /* The selected page must have a real allocation, or the capture comes back
   * blank and would REPLACE a good cached texture. A page selected this very
   * frame (a card activation restoring a tab, then closing the overview) has
   * not been through a layout pass yet; skip, and let the PEEK refresh keep
   * that card honest instead. */
  if (ADW_IS_TAB_VIEW (self->view))
    {
      AdwTabPage *sel =
        adw_tab_view_get_selected_page (ADW_TAB_VIEW (self->view));
      GtkWidget *pw = sel != NULL ? adw_tab_page_get_child (sel) : NULL;

      if (pw == NULL || gtk_widget_get_width (pw) <= 0 ||
          !gtk_widget_get_mapped (pw))
        return FALSE;
    }

  vw = gtk_widget_get_width (self->view);
  vh = gtk_widget_get_height (self->view);
  if (vw <= 0 || vh <= 0)
    return FALSE;

  overview_thumb_pixel_size (self, &W, &H, &scale);

  /* Scaled per axis into the card's pixel box, exactly as render_compose places
   * panes: the card's aspect is derived from the view's, so the axes agree to
   * within the clamp and nothing visibly stretches. */
  paintable = gtk_widget_paintable_new (self->view);
  snapshot = gtk_snapshot_new ();
  gtk_snapshot_scale (snapshot, (float) W / (float) vw, (float) H / (float) vh);
  gdk_paintable_snapshot (paintable, snapshot, vw, vh);
  g_object_unref (paintable);

  node = gtk_snapshot_free_to_node (snapshot);
  if (node == NULL)
    return FALSE;
  tex = gsk_renderer_render_texture (renderer, node,
                                     &GRAPHENE_RECT_INIT (0, 0, (float) W,
                                                          (float) H));
  gsk_render_node_unref (node);
  if (tex == NULL)
    return FALSE;

  g_hash_table_insert (self->thumb_cache, GSIZE_TO_POINTER ((gsize) group_id),
                       tex);
  g_debug ("thumb: group %" G_GUINT64_FORMAT " captured live into %dx%d "
           "texture in %.1f ms", group_id, W, H,
           (g_get_monotonic_time () - t0) / 1000.0);
  return TRUE;
}

/* Live-capture the active tab with inactive-pane dimming suppressed, so the
 * cached thumbnail matches the overview's PEEK-rendered thumbnails (which never
 * carry dimming). Without this a card captured while a tab was active keeps a
 * dimmed texture, and the next open's refresh (produced by the undimmed PEEK
 * path) brightens it a beat after the zoom. See
 * epimone_pages_suppress_dim_for_capture.
 *
 * Only the scrim's DRAW is gated, not its visibility, so the live view's render
 * node is not invalidated and the capture reads a full frame, just one with the
 * dim fill skipped. Everything here is synchronous, so the real window never
 * presents a frame with dimming off. */
static gboolean
overview_capture_live_undimmed (EpimoneOverview *self, guint64 group_id)
{
  gboolean ok;

  epimone_pages_suppress_dim_for_capture (TRUE);

  /* Regenerate the live view's cached render node with the dim gate active, so
   * the capture's GtkWidgetPaintable reads a fresh, undimmed node rather than
   * replaying the last drawn (dimmed) frame. Snapshotting the view into a
   * throwaway is the documented way to force a widget subtree to redraw for a
   * paintable; epimone_overview_snapshot uses the same trick for thumb_term.
   * Verified necessary: without it the paintable replays the dimmed node and the
   * suppression has no effect. */
  if (self->view != NULL)
    {
      GtkWidget *parent = gtk_widget_get_parent (self->view);

      if (parent != NULL)
        {
          GtkSnapshot *throwaway = gtk_snapshot_new ();

          gtk_widget_snapshot_child (parent, self->view, throwaway);
          g_object_unref (throwaway);
        }
    }

  ok = overview_capture_live_group (self, group_id);
  epimone_pages_suppress_dim_for_capture (FALSE);
  return ok;
}

void
epimone_overview_animate_open (EpimoneOverview *self, guint64 active_group)
{
  g_return_if_fail (EPIMONE_IS_OVERVIEW (self));

  /* The active tab is on screen RIGHT NOW: its card's texture comes from the
   * live widget, not from the ring, so it is current to this frame. Before
   * overview_reload, so build_card's cache lookup finds it. Only when the
   * capture genuinely produced a texture this call is build_card allowed to
   * keep it (fresh_capture_group) instead of queuing a settle-time PEEK
   * refresh; if the capture bailed (e.g. the page had no allocation), the
   * card must still render normally or it would open blank. */
  if (overview_capture_live_undimmed (self, active_group))
    self->fresh_capture_group = active_group;

  /* Cards must exist before one can be flown from, and they must have been
   * allocated before their rect is known, so build them, then force a layout
   * pass rather than waiting for the next frame. */
  overview_reload (self);
  /* Suppression applies to THIS open reload only; clear it so the next reload
   * (kill, restore, palette/content change) refreshes the active card as usual. */
  self->fresh_capture_group = 0;
  overview_set_pending_anchor (self, NULL);
  if (gtk_widget_get_mapped (GTK_WIDGET (self)))
    {
      gtk_widget_allocate (self->overview_ui,
                           gtk_widget_get_width (GTK_WIDGET (self)),
                           gtk_widget_get_height (GTK_WIDGET (self)), -1, NULL);
      overview_scroll_anchor_into_view (self, active_group);
    }

  overview_animate_to (self, TRUE,
                       overview_find_anchor_for_group (self, active_group));

  /* Focus must move OFF the terminal and onto the overview's resting place the
   * moment the overview starts opening. A grab on the EpimoneOverview widget
   * itself would be a silent no-op: it is a plain non-focusable GtkWidget,
   * GTK's default grab does not descend into focusable children, and focus
   * would stay on the live VteTerminal, which consumes every key it is
   * handed, so Escape would never reach this widget's key controller and
   * every keystroke typed "into" the overview would be fed to the shell
   * underneath. Grabbing the focusable overview_ui (can-focus was switched on
   * by overview_update_targetable just above) is the same resting place
   * card_menu_closed_cb restores, keeping Enter inert while the Escape
   * controller (on an ancestor of overview_ui) sees every key. */
  gtk_widget_grab_focus (self->overview_ui);
}

void
epimone_overview_animate_close (EpimoneOverview *self, guint64 active_group)
{
  GtkWidget *anchor;

  g_return_if_fail (EPIMONE_IS_OVERVIEW (self));

  /* A card activation already named the card; otherwise fly back into whichever
   * card depicts the tab that is about to be showing. */
  anchor = self->pending_anchor != NULL
    ? self->pending_anchor
    : overview_find_anchor_for_group (self, active_group);
  overview_set_pending_anchor (self, NULL);

  /* The tab being revealed is mapped; capturing it now keeps its cache entry
   * current for the NEXT open even if the user never switches back to it.
   * Undimmed, so it matches the PEEK thumbnails and never brightens on refresh. */
  overview_capture_live_undimmed (self, active_group);

  overview_animate_to (self, FALSE, anchor);
}

