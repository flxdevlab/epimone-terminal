#include "epimone-layout.h"
#include "epimone-window.h"
#include "epimone-page.h"
#include "epimone-client.h"

#include <vte/vte.h>
#include <math.h>

/* Version 2 demoted this file. It used to hold each tab's split tree, which made
 * it a re-serialization of live widget state and therefore unable to describe a
 * tab the user had closed. The tree now lives in the daemon's per-group blob,
 * which outlives the widgets, and this file keeps only what the daemon has no
 * business knowing: window geometry, and which groups were on screen in which
 * window and order.
 *
 * Version 1 files are still read, so upgrading does not lose a layout; see
 * restore_from_v1. */
#define LAYOUT_VERSION 2
#define LAYOUT_TYPE_V2 "(ua(iibiat))"
#define LAYOUT_TYPE_V1 "(ua(ia(tv)))"
#define SAVE_DEBOUNCE_MS 250
#define RESTORE_TICK_FRAMES 12

/* Version of the per-group blob payload, independent of LAYOUT_VERSION: the two
 * are written by different code on different schedules and will drift. */
/* Version 2 appended the focused pane's grid size. A thumbnail rendered for a
 * detached tab has no widget to measure, so without this it would have to guess
 * 80x24 and come out the wrong shape. Version 3 appended the user-chosen tab
 * name ("" = none): the existing title field is overwritten from the live shell
 * title on every sync, so a rename needs its own field to survive. Older blobs
 * are still read; their grid / custom name is simply unknown. */
#define BLOB_VERSION 3

/* GVariant type of the blob payload, and of the groups state file. */
#define BLOB_TYPE    "(utsvqqs)"    /* v3: ... plus the custom tab name */
#define BLOB_TYPE_V2 "(utsvqq)"     /* v2: ... plus focused pane cols, rows */
#define BLOB_TYPE_V1 "(utsv)"
#define GROUPS_TYPE "(utat)"
#define GROUPS_VERSION 1

/* Defined further down with the other blob code, but needed by restore_group. */
static GVariant *blob_parse (const guint8 *blob, gsize len,
                             guint64 *out_focused, const char **out_title,
                             const char **out_custom, GVariant **out_tree,
                             guint *out_cols, guint *out_rows);

static gboolean g_restoring = FALSE;
static gboolean g_frozen = FALSE;
static guint    g_save_source = 0;

/* One timer, two sinks. layout.json is only rewritten for its original
 * triggers; the group blobs also pick up divider drags, title changes and
 * unzoom. */
static gboolean g_layout_dirty = FALSE;
static gboolean g_groups_dirty = FALSE;

/* Group bookkeeping is best-effort: it must never take out a terminal. Failures
 * are logged once per kind rather than on every debounce tick, so a daemon that
 * has gone away does not fill the log. */
static gboolean g_warned_group_new = FALSE;
static gboolean g_warned_group_set = FALSE;
static gboolean g_warned_group_add = FALSE;

/* Two instance ids, answering two different questions. Both gate restore; see
 * epimone_layout_restore_group.
 *
 * g_session_instance: the daemon this GUI process bound to, captured once at
 * startup and never updated. If the live daemon stops matching it, the daemon
 * has been replaced and every id the GUI holds is stale.
 *
 * g_recorded_instance: what groups.json says, read at startup and then kept in
 * step by each sync. Before the first sync of a session it still holds the
 * previous run's value, which is exactly when stored group ids may belong to a
 * daemon that no longer exists. */
static guint64  g_session_instance = 0;
static guint64  g_recorded_instance = 0;
static gboolean g_instance_checked = FALSE;

/* ------------------------------------------------------------------ *
 * paths
 * ------------------------------------------------------------------ */

static char *
layout_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "epimone", "layout.json", NULL);
}

/* Separate from layout.json on purpose: the group record has a different
 * lifetime (it tracks the daemon instance, not the window arrangement). */
static char *
groups_path (void)
{
  return g_build_filename (g_get_user_state_dir (), "epimone", "groups.json", NULL);
}

/* ------------------------------------------------------------------ *
 * serialization
 * ------------------------------------------------------------------ */

/* Build a boxed-variant node ("v") for a split-tree widget. Internal nodes are
 * GtkPaned; every other node is a leaf (a scrim GtkOverlay around a
 * GtkScrolledWindow wrapping a VTE), from which
 * epimone_terminal_session_id() recovers the session id. */
static GVariant *
node_to_variant (GtkWidget *w)
{
  if (w != NULL && GTK_IS_PANED (w))
    {
      GtkPaned *p = GTK_PANED (w);
      GtkOrientation o = gtk_orientable_get_orientation (GTK_ORIENTABLE (p));
      int dim = (o == GTK_ORIENTATION_HORIZONTAL)
                  ? gtk_widget_get_width (w) : gtk_widget_get_height (w);
      int pos = gtk_paned_get_position (p);
      double ratio = (dim > 0) ? (double) pos / (double) dim : 0.5;
      GVariant *start = node_to_variant (gtk_paned_get_start_child (p));
      GVariant *end = node_to_variant (gtk_paned_get_end_child (p));

      return g_variant_new_variant (
        g_variant_new ("(ssd@v@v)", "split",
                       (o == GTK_ORIENTATION_HORIZONTAL) ? "h" : "v",
                       ratio, start, end));
    }

  /* Leaf (or NULL): a 0 id round-trips as a pruned pane on restore. */
  {
    guint64 id = (w != NULL) ? epimone_terminal_session_id (w) : 0;
    return g_variant_new_variant (g_variant_new ("(st)", "leaf", id));
  }
}

/* One window as (width, height, maximized, active_tab_index, group_ids).
 *
 * No split trees: the arrangement of each tab is the daemon's business. What
 * is left here is the part the daemon cannot know: how big the window was and
 * which groups were showing in it, in tab order. */
static GVariant *
window_to_variant (EpimoneWindow *win)
{
  AdwTabView *tv = epimone_window_get_tab_view (win);
  GVariantBuilder gids;
  int n = adw_tab_view_get_n_pages (tv);
  int active = 0;
  AdwTabPage *sel = adw_tab_view_get_selected_page (tv);
  int width = gtk_widget_get_width (GTK_WIDGET (win));
  int height = gtk_widget_get_height (GTK_WIDGET (win));
  gboolean maximized = gtk_window_is_maximized (GTK_WINDOW (win));

  if (sel != NULL)
    active = adw_tab_view_get_page_position (tv, sel);

  g_variant_builder_init (&gids, G_VARIANT_TYPE ("at"));
  for (int i = 0; i < n; i++)
    {
      AdwTabPage *tp = adw_tab_view_get_nth_page (tv, i);
      EpimonePage *page = EPIMONE_PAGE (adw_tab_page_get_child (tp));
      guint64 gid = epimone_page_get_group_id (page);

      /* A tab whose group has not been created yet (the sync runs on the same
       * debounce and may not have got to it) is simply not listed; the group it
       * gets a moment later is picked up by the next save. */
      if (gid != 0)
        g_variant_builder_add (&gids, "t", gid);
    }

  return g_variant_new ("(iibi@at)", width, height, maximized, active,
                        g_variant_builder_end (&gids));
}

void
epimone_layout_save_now (void)
{
  GApplication *app = g_application_get_default ();
  GVariantBuilder wins;
  GVariant *top;
  char *text;
  g_autofree char *path = NULL;
  g_autofree char *dir = NULL;
  int count = 0;
  GError *err = NULL;

  if (app == NULL)
    return;

  g_variant_builder_init (&wins, G_VARIANT_TYPE ("a(iibiat)"));
  for (GList *l = gtk_application_get_windows (GTK_APPLICATION (app)); l != NULL; l = l->next)
    {
      if (!EPIMONE_IS_WINDOW (l->data))
        continue;
      g_variant_builder_add_value (&wins, window_to_variant (EPIMONE_WINDOW (l->data)));
      count++;
    }

  if (count == 0)
    {
      /* Nothing to persist; don't clobber a good file with an empty one. */
      g_variant_builder_clear (&wins);
      return;
    }

  top = g_variant_new ("(u@a(iibiat))", (guint32) LAYOUT_VERSION,
                       g_variant_builder_end (&wins));
  g_variant_ref_sink (top);
  text = g_variant_print (top, TRUE);

  path = layout_path ();
  dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);

  if (!g_file_set_contents (path, text, -1, &err))
    {
      g_warning ("epimone: could not write layout: %s", err->message);
      g_clear_error (&err);
    }

  g_free (text);
  g_variant_unref (top);
}

/* ------------------------------------------------------------------ *
 * daemon-side groups
 *
 * One group per tab. The group's blob is the tab's arrangement, and the group's
 * members are the tab's sessions. Everything here is best-effort: a rejected
 * request is logged and skipped, never propagated.
 * ------------------------------------------------------------------ */

/* Collect the session ids of every live pane in a split tree. */
static void
collect_session_ids (GtkWidget *w, GArray *out)
{
  if (w == NULL)
    return;
  if (GTK_IS_PANED (w))
    {
      collect_session_ids (gtk_paned_get_start_child (GTK_PANED (w)), out);
      collect_session_ids (gtk_paned_get_end_child (GTK_PANED (w)), out);
      return;
    }
  {
    guint64 id = epimone_terminal_session_id (w);
    if (id != 0)
      g_array_append_val (out, id);
  }
}

/* Serialize one tab into its blob payload. Reuses node_to_variant, the same
 * function that produces layout.json's split-tree nodes, so there is one
 * implementation of the tree format. Returns NULL if the tab has no tree yet.
 * Caller frees; *out_len excludes the terminating NUL. */
static char *
page_blob_text (EpimonePage *page, gsize *out_len)
{
  GtkWidget *root = epimone_page_get_tree_root (page);
  GVariant *top;
  char *text;

  *out_len = 0;
  if (root == NULL)
    return NULL;

  /* "@v", not "v": node_to_variant already returns a boxed variant, and a plain
   * "v" would box it a second time. Same idiom as window_to_variant's "(t@v)". */
  {
    guint cols = 0, rows = 0;

    epimone_page_get_focused_grid (page, &cols, &rows);
    /* The SHELL title, not the display title: with a custom name set the two
     * differ, and this field is the "automatic" state a cleared name returns
     * to, so recording the custom name here would make clearing a no-op. */
    top = g_variant_new ("(uts@vqqs)",
                         (guint32) BLOB_VERSION,
                         epimone_page_get_focused_session (page),
                         epimone_page_get_shell_title (page) ?: "",
                         node_to_variant (root),
                         (guint16) cols, (guint16) rows,
                         epimone_page_get_custom_title (page) ?: "");
  }
  g_variant_ref_sink (top);
  text = g_variant_print (top, TRUE);
  g_variant_unref (top);

  *out_len = strlen (text);
  return text;
}

/* Push one tab's arrangement and membership into the daemon. */
static void
sync_page_group (EpimonePage *page)
{
  g_autofree char *text = NULL;
  g_autoptr (GArray) ids = NULL;
  gsize len = 0;
  guint64 gid;
  GError *err = NULL;

  /* While a pane is zoomed the enclosing GtkPaneds report zoom positions rather
   * than the user's ratios, so writing now would record a layout the user never
   * chose. The existing blob stays; unzoom schedules a fresh write. */
  if (epimone_page_is_zoomed (page))
    return;

  ids = g_array_new (FALSE, FALSE, sizeof (guint64));
  collect_session_ids (epimone_page_get_tree_root (page), ids);
  if (ids->len == 0)
    return;   /* no live panes: nothing to own a group */

  text = page_blob_text (page, &len);
  if (text == NULL)
    return;

  gid = epimone_page_get_group_id (page);

  /* An existing group may have been destroyed in the meantime: the daemon drops
   * a group as soon as it loses its last member, so a tab whose panes all exited
   * and were killed no longer has one. Treat that as "make a new one" rather
   * than an error, and do it in this same pass so the tab is never left
   * group-less. */
  if (gid != 0 && !epimone_client_group_set (gid, (const guint8 *) text, len, &err))
    {
      if (!g_warned_group_set)
        {
          g_warning ("epimone: could not update group %" G_GUINT64_FORMAT
                     " (%s); recreating", gid,
                     err != NULL ? err->message : "unknown error");
          g_warned_group_set = TRUE;
        }
      g_clear_error (&err);
      epimone_page_set_group_id (page, 0);
      gid = 0;
    }

  if (gid == 0)
    {
      gid = epimone_client_group_new ((const guint8 *) text, len, &err);
      if (gid == 0)
        {
          if (!g_warned_group_new)
            {
              g_warning ("epimone: could not create layout group (%s); "
                         "tabs will not be restorable this session",
                         err != NULL ? err->message : "unknown error");
              g_warned_group_new = TRUE;
            }
          g_clear_error (&err);
          return;
        }
      epimone_page_set_group_id (page, gid);
    }

  /* Enroll panes the daemon does not know about yet. GROUP_ADD is an atomic
   * move, so this is also the right call if a pane ever arrives from another
   * tab. Already-enrolled panes are skipped, which makes the steady state a
   * single GROUP_SET per tab per tick. */
  for (guint i = 0; i < ids->len; i++)
    {
      guint64 sid = g_array_index (ids, guint64, i);

      if (epimone_page_session_enrolled (page, sid))
        continue;
      if (epimone_client_group_add (gid, sid, &err))
        {
          epimone_page_mark_enrolled (page, sid);
        }
      else
        {
          if (!g_warned_group_add)
            {
              g_warning ("epimone: could not add session %" G_GUINT64_FORMAT
                         " to group %" G_GUINT64_FORMAT " (%s)", sid, gid,
                         err != NULL ? err->message : "unknown error");
              g_warned_group_add = TRUE;
            }
          g_clear_error (&err);
        }
    }
}

/* Record which groups are on screen, stamped with the daemon instance they
 * belong to. The stamp is checked before any of these ids is trusted (see
 * instance_ok). */
static void
save_groups_state (GArray *gids)
{
  GVariantBuilder ids;
  GVariant *top;
  g_autofree char *text = NULL;
  g_autofree char *path = NULL;
  g_autofree char *dir = NULL;
  guint64 instance;
  GError *err = NULL;

  instance = epimone_client_instance_id (NULL);
  /* Keep the in-memory copy in step with the file, so the restore guard tracks
   * the daemon this session is actually talking to. */
  if (instance != 0)
    g_recorded_instance = instance;

  g_variant_builder_init (&ids, G_VARIANT_TYPE ("at"));
  for (guint i = 0; i < gids->len; i++)
    g_variant_builder_add (&ids, "t", g_array_index (gids, guint64, i));

  /* "@at" takes the finished GVariant; a bare "at" would want the builder
   * itself. Matches epimone_layout_save_now's "(u@a(ia(tv)))". */
  top = g_variant_new ("(ut@at)", (guint32) GROUPS_VERSION, instance,
                       g_variant_builder_end (&ids));
  g_variant_ref_sink (top);
  text = g_variant_print (top, TRUE);
  g_variant_unref (top);

  path = groups_path ();
  dir = g_path_get_dirname (path);
  g_mkdir_with_parents (dir, 0700);
  if (!g_file_set_contents (path, text, -1, &err))
    {
      g_debug ("epimone: could not write group state: %s", err->message);
      g_clear_error (&err);
    }
}

/* Walk every open tab, sync its group, and record the result. */
static void
sync_groups_now (void)
{
  GApplication *app = g_application_get_default ();
  g_autoptr (GArray) gids = NULL;

  if (app == NULL)
    return;

  gids = g_array_new (FALSE, FALSE, sizeof (guint64));

  for (GList *l = gtk_application_get_windows (GTK_APPLICATION (app));
       l != NULL; l = l->next)
    {
      AdwTabView *tv;
      int n;

      if (!EPIMONE_IS_WINDOW (l->data))
        continue;

      tv = epimone_window_get_tab_view (EPIMONE_WINDOW (l->data));
      n = adw_tab_view_get_n_pages (tv);
      for (int i = 0; i < n; i++)
        {
          AdwTabPage *tp = adw_tab_view_get_nth_page (tv, i);
          EpimonePage *page = EPIMONE_PAGE (adw_tab_page_get_child (tp));
          guint64 gid;

          sync_page_group (page);
          gid = epimone_page_get_group_id (page);
          if (gid != 0)
            g_array_append_val (gids, gid);
        }
    }

  save_groups_state (gids);
}

void
epimone_layout_check_instance (void)
{
  g_autofree char *path = groups_path ();
  g_autofree char *text = NULL;
  GVariant *top;
  guint32 version = 0;
  guint64 recorded = 0;
  guint64 live;
  GVariant *ids = NULL;

  live = epimone_client_instance_id (NULL);
  g_instance_checked = TRUE;
  if (g_session_instance == 0)
    g_session_instance = live;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    {
      g_debug ("epimone: no recorded group state; daemon instance %"
               G_GUINT64_FORMAT, live);
      return;
    }

  top = g_variant_parse (G_VARIANT_TYPE (GROUPS_TYPE), text, NULL, NULL, NULL);
  if (top == NULL)
    {
      g_debug ("epimone: could not parse recorded group state");
      return;
    }

  /* "@at", not the bare "at" the type string uses: for g_variant_get a bare array
   * format wants a GVariantIter**, and handing it a GVariant** silently stores an
   * iterator where a variant is expected. Mirrors the "(ut@at)" used to build it. */
  g_variant_get (top, "(ut@at)", &version, &recorded, &ids);
  g_recorded_instance = recorded;

  if (live == 0)
    g_debug ("epimone: daemon reports no instance id");
  else if (recorded == 0)
    g_debug ("epimone: recorded group state predates instance stamping");
  else if (recorded != live)
    g_message ("epimone: daemon instance changed (%" G_GUINT64_FORMAT " -> %"
               G_GUINT64_FORMAT "); %" G_GSIZE_FORMAT
               " recorded group id(s) are stale",
               recorded, live, ids != NULL ? g_variant_n_children (ids) : 0);
  else
    g_debug ("epimone: daemon instance %" G_GUINT64_FORMAT " matches recorded "
             "group state (%" G_GSIZE_FORMAT " group(s))", live,
             ids != NULL ? g_variant_n_children (ids) : 0);

  g_clear_pointer (&ids, g_variant_unref);
  g_variant_unref (top);
}

/* ------------------------------------------------------------------ *
 * debounce
 * ------------------------------------------------------------------ */

static gboolean
save_timeout_cb (gpointer data)
{
  gboolean do_layout = g_layout_dirty;
  gboolean do_groups = g_groups_dirty;

  (void) data;
  g_save_source = 0;
  g_layout_dirty = FALSE;
  g_groups_dirty = FALSE;

  /* Groups first. layout.json records group ids, so a tab whose group has not
   * been created yet would be left out of the file entirely, and with the tree
   * not stored there, a missing id means a tab that cannot be rebuilt. Syncing
   * first guarantees every tab has an id to record. */
  if (do_groups)
    sync_groups_now ();
  if (do_layout)
    epimone_layout_save_now ();
  return G_SOURCE_REMOVE;
}

/* Arm the shared timer if it is not already running. */
static void
arm_save (void)
{
  if (g_restoring || g_frozen)
    return;
  if (g_save_source != 0)
    return;
  g_save_source = g_timeout_add (SAVE_DEBOUNCE_MS, save_timeout_cb, NULL);
}

void
epimone_layout_schedule_save (void)
{
  if (g_restoring || g_frozen)
    return;
  g_layout_dirty = TRUE;
  g_groups_dirty = TRUE;
  arm_save ();
}

void
epimone_layout_schedule_group_save (void)
{
  if (g_restoring || g_frozen)
    return;
  g_groups_dirty = TRUE;
  arm_save ();
}

void
epimone_layout_begin_shutdown (void)
{
  if (g_frozen)
    return;
  if (g_save_source != 0)
    {
      g_source_remove (g_save_source);
      g_save_source = 0;
    }
  /* Blobs first, then the file that references them, for the same reason
   * save_timeout_cb does it in that order. */
  sync_groups_now ();
  epimone_layout_save_now ();
  g_layout_dirty = FALSE;
  g_groups_dirty = FALSE;
  g_frozen = TRUE;
}

/* ------------------------------------------------------------------ *
 * restore
 * ------------------------------------------------------------------ */

/* Recursively rebuild a subtree, pruning leaves whose session is not alive.
 * Returns the built widget, or NULL if the whole subtree was pruned. */
static GtkWidget *
build_node (EpimonePage *page, GVariant *node_v, GHashTable *alive)
{
  GVariant *inner = g_variant_get_variant (node_v);
  const GVariantType *t = g_variant_get_type (inner);
  GtkWidget *result = NULL;

  if (g_variant_type_equal (t, G_VARIANT_TYPE ("(st)")))
    {
      const char *tag;
      guint64 id;
      g_variant_get (inner, "(&st)", &tag, &id);
      if (id != 0 && g_hash_table_contains (alive, GUINT_TO_POINTER ((guint) id)))
        result = epimone_page_create_terminal_for_session (page, id);
    }
  else if (g_variant_type_equal (t, G_VARIANT_TYPE ("(ssdvv)")))
    {
      const char *tag, *orient;
      double ratio;
      GVariant *startv = NULL, *endv = NULL;
      GtkWidget *start, *end;

      g_variant_get (inner, "(&s&sd@v@v)", &tag, &orient, &ratio, &startv, &endv);
      start = build_node (page, startv, alive);
      end = build_node (page, endv, alive);
      g_variant_unref (startv);
      g_variant_unref (endv);

      if (start == NULL && end == NULL)
        result = NULL;
      else if (start == NULL)
        result = end;
      else if (end == NULL)
        result = start;
      else
        {
          GtkOrientation o = (orient[0] == 'h')
                               ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL;
          GtkWidget *paned = epimone_page_new_paned (o);
          double *rp = g_new (double, 1);

          *rp = ratio;
          gtk_paned_set_start_child (GTK_PANED (paned), start);
          gtk_paned_set_end_child (GTK_PANED (paned), end);
          g_object_set_data_full (G_OBJECT (paned), "epi-ratio", rp, g_free);
          result = paned;
        }
    }

  g_variant_unref (inner);
  return result;
}

/* Apply saved split ratios once the widgets have real allocations. */
static void
apply_ratios (GtkWidget *w)
{
  if (w == NULL)
    return;
  if (GTK_IS_PANED (w))
    {
      double *rp = g_object_get_data (G_OBJECT (w), "epi-ratio");
      if (rp != NULL)
        {
          GtkOrientation o = gtk_orientable_get_orientation (GTK_ORIENTABLE (w));
          int dim = (o == GTK_ORIENTATION_HORIZONTAL)
                      ? gtk_widget_get_width (w) : gtk_widget_get_height (w);
          if (dim > 0)
            gtk_paned_set_position (GTK_PANED (w), (int) (*rp * dim + 0.5));
        }
      apply_ratios (gtk_paned_get_start_child (GTK_PANED (w)));
      apply_ratios (gtk_paned_get_end_child (GTK_PANED (w)));
    }
}

/* Ratios can only be applied once the widgets have real allocations, which takes
 * a few frames, so this runs on the frame clock rather than once.
 *
 * Two modes. With `win` set it covers every page in that window, which is the
 * startup path rebuilding a whole window at once. With `page` set it covers just
 * that page, used when a single tab is rebuilt from a group, so re-running does
 * not reach into tabs the user has since adjusted by hand. (Paneds keep their
 * "epi-ratio" data after a restore, so a window-wide pass would re-apply stale
 * ratios to an earlier-restored tab and undo the user's drags.) */
typedef struct {
  EpimoneWindow *win;         /* borrowed; NULL when page is set */
  EpimonePage   *page;        /* owned ref; NULL for the whole-window path */
  guint64        focus_session;
  int            frames_left;
  gboolean       focused_done;
} RestoreTick;

static void
restore_tick_free (gpointer data)
{
  RestoreTick *rt = data;

  g_clear_object (&rt->page);
  g_free (rt);
}

static gboolean
restore_tick_cb (GtkWidget *widget, GdkFrameClock *clock, gpointer data)
{
  RestoreTick *rt = data;

  (void) widget;
  (void) clock;

  if (rt->page != NULL)
    {
      apply_ratios (epimone_page_get_tree_root (rt->page));
      if (!rt->focused_done)
        {
          if (rt->focus_session != 0)
            epimone_page_focus_session (rt->page, rt->focus_session);
          else
            epimone_page_focus_terminal (rt->page);
          rt->focused_done = TRUE;
        }
    }
  else
    {
      AdwTabView *tv = epimone_window_get_tab_view (rt->win);
      int n = adw_tab_view_get_n_pages (tv);

      for (int i = 0; i < n; i++)
        {
          AdwTabPage *tp = adw_tab_view_get_nth_page (tv, i);
          EpimonePage *page = EPIMONE_PAGE (adw_tab_page_get_child (tp));
          apply_ratios (epimone_page_get_tree_root (page));
        }

      if (!rt->focused_done)
        {
          AdwTabPage *sel = adw_tab_view_get_selected_page (tv);
          if (sel != NULL && rt->focus_session != 0)
            epimone_page_focus_session (EPIMONE_PAGE (adw_tab_page_get_child (sel)),
                                        rt->focus_session);
          rt->focused_done = TRUE;
        }
    }

  if (--rt->frames_left <= 0)
    return G_SOURCE_REMOVE;
  return G_SOURCE_CONTINUE;
}

/* Which sessions can a leaf legitimately attach to right now?
 *
 * Only live ones. A session that has exited is still listed (dead, with its ring
 * intact) but attaching to it would replay the ring and immediately hit EOF, so a
 * leaf naming one is pruned exactly as a leaf naming session id 0 is. Shared by
 * the startup path and group restore so both prune identically. */
static GHashTable *
build_alive_set (GError **error)
{
  GPtrArray *sessions = epimone_client_list_sessions (error);
  GHashTable *alive;

  if (sessions == NULL)
    return NULL;

  alive = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint i = 0; i < sessions->len; i++)
    {
      EpiSessionInfo *info = g_ptr_array_index (sessions, i);
      if (info->alive)
        g_hash_table_add (alive, GUINT_TO_POINTER ((guint) info->id));
    }
  g_ptr_array_unref (sessions);
  return alive;
}

/* session id -> the group that currently owns it, from one GROUP_LIST.
 * Returns NULL if the daemon cannot be asked. */
static GHashTable *
build_session_group_map (void)
{
  g_autoptr (GPtrArray) groups = epimone_client_list_groups (NULL, NULL);
  GHashTable *map;

  if (groups == NULL)
    return NULL;

  map = g_hash_table_new (g_direct_hash, g_direct_equal);
  for (guint i = 0; i < groups->len; i++)
    {
      EpiGroupInfo *g = g_ptr_array_index (groups, i);

      for (guint m = 0; m < g->members->len; m++)
        {
          guint64 sid = g_array_index (g->members, guint64, m);
          g_hash_table_insert (map, GUINT_TO_POINTER ((guint) sid),
                               GUINT_TO_POINTER ((guint) g->id));
        }
    }
  return map;
}

/* A page has just been rebuilt around existing sessions. If those sessions are
 * already in a group, adopt it instead of letting the next sync mint a new one.
 *
 * Without this, restarting the GUI would rebuild every tab from layout.json with
 * no group, the sync would create fresh groups, and GROUP_ADD would move the
 * sessions across, emptying and destroying the original groups. The arrangement
 * would survive but every group id would change on each launch, which makes stored
 * references worthless. Adopting keeps a tab's group id stable for as long as its
 * sessions live. */
static void
adopt_existing_group (EpimonePage *page, GtkWidget *root, GHashTable *sgmap)
{
  g_autoptr (GArray) ids = NULL;
  guint64 gid = 0;

  if (sgmap == NULL || root == NULL)
    return;

  ids = g_array_new (FALSE, FALSE, sizeof (guint64));
  collect_session_ids (root, ids);

  for (guint i = 0; i < ids->len && gid == 0; i++)
    {
      guint64 sid = g_array_index (ids, guint64, i);
      gpointer v = g_hash_table_lookup (sgmap, GUINT_TO_POINTER ((guint) sid));

      if (v != NULL)
        gid = (guint64) GPOINTER_TO_UINT (v);
    }

  if (gid == 0)
    return;

  epimone_page_set_group_id (page, gid);
  /* Every pane here is already a member (that membership is what kept the group
   * alive), so record them rather than re-sending GROUP_ADD. Any pane that turns
   * out to belong elsewhere is pulled in by the next sync's atomic move. */
  for (guint i = 0; i < ids->len; i++)
    epimone_page_mark_enrolled (page, g_array_index (ids, guint64, i));
}

/* MIGRATION path: a version 1 layout file, which still carries split trees.
 *
 * Kept so upgrading does not lose an existing layout. Nothing writes this format
 * any more (the first save after startup replaces the file with version 2), so
 * this runs at most once per install. It rebuilds trees out of the file exactly as
 * before, and adopt_existing_group() then binds each rebuilt tab to the group its
 * sessions are already in, which is what carries the layout forward into the new
 * scheme. */
static gboolean
restore_from_v1 (AdwApplication *app, GVariant *top)
{
  GVariant *windows = NULL;
  GVariantIter wit;
  GVariant *winv;
  GHashTable *alive;
  GHashTable *sgmap;
  guint32 version;
  int created = 0;
  GError *err = NULL;

  /* Which sessions are alive right now? */
  alive = build_alive_set (&err);
  if (alive == NULL)
    {
      g_clear_error (&err);
      g_variant_unref (top);
      return FALSE;
    }
  /* Which group already owns each session, so rebuilt tabs keep their group id. */
  sgmap = build_session_group_map ();

  g_restoring = TRUE;

  g_variant_get (top, "(u@a(ia(tv)))", &version, &windows);
  g_variant_iter_init (&wit, windows);
  while ((winv = g_variant_iter_next_value (&wit)) != NULL)
    {
      int active;
      GVariant *tabs = NULL;
      GVariantIter tit;
      GVariant *tabv;
      GPtrArray *pages = g_ptr_array_new ();
      guint64 active_focus = 0;
      int tab_index = 0;

      g_variant_get (winv, "(i@a(tv))", &active, &tabs);
      g_variant_iter_init (&tit, tabs);
      while ((tabv = g_variant_iter_next_value (&tit)) != NULL)
        {
          guint64 focused;
          GVariant *node_v = NULL;
          EpimonePage *page = EPIMONE_PAGE (epimone_page_new_empty ());
          GtkWidget *root;

          g_variant_get (tabv, "(t@v)", &focused, &node_v);
          root = build_node (page, node_v, alive);
          g_variant_unref (node_v);

          if (root == NULL)
            {
              /* No surviving panes in this tab: drop it. */
              g_object_ref_sink (page);
              g_object_unref (page);
            }
          else
            {
              epimone_page_set_tree_root (page, root);
              adopt_existing_group (page, root, sgmap);
              if (tab_index == active)
                active_focus = focused;
              g_ptr_array_add (pages, page);
            }
          tab_index++;
          g_variant_unref (tabv);
        }
      g_variant_unref (tabs);

      if (pages->len > 0)
        {
          GtkWidget *win = epimone_window_new (app);
          RestoreTick *rt;

          for (guint i = 0; i < pages->len; i++)
            epimone_window_adopt_page (EPIMONE_WINDOW (win),
                                       EPIMONE_PAGE (g_ptr_array_index (pages, i)));

          epimone_window_set_active_tab (EPIMONE_WINDOW (win),
                                         active < (int) pages->len ? active : 0);
          gtk_window_present (GTK_WINDOW (win));

          rt = g_new0 (RestoreTick, 1);
          rt->win = EPIMONE_WINDOW (win);
          rt->focus_session = active_focus;
          rt->frames_left = RESTORE_TICK_FRAMES;
          gtk_widget_add_tick_callback (win, restore_tick_cb, rt,
                                        restore_tick_free);
          created++;
        }

      g_ptr_array_free (pages, TRUE);
      g_variant_unref (winv);
    }

  g_variant_unref (windows);
  g_variant_unref (top);
  g_hash_table_destroy (alive);
  if (sgmap != NULL)
    g_hash_table_destroy (sgmap);

  g_restoring = FALSE;

  /* Rewrite the file in the new format straight away. Without this the migration
   * only happens in memory: layout.json keeps its version 1 contents until some
   * unrelated event marks the layout dirty, so the next launch would migrate all
   * over again. */
  if (created > 0)
    epimone_layout_schedule_save ();
  return created > 0;
}

/* ------------------------------------------------------------------ *
 * restore one tab from its daemon group
 * ------------------------------------------------------------------ */

#define RESTORE_ERROR epimone_layout_error_quark ()

static GQuark
epimone_layout_error_quark (void)
{
  return g_quark_from_static_string ("epimone-layout-error");
}

/* Is the daemon answering now the one the recorded group ids came from?
 *
 * Session and group ids are handed out from a counter that restarts at 1 on every
 * daemon start, so after a restart group id 2 can name a completely unrelated tab
 * and session id 3 an unrelated shell. Rebuilding from a stale id would attach a
 * tab to whatever happens to hold those numbers now, which is the one failure this
 * whole mechanism must not have. Refusing is the entire reason the instance id
 * exists.
 *
 * In normal operation the recorded value tracks the live daemon (the debounced
 * sync rewrites it), so this passes; it only fails in the window where the GUI
 * still holds ids from a daemon that has since been replaced. */
static gboolean
instance_ok (GError **error)
{
  guint64 live = epimone_client_instance_id (NULL);

  if (live == 0)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "daemon does not report an instance id; refusing to restore");
      return FALSE;
    }
  if (!g_instance_checked || g_session_instance == 0)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "daemon instance never established; refusing to restore "
                   "(live instance %" G_GUINT64_FORMAT ")", live);
      return FALSE;
    }
  /* The daemon has been replaced since this window opened. Its sessions died with
   * it and the ids have been reissued to unrelated things. Permanent for the life
   * of this process, since g_session_instance is never refreshed. */
  if (live != g_session_instance)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "daemon was replaced (bound to %" G_GUINT64_FORMAT ", live %"
                   G_GUINT64_FORMAT "); every stored id is stale, refusing to "
                   "restore", g_session_instance, live);
      return FALSE;
    }
  /* groups.json still describes a different daemon's ids: true between startup
   * and this session's first sync, which is when a caller working from the
   * recorded id list would be using ids that no longer mean anything. */
  if (g_recorded_instance != 0 && g_recorded_instance != live)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "recorded group state is from daemon instance %"
                   G_GUINT64_FORMAT " but the live daemon is %" G_GUINT64_FORMAT
                   "; stored group and session ids are stale, refusing to restore",
                   g_recorded_instance, live);
      return FALSE;
    }
  return TRUE;
}

/* Find the page already showing any of @members, across every open window. */
static EpimonePage *
find_page_for_members (GArray *members, EpimoneWindow **out_win)
{
  GApplication *app = g_application_get_default ();

  if (out_win != NULL)
    *out_win = NULL;
  if (app == NULL || members == NULL)
    return NULL;

  for (GList *l = gtk_application_get_windows (GTK_APPLICATION (app));
       l != NULL; l = l->next)
    {
      if (!EPIMONE_IS_WINDOW (l->data))
        continue;
      for (guint i = 0; i < members->len; i++)
        {
          guint64 sid = g_array_index (members, guint64, i);
          EpimonePage *page =
            epimone_window_find_page_for_session (EPIMONE_WINDOW (l->data), sid);

          if (page != NULL)
            {
              if (out_win != NULL)
                *out_win = EPIMONE_WINDOW (l->data);
              return page;
            }
        }
    }
  return NULL;
}

gboolean
epimone_layout_restore_group (EpimoneWindow *win, guint64 group_id,
                              GError **error)
{
  g_autoptr (GPtrArray) groups = NULL;
  EpiGroupInfo *found = NULL;
  GVariant *top = NULL;
  GVariant *tree = NULL;
  guint64 focused = 0;
  const char *title = NULL;
  const char *custom = NULL;
  GHashTable *alive = NULL;
  EpimonePage *page;
  GtkWidget *root;
  g_autoptr (GArray) ids = NULL;
  RestoreTick *rt;
  gboolean was_restoring;

  g_return_val_if_fail (EPIMONE_IS_WINDOW (win), FALSE);

  if (group_id == 0)
    {
      g_set_error (error, RESTORE_ERROR, 1, "group id 0 is not a group");
      return FALSE;
    }
  if (!instance_ok (error))
    return FALSE;

  groups = epimone_client_list_groups (NULL, error);
  if (groups == NULL)
    return FALSE;

  for (guint i = 0; i < groups->len; i++)
    {
      EpiGroupInfo *g = g_ptr_array_index (groups, i);
      if (g->id == group_id)
        {
          found = g;
          break;
        }
    }
  if (found == NULL)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "no group %" G_GUINT64_FORMAT, group_id);
      return FALSE;
    }

  /* Already on screen: select that tab rather than building a second copy of it.
   * Rebuilding would attach its sessions again, which the daemon implements by
   * detaching the incumbent, so the live tab's panes would go dead. Treating
   * this as "show me that tab" is both safe and what the user means. */
  {
    EpimoneWindow *existing_win = NULL;
    EpimonePage *existing = find_page_for_members (found->members, &existing_win);

    if (existing != NULL)
      {
        g_message ("epimone: group %" G_GUINT64_FORMAT
                   " is already on screen; selecting its tab",
                   group_id);
        epimone_window_select_page (existing_win, existing);
        return TRUE;
      }
  }

  if (found->blob == NULL || found->blob_len == 0)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "group %" G_GUINT64_FORMAT " has no stored layout", group_id);
      return FALSE;
    }

  top = blob_parse (found->blob, found->blob_len, &focused, &title, &custom,
                    &tree, NULL, NULL);
  if (top == NULL)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "group %" G_GUINT64_FORMAT " layout does not parse, or is a "
                   "version this build does not understand (expects %s, %s or %s)",
                   group_id, BLOB_TYPE, BLOB_TYPE_V2, BLOB_TYPE_V1);
      return FALSE;
    }

  alive = build_alive_set (error);
  if (alive == NULL)
    {
      g_variant_unref (tree);
      g_variant_unref (top);
      return FALSE;
    }

  /* Suppress the saves that building terminals would otherwise queue; one save is
   * scheduled at the end instead, once the tab is whole. Nothing can run between
   * here and the reset: the build is synchronous within this callback.
   *
   * The previous value is preserved rather than forced back to FALSE, so a caller
   * that is itself mid-restore (startup, rebuilding several tabs in a loop) stays
   * suppressed until it is done. */
  was_restoring = g_restoring;
  g_restoring = TRUE;

  page = EPIMONE_PAGE (epimone_page_new_empty ());
  root = build_node (page, tree, alive);

  g_hash_table_destroy (alive);
  g_variant_unref (tree);

  if (root == NULL)
    {
      /* Every leaf was pruned: all of this tab's shells are gone. Deliberately no
       * empty tab; there is nothing to show, and an empty tab would be a worse
       * outcome than a clear refusal. */
      g_restoring = FALSE;
      g_object_ref_sink (page);
      g_object_unref (page);
      g_variant_unref (top);
      g_set_error (error, RESTORE_ERROR, 1,
                   "group %" G_GUINT64_FORMAT ": none of its sessions are still "
                   "alive", group_id);
      return FALSE;
    }

  epimone_page_set_tree_root (page, root);
  epimone_page_set_initial_title (page, title);
  /* A custom name outranks the seeded shell title AND whatever the panes go on
   * to emit; that precedence lives in the page. NULL (older blob, or none
   * set) leaves the tab on automatic titles. */
  epimone_page_set_custom_title (page, custom);

  /* Keep the same group. The panes are already members of it in the daemon
   * (that membership is why the group outlived the tab), so they are marked
   * enrolled rather than re-added, and the next sync is a plain GROUP_SET on
   * this id instead of creating a duplicate group. */
  epimone_page_set_group_id (page, group_id);
  ids = g_array_new (FALSE, FALSE, sizeof (guint64));
  collect_session_ids (root, ids);
  for (guint i = 0; i < ids->len; i++)
    epimone_page_mark_enrolled (page, g_array_index (ids, guint64, i));

  epimone_window_adopt_and_select_page (win, page);

  /* Ratios need real allocations; the focused pane is set from the blob. Scoped
   * to this page so tabs the user has since dragged are left alone. */
  rt = g_new0 (RestoreTick, 1);
  rt->page = g_object_ref (page);
  rt->focus_session = focused;
  rt->frames_left = RESTORE_TICK_FRAMES;
  gtk_widget_add_tick_callback (GTK_WIDGET (page), restore_tick_cb, rt,
                               restore_tick_free);

  g_variant_unref (top);
  g_restoring = was_restoring;

  g_message ("epimone: restored group %" G_GUINT64_FORMAT " (%u pane(s), "
             "focus session %" G_GUINT64_FORMAT ")",
             group_id, ids->len, focused);

  /* Now that the tab is whole, let the ordinary sync write it back. A caller that
   * is still mid-restore schedules its own save once every tab is up. */
  if (!was_restoring)
    epimone_layout_schedule_save ();
  return TRUE;
}

gboolean
epimone_layout_rename_group (guint64 group_id, const char *name, GError **error)
{
  g_autoptr (GPtrArray) groups = NULL;
  EpiGroupInfo *found = NULL;
  EpimonePage *page;
  GVariant *top;
  GVariant *tree = NULL;
  GVariant *rewritten;
  guint64 focused = 0;
  const char *title = NULL;
  guint cols = 0, rows = 0;
  g_autofree char *text = NULL;

  if (name != NULL && name[0] == '\0')
    name = NULL;

  if (group_id == 0)
    {
      g_set_error (error, RESTORE_ERROR, 1, "group id 0 is not a group");
      return FALSE;
    }
  if (!instance_ok (error))
    return FALSE;

  groups = epimone_client_list_groups (NULL, error);
  if (groups == NULL)
    return FALSE;
  for (guint i = 0; i < groups->len; i++)
    {
      EpiGroupInfo *g = g_ptr_array_index (groups, i);

      if (g->id == group_id)
        {
          found = g;
          break;
        }
    }
  if (found == NULL)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "no group %" G_GUINT64_FORMAT, group_id);
      return FALSE;
    }

  /* Attached: route through the live page. It retitles its tab and window
   * immediately, and the ordinary debounced sync writes the blob. */
  page = find_page_for_members (found->members, NULL);
  if (page != NULL)
    {
      epimone_page_set_custom_title (page, name);
      return TRUE;
    }

  /* Detached: no widgets to route through, so rewrite the stored blob in
   * place, changing only the custom-name field. Always written as the current
   * version; renaming an older blob upgrades it. */
  top = blob_parse (found->blob, found->blob_len, &focused, &title, NULL,
                    &tree, &cols, &rows);
  if (top == NULL)
    {
      g_set_error (error, RESTORE_ERROR, 1,
                   "group %" G_GUINT64_FORMAT " layout does not parse; "
                   "cannot rename it", group_id);
      return FALSE;
    }

  rewritten = g_variant_new ("(uts@vqqs)", (guint32) BLOB_VERSION, focused,
                             title ?: "", tree, (guint16) cols, (guint16) rows,
                             name ?: "");
  g_variant_ref_sink (rewritten);
  text = g_variant_print (rewritten, TRUE);
  g_variant_unref (rewritten);
  /* "@v" ref_sinks the (non-floating) tree, so the parse reference still has
   * to be dropped; title is borrowed from top and already copied into text. */
  g_variant_unref (tree);
  g_variant_unref (top);

  return epimone_client_group_set (group_id, (const guint8 *) text,
                                   strlen (text), error);
}

/* Parse a blob, accepting version 3 and the versions 2 and 1 that predate the
 * custom name and the stored grid size. Returns the owned top variant (caller
 * unrefs) or NULL; *out_tree is owned too, and the strings are borrowed from
 * the top variant. Dispatching by which type parses is the same trick the
 * layout file uses, and for the same reason: g_variant_parse needs the type up
 * front. */
static GVariant *
blob_parse (const guint8 *blob, gsize len,
            guint64 *out_focused, const char **out_title,
            const char **out_custom, GVariant **out_tree,
            guint *out_cols, guint *out_rows)
{
  GVariant *top;
  guint32 version = 0;
  guint16 cols = 0, rows = 0;
  const char *custom = NULL;

  if (out_focused) *out_focused = 0;
  if (out_title)   *out_title = NULL;
  if (out_custom)  *out_custom = NULL;
  if (out_tree)    *out_tree = NULL;
  if (out_cols)    *out_cols = 0;
  if (out_rows)    *out_rows = 0;

  if (blob == NULL || len == 0)
    return NULL;

  top = g_variant_parse (G_VARIANT_TYPE (BLOB_TYPE), (const char *) blob,
                         (const char *) blob + len, NULL, NULL);
  if (top != NULL)
    {
      g_variant_get (top, "(ut&s@vqq&s)", &version, out_focused, out_title,
                     out_tree, &cols, &rows, &custom);
      if (version != 3)
        goto bad;
      if (out_cols) *out_cols = cols;
      if (out_rows) *out_rows = rows;
      /* "" means no custom name; hand out NULL so callers get one spelling. */
      if (out_custom && custom != NULL && custom[0] != '\0')
        *out_custom = custom;
      return top;
    }

  top = g_variant_parse (G_VARIANT_TYPE (BLOB_TYPE_V2), (const char *) blob,
                         (const char *) blob + len, NULL, NULL);
  if (top != NULL)
    {
      g_variant_get (top, "(ut&s@vqq)", &version, out_focused, out_title,
                     out_tree, &cols, &rows);
      if (version != 2)
        goto bad;
      if (out_cols) *out_cols = cols;
      if (out_rows) *out_rows = rows;
      return top;
    }

  top = g_variant_parse (G_VARIANT_TYPE (BLOB_TYPE_V1), (const char *) blob,
                         (const char *) blob + len, NULL, NULL);
  if (top != NULL)
    {
      g_variant_get (top, "(ut&s@v)", &version, out_focused, out_title, out_tree);
      if (version != 1)
        goto bad;
      /* Grid unknown: the caller falls back. */
      return top;
    }
  return NULL;

bad:
  if (out_tree && *out_tree) { g_variant_unref (*out_tree); *out_tree = NULL; }
  g_variant_unref (top);
  return NULL;
}

gboolean
epimone_layout_blob_peek (const guint8 *blob, gsize len, char **out_title,
                          guint64 *out_focused, guint *out_cols, guint *out_rows)
{
  const char *title = NULL;
  GVariant *tree = NULL;
  GVariant *top;

  if (out_title)
    *out_title = NULL;

  top = blob_parse (blob, len, out_focused, &title, NULL, &tree,
                    out_cols, out_rows);
  if (top == NULL)
    return FALSE;

  if (out_title != NULL && title != NULL && title[0] != '\0')
    *out_title = g_strdup (title);

  if (tree != NULL)
    g_variant_unref (tree);
  g_variant_unref (top);
  return TRUE;
}

char *
epimone_layout_blob_dup_custom_title (const guint8 *blob, gsize len)
{
  const char *custom = NULL;
  GVariant *tree = NULL;
  GVariant *top;
  char *out = NULL;

  top = blob_parse (blob, len, NULL, NULL, &custom, &tree, NULL, NULL);
  if (top == NULL)
    return NULL;

  out = g_strdup (custom);   /* NULL when the blob predates it or it is unset */

  if (tree != NULL)
    g_variant_unref (tree);
  g_variant_unref (top);
  return out;
}

/* ------------------------------------------------------------------ *
 * blob geometry, for drawing a tab's arrangement
 * ------------------------------------------------------------------ */

/* Walk one subtree, emitting a leaf per pane and a seam per split, in the
 * fractional rect (@x,@y,@w,@h).
 *
 * A split whose child collapses to nothing (every leaf under it pruned) hands
 * its whole rect to the surviving sibling and contributes NO seam, the same
 * collapse build_node performs, so the picture matches what restoring produces.
 * Returns TRUE if anything was emitted for this subtree. */
static gboolean
walk_node_geometry (GVariant  *node_v,
                    double     x,
                    double     y,
                    double     w,
                    double     h,
                    GPtrArray *leaves,
                    GPtrArray *seams)
{
  GVariant *inner = g_variant_get_variant (node_v);
  const GVariantType *t = g_variant_get_type (inner);
  gboolean emitted = FALSE;

  if (g_variant_type_equal (t, G_VARIANT_TYPE ("(st)")))
    {
      const char *tag;
      guint64 id;

      g_variant_get (inner, "(&st)", &tag, &id);
      if (id != 0)
        {
          EpimoneLayoutLeaf *leaf = g_new0 (EpimoneLayoutLeaf, 1);

          leaf->session = id;
          leaf->x = x; leaf->y = y; leaf->w = w; leaf->h = h;
          g_ptr_array_add (leaves, leaf);
          emitted = TRUE;
        }
    }
  else if (g_variant_type_equal (t, G_VARIANT_TYPE ("(ssdvv)")))
    {
      const char *tag, *orient;
      double ratio;
      GVariant *startv = NULL, *endv = NULL;
      gboolean horizontal;
      double a_x = x, a_y = y, a_w = w, a_h = h;
      double b_x = x, b_y = y, b_w = w, b_h = h;
      guint leaves_before = leaves->len;
      guint seams_before = seams != NULL ? seams->len : 0;
      gboolean got_start, got_end;

      g_variant_get (inner, "(&s&sd@v@v)", &tag, &orient, &ratio, &startv, &endv);
      if (ratio <= 0.0 || ratio >= 1.0 || !isfinite (ratio))
        ratio = 0.5;
      horizontal = (orient[0] == 'h');

      if (horizontal)
        {
          a_w = w * ratio;
          b_x = x + a_w;
          b_w = w - a_w;
        }
      else
        {
          a_h = h * ratio;
          b_y = y + a_h;
          b_h = h - a_h;
        }

      got_start = walk_node_geometry (startv, a_x, a_y, a_w, a_h, leaves, seams);
      got_end = walk_node_geometry (endv, b_x, b_y, b_w, b_h, leaves, seams);

      if (got_start != got_end)
        {
          /* One side collapsed: redo the survivor over the full rect so the
           * pruned pane leaves no gap. */
          g_ptr_array_set_size (leaves, leaves_before);
          if (seams != NULL)
            g_ptr_array_set_size (seams, seams_before);
          walk_node_geometry (got_start ? startv : endv, x, y, w, h, leaves, seams);
          emitted = TRUE;
        }
      else if (got_start && got_end)
        {
          if (seams != NULL)
            {
              EpimoneLayoutSeam *seam = g_new0 (EpimoneLayoutSeam, 1);

              seam->vertical = horizontal;
              if (horizontal)
                {
                  seam->x = x + w * ratio; seam->y = y;
                  seam->w = 0.0;           seam->h = h;
                }
              else
                {
                  seam->x = x;             seam->y = y + h * ratio;
                  seam->w = w;             seam->h = 0.0;
                }
              g_ptr_array_add (seams, seam);
            }
          emitted = TRUE;
        }

      g_variant_unref (startv);
      g_variant_unref (endv);
    }

  g_variant_unref (inner);
  return emitted;
}

gboolean
epimone_layout_blob_geometry (const guint8 *blob, gsize len,
                              GPtrArray **out_leaves, GPtrArray **out_seams)
{
  GVariant *tree = NULL;
  GVariant *top;
  guint64 focused = 0;
  guint fcols = 0, frows = 0;
  /* Not NULL: blob_parse hands this straight to g_variant_get's "&s" slot, and a
   * NULL there left the trailing cols/rows unset, which silently cost every pane
   * its derived grid. */
  const char *title = NULL;
  GPtrArray *leaves = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *seams = g_ptr_array_new_with_free_func (g_free);

  top = blob_parse (blob, len, &focused, &title, NULL, &tree, &fcols, &frows);
  if (top == NULL || tree == NULL)
    {
      if (top != NULL)
        g_variant_unref (top);
      g_ptr_array_unref (leaves);
      g_ptr_array_unref (seams);
      return FALSE;
    }

  walk_node_geometry (tree, 0.0, 0.0, 1.0, 1.0, leaves, seams);

  /* Recover the tab's total grid from the one pane whose grid the blob records,
   * then give every pane the grid its own fraction implies. Without a recorded
   * grid (a version 1 blob) the panes keep 0x0 and the caller falls back. */
  if (fcols > 0 && frows > 0)
    {
      double total_cols = 0, total_rows = 0;

      for (guint i = 0; i < leaves->len; i++)
        {
          EpimoneLayoutLeaf *leaf = g_ptr_array_index (leaves, i);

          if (leaf->session == focused && leaf->w > 0 && leaf->h > 0)
            {
              total_cols = fcols / leaf->w;
              total_rows = frows / leaf->h;
              break;
            }
        }
      /* No focused pane in the tree (or it was pruned): treat the recorded grid
       * as the whole tab's, which is exactly right for a single-pane tab. */
      if (total_cols <= 0 || total_rows <= 0)
        {
          total_cols = fcols;
          total_rows = frows;
        }

      for (guint i = 0; i < leaves->len; i++)
        {
          EpimoneLayoutLeaf *leaf = g_ptr_array_index (leaves, i);

          leaf->cols = (guint) MAX (1.0, floor (total_cols * leaf->w + 0.5));
          leaf->rows = (guint) MAX (1.0, floor (total_rows * leaf->h + 0.5));
        }
    }

  g_variant_unref (tree);
  g_variant_unref (top);

  if (out_leaves != NULL)
    *out_leaves = leaves;
  else
    g_ptr_array_unref (leaves);
  if (out_seams != NULL)
    *out_seams = seams;
  else
    g_ptr_array_unref (seams);
  return TRUE;
}

/* ------------------------------------------------------------------ *
 * startup restore
 * ------------------------------------------------------------------ */

/* Rebuild windows from a version 2 file: geometry from the file, arrangement from
 * the daemon's groups. */
static gboolean
restore_from_v2 (AdwApplication *app, GVariant *top)
{
  GVariant *windows = NULL;
  GVariantIter wit;
  GVariant *winv;
  guint32 version;
  int created = 0;
  gboolean was_restoring = g_restoring;

  g_variant_get (top, "(u@a(iibiat))", &version, &windows);

  g_restoring = TRUE;

  g_variant_iter_init (&wit, windows);
  while ((winv = g_variant_iter_next_value (&wit)) != NULL)
    {
      int width = 0, height = 0, active = 0;
      gboolean maximized = FALSE;
      GVariant *gids = NULL;
      GtkWidget *win;
      gsize n, i;
      int restored = 0;

      g_variant_get (winv, "(iibi@at)", &width, &height, &maximized, &active,
                     &gids);

      win = epimone_window_new (app);
      if (width > 0 && height > 0)
        gtk_window_set_default_size (GTK_WINDOW (win), width, height);

      n = g_variant_n_children (gids);
      for (i = 0; i < n; i++)
        {
          guint64 gid = 0;
          GError *err = NULL;

          g_variant_get_child (gids, i, "t", &gid);
          if (epimone_layout_restore_group (EPIMONE_WINDOW (win), gid, &err))
            restored++;
          else
            {
              /* A group that has gone (all its sessions killed while the GUI
               * was not running) or that a guard refused. Skip the tab, keep
               * the window. */
              g_message ("epimone: startup: skipping group %" G_GUINT64_FORMAT
                         ": %s", gid,
                         err != NULL ? err->message : "unknown error");
              g_clear_error (&err);
            }
        }
      g_variant_unref (gids);

      if (restored == 0)
        {
          /* Every tab this window had is gone. Showing an empty window would be
           * worse than not showing one; the caller opens a fresh one if nothing
           * at all came back. */
          gtk_window_destroy (GTK_WINDOW (win));
        }
      else
        {
          epimone_window_set_active_tab (EPIMONE_WINDOW (win),
                                         (active < restored) ? active : 0);
          if (maximized)
            gtk_window_maximize (GTK_WINDOW (win));
          gtk_window_present (GTK_WINDOW (win));
          created++;
        }

      g_variant_unref (winv);
    }

  g_variant_unref (windows);
  g_restoring = was_restoring;

  if (created > 0)
    epimone_layout_schedule_save ();
  return created > 0;
}

gboolean
epimone_layout_restore (AdwApplication *app)
{
  g_autofree char *path = layout_path ();
  g_autofree char *text = NULL;
  gsize len = 0;
  GVariant *top;

  if (!g_file_get_contents (path, &text, &len, NULL))
    return FALSE;

  /* Try the current format, then the old one. The version is inside the file, but
   * g_variant_parse needs the type up front, so dispatching by which type parses
   * is both simpler and stricter than reading the number and trusting it. */
  top = g_variant_parse (G_VARIANT_TYPE (LAYOUT_TYPE_V2), text, text + len,
                         NULL, NULL);
  if (top != NULL)
    {
      gboolean ok = restore_from_v2 (app, top);
      g_variant_unref (top);
      return ok;
    }

  top = g_variant_parse (G_VARIANT_TYPE (LAYOUT_TYPE_V1), text, text + len,
                         NULL, NULL);
  if (top != NULL)
    {
      g_message ("epimone: migrating a version 1 layout file; the next save "
                 "writes version 2");
      return restore_from_v1 (app, top);   /* takes ownership of top */
    }

  g_warning ("epimone: layout file parses as neither version 2 nor version 1");
  return FALSE;
}
