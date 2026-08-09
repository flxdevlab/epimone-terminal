#include "epimone-page.h"
#include "epimone-client.h"
#include "epimone-layout.h"
#include "epimone-shortcuts.h"

#include <vte/vte.h>
#include <gdk/gdkkeysyms.h>
#include <glib-unix.h>

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <unistd.h>

#define EPI_BRIDGE_KEY "epi-bridge"
#define EPI_PUMP_CHUNK 65536

/* The split tree stores "leaves": each a GtkOverlay holding a
 * GtkScrolledWindow around one VteTerminal (so every pane has a hover-reveal
 * overlay scrollbar; see
 * epimone_page_wrap_terminal). `focused` still tracks the VteTerminal; tree
 * surgery works on the enclosing leaf. Use epimone_leaf_terminal()/
 * epimone_terminal_leaf() to convert between them. */
struct _EpimonePage
{
  AdwBin       parent_instance;
  VteTerminal *focused;
  char        *title;
  char        *subtitle;      /* focused pane's cwd, $HOME shown as ~ */
  char        *custom_title;  /* user-chosen name; NULL = follow the shell */
  GtkWidget   *popover;
  GtkWidget   *zoomed_leaf;   /* the zoomed leaf, NULL = not zoomed */

  guint64      group_id;      /* daemon group backing this tab, 0 = none yet */
  GHashTable  *enrolled;      /* session ids already added to that group */

  /* Divider-drag winsize coalescing (see epimone_page_maybe_resize). ptr_down
   * tracks the primary mouse button within this page via a passive capture-phase
   * controller; divider_dragging is set when a paned position changes while it
   * is down. Together they mean "a handle is being dragged", during which the
   * per-motion PTY winsize push is held and flushed once at release. */
  gboolean     ptr_down;
  gboolean     divider_dragging;
};

/* Per-terminal bridge: connects the daemon data-channel socket to a local PTY
 * whose master is handed to VTE. VTE gets a genuine PTY (correct termios /
 * winsize semantics) while the daemon stays the sole reader of the real PTY,
 * which is what keeps the ring buffer complete for replay. */
typedef struct {
  EpimonePage *page;         /* borrowed */
  VteTerminal *term;         /* borrowed */
  guint64      session_id;

  int          sock_fd;      /* daemon data channel (non-blocking), -1 if none */
  int          pty_slave;    /* bridge's end of the local PTY (raw), -1 if none */

  GByteArray  *to_sock;      /* pending slave -> socket bytes */
  GByteArray  *to_slave;     /* pending socket -> slave bytes */

  guint        sock_in_id;
  guint        sock_out_id;
  guint        slave_in_id;
  guint        slave_out_id;
  guint        death_id;     /* pending death idle */

  glong        last_rows;
  glong        last_cols;
  guint        resize_id;      /* debounced winsize-push timeout; see maybe_resize */
  gboolean     resize_pending;  /* grid changed during a divider drag; flush at drag-end */
  gboolean     dead;

  /* Replay-bell suppression: see epimone_bridge_bell_window(). */
  gint64       attached_at;    /* g_get_monotonic_time() when the bridge opened */
  guint        bell_quiet_id;  /* pending "replay has gone quiet" timeout */
} EpiBridge;

enum {
  PROP_0,
  PROP_TITLE,
  PROP_SUBTITLE,
  N_PROPS
};

enum {
  SIGNAL_CLOSE_PAGE,
  N_SIGNALS
};

static GParamSpec *properties[N_PROPS];
static guint signals[N_SIGNALS];

G_DEFINE_FINAL_TYPE (EpimonePage, epimone_page, ADW_TYPE_BIN)

static void epimone_page_remove_terminal (EpimonePage *self, GtkWidget *term);
static void epimone_page_unzoom (EpimonePage *self);
static void epimone_page_update_dimming (EpimonePage *self);
static void epimone_pages_invalidate_scrims (void);

/* ------------------------------------------------------------------ *
 * context menu model
 * ------------------------------------------------------------------ */

static void
epimone_page_menu_append (GMenu      *section,
                          const char *label,
                          const char *action,
                          const char *accel)
{
  GMenuItem *item = g_menu_item_new (label, action);

  if (accel != NULL)
    g_menu_item_set_attribute (item, "accel", "s", accel);

  g_menu_append_item (section, item);
  g_object_unref (item);
}

/* The "Zoom Pane" item's label flips to "Unzoom Pane" while a pane is zoomed
 * (epimone_page_set_zoom_label). A GMenu item's label is immutable once added,
 * so the toggle removes and re-inserts the item, which needs the owning
 * section and the item's index within it, captured here at construction. */
#define EPIMONE_ZOOM_ACCEL "<Control><Shift>z"
static GMenu *epimone_zoom_section;   /* borrowed; the model is a permanent static */
static int    epimone_zoom_index = -1;

static GMenuModel *
epimone_page_get_menu_model (void)
{
  static GMenuModel *model;

  if (model == NULL)
    {
      GMenu *menu = g_menu_new ();
      GMenu *section;

      section = g_menu_new ();
      epimone_page_menu_append (section, "_Copy", "win.copy", "<Control><Shift>c");
      epimone_page_menu_append (section, "_Paste", "win.paste", "<Control><Shift>v");
      epimone_page_menu_append (section, "Select _All", "win.select-all", "<Control><Shift>a");
      epimone_page_menu_append (section, "Select _None", "win.select-none", NULL);
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      section = g_menu_new ();
      epimone_page_menu_append (section, "Split _Right", "win.split-right", "<Control><Shift>d");
      epimone_page_menu_append (section, "Split _Down", "win.split-down", "<Control><Shift>e");
      epimone_zoom_section = section;
      epimone_zoom_index = g_menu_model_get_n_items (G_MENU_MODEL (section));
      epimone_page_menu_append (section, "_Zoom Pane", "win.zoom", EPIMONE_ZOOM_ACCEL);
      /* Disabled, not hidden, on a single-pane tab: the enabled state of
       * win.open-in-new-tab is refreshed just before this menu pops
       * (epimone_page_popup_menu) and restored when it closes. */
      epimone_page_menu_append (section, "_Open in New Tab", "win.open-in-new-tab", "<Control><Shift>b");
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      /* Read-Only sits alone in its own section: it is a mode latch, not a
       * command, and the check mark the stateful action gives it reads
       * better isolated. The action's state is synced from the FOCUSED pane
       * just before the menu pops (epimone_page_popup_menu). */
      section = g_menu_new ();
      epimone_page_menu_append (section, "Read-Only", "win.read-only", NULL);
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      /* Recovery for a pane whose state a binary wrecked. Reset and Clear
       * empties only the VTE's local scrollback; the daemon's ring keeps the
       * history, and a detach/restore replays it. */
      section = g_menu_new ();
      epimone_page_menu_append (section, "Reset", "win.reset", NULL);
      epimone_page_menu_append (section, "Reset and Clear", "win.reset-and-clear", NULL);
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      section = g_menu_new ();
      epimone_page_menu_append (section, "New _Tab", "win.new-tab", "<Control><Shift>t");
      epimone_page_menu_append (section, "New _Window", "win.new-window", "<Control><Shift>n");
      /* No ellipsis, per the app-wide convention (no ellipsis on menu items,
       * anywhere). Drives the SAME custom-title mechanism as the overview
       * card menu's Set Title (epimone_page_set_custom_title + the group
       * blob). */
      epimone_page_menu_append (section, "Set Title", "win.set-title", NULL);
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      section = g_menu_new ();
      /* DETACH, not "Close": the vocabulary these two items share.
       *
       * Closing in Epimone never destroys anything: it drops the widget and the
       * session keeps running in the daemon, ready to come back from the
       * overview. But "close" implies death in every other terminal, which made
       * "Close Pane" and "Kill Pane" read as synonyms when they are opposites:
       * the one word that loses work and the one that cannot. Detach/kill is
       * the standard multiplexer distinction, and the labels follow it. Only
       * the LABELS say detach: the action names (win.close-pane,
       * win.tab-close…) and the shortcuts schema keys keep their original
       * spelling, because renaming those would invalidate every stored
       * binding and needs a migration. */
      epimone_page_menu_append (section, "Detach _Pane", "win.close-pane", "<Control><Shift>w");
      /* Detach leaves the program running; Kill is the only thing in the UI that
       * ends it. The accelerator still lands in the confirmation dialog;
       * win.kill-pane always asks before killing. */
      epimone_page_menu_append (section, "_Kill Pane", "win.kill-pane", "<Control><Shift>x");
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      /* Its own section, so it sits below a separator: everything above acts on
       * this pane, window or tab, while Preferences is app-level. Same
       * win.preferences action as the hamburger menu and Ctrl+, . */
      section = g_menu_new ();
      epimone_page_menu_append (section, "_Preferences", "win.preferences", "<Control>comma");
      g_menu_append_section (menu, NULL, G_MENU_MODEL (section));
      g_object_unref (section);

      model = G_MENU_MODEL (menu);
    }

  return model;
}

/* Flip the context menu's zoom item between "Zoom Pane" and "Unzoom Pane".
 * Refreshed just before the menu pops (epimone_page_popup_menu) so it reflects
 * the current zoom state. GMenu item labels are immutable, so the item is
 * removed and re-inserted at the same index; the model-backed popover picks up
 * the items-changed. The action (win.zoom) is unchanged; only the label. */
static void
epimone_page_set_zoom_label (gboolean zoomed)
{
  GMenuItem *item;

  if (epimone_zoom_section == NULL || epimone_zoom_index < 0)
    return;

  item = g_menu_item_new (zoomed ? "_Unzoom Pane" : "_Zoom Pane", "win.zoom");
  g_menu_item_set_attribute (item, "accel", "s", EPIMONE_ZOOM_ACCEL);
  g_menu_remove (epimone_zoom_section, epimone_zoom_index);
  g_menu_insert_item (epimone_zoom_section, epimone_zoom_index, item);
  g_object_unref (item);
}

/* ------------------------------------------------------------------ *
 * bridge helpers
 * ------------------------------------------------------------------ */

static EpiBridge *
bridge_of (GtkWidget *term)
{
  if (term == NULL || !VTE_IS_TERMINAL (term))
    return NULL;
  return g_object_get_data (G_OBJECT (term), EPI_BRIDGE_KEY);
}

/* Resolve a split-tree leaf (a GtkOverlay wrapping a GtkScrolledWindow, see
 * epimone_page_wrap_terminal) to the VteTerminal it wraps. Tolerates a bare
 * scrolled window or VteTerminal. Returns NULL for anything else. */
static VteTerminal *
epimone_leaf_terminal (GtkWidget *leaf)
{
  if (leaf == NULL)
    return NULL;
  if (VTE_IS_TERMINAL (leaf))
    return VTE_TERMINAL (leaf);
  if (GTK_IS_OVERLAY (leaf))
    leaf = gtk_overlay_get_child (GTK_OVERLAY (leaf));
  if (leaf != NULL && GTK_IS_SCROLLED_WINDOW (leaf))
    {
      GtkWidget *child = gtk_scrolled_window_get_child (GTK_SCROLLED_WINDOW (leaf));
      if (child != NULL && VTE_IS_TERMINAL (child))
        return VTE_TERMINAL (child);
    }
  return NULL;
}

/* The leaf that owns a terminal, i.e. the widget that lives
 * in the split tree. */
static GtkWidget *
epimone_terminal_leaf (VteTerminal *terminal)
{
  GtkWidget *parent = gtk_widget_get_parent (GTK_WIDGET (terminal));
  if (parent != NULL && GTK_IS_SCROLLED_WINDOW (parent))
    {
      GtkWidget *grandparent = gtk_widget_get_parent (parent);

      if (grandparent != NULL && GTK_IS_OVERLAY (grandparent))
        return grandparent;
      return parent;   /* defensive: bare scrolled window */
    }
  return GTK_WIDGET (terminal);   /* defensive: unwrapped */
}

/* The page a terminal lives in RIGHT NOW, from the widget tree.
 *
 * A per-terminal handler must not trust its connect-time user_data for this;
 * that silently assumes a pane never changes parent. "Open in New Tab"
 * breaks that assumption (it moves a live terminal into a different page),
 * so anything event-driven resolves the page at event time instead. The
 * connect-time page is kept as a fallback for the construction window, before
 * the terminal has been rooted in a tree (no event that matters can actually
 * fire then, but a NULL page must never come out of here). */
static EpimonePage *
epimone_page_for_terminal (GtkWidget *terminal, gpointer connect_time_page)
{
  GtkWidget *page = gtk_widget_get_ancestor (terminal, EPIMONE_TYPE_PAGE);

  return page != NULL ? EPIMONE_PAGE (page) : EPIMONE_PAGE (connect_time_page);
}

guint64
epimone_terminal_session_id (GtkWidget *widget)
{
  VteTerminal *t = epimone_leaf_terminal (widget);
  EpiBridge *b = t ? bridge_of (GTK_WIDGET (t)) : NULL;
  return b ? b->session_id : 0;
}

/* Write as much of buf as possible; drop what was written. Returns -1 on a
 * hard error, 0 otherwise (buf->len tells whether data remains). */
static int
flush_buf (int fd, GByteArray *buf)
{
  while (buf->len > 0)
    {
      ssize_t n = write (fd, buf->data, buf->len);
      if (n > 0)
        {
          g_byte_array_remove_range (buf, 0, (guint) n);
          continue;
        }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
      if (n < 0 && errno == EINTR)
        continue;
      return -1;
    }
  return 0;
}

static void schedule_death (EpiBridge *b);

static gboolean
on_sock_writable (int fd, GIOCondition cond, gpointer data)
{
  EpiBridge *b = data;

  (void) fd;
  if (cond & (G_IO_ERR | G_IO_HUP) || flush_buf (b->sock_fd, b->to_sock) < 0)
    {
      b->sock_out_id = 0;
      schedule_death (b);
      return G_SOURCE_REMOVE;
    }
  if (b->to_sock->len == 0)
    {
      b->sock_out_id = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_slave_writable (int fd, GIOCondition cond, gpointer data)
{
  EpiBridge *b = data;

  (void) fd;
  (void) cond;
  if (flush_buf (b->pty_slave, b->to_slave) < 0)
    {
      b->slave_out_id = 0;
      return G_SOURCE_REMOVE;
    }
  if (b->to_slave->len == 0)
    {
      b->slave_out_id = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

/* Queue bytes VTE produced (keyboard/paste/replies) toward the daemon. */
static void
forward_to_sock (EpiBridge *b, const guint8 *data, gsize len)
{
  if (b->sock_fd < 0)
    return;
  g_byte_array_append (b->to_sock, data, len);
  if (flush_buf (b->sock_fd, b->to_sock) < 0)
    {
      schedule_death (b);
      return;
    }
  if (b->to_sock->len > 0 && b->sock_out_id == 0)
    b->sock_out_id = g_unix_fd_add (b->sock_fd, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                    on_sock_writable, b);
}

/* ------------------------------------------------------------------ *
 * Replay-bell suppression
 *
 * Attaching to a session replays the daemon's ring buffer -- the scrollback
 * that already existed -- before any live output. If that history contains a
 * BEL (and it usually does: a tab-completion beep from an hour ago is enough),
 * VTE re-processes it on attach and rings. Because Epimone restores its
 * sessions on launch, that lands as a single bell every time the app starts.
 *
 * Note the startup BELs from the shell integration are NOT the problem: those
 * are OSC string terminators (`ESC]7;...BEL`), which VTE consumes as part of
 * the sequence. Verified: a freshly created session rings zero times. It is
 * only replayed history that rings.
 *
 * So the audible bell is switched off for the replay burst and switched back on
 * once it is over. The replay arrives as one burst immediately after ATTACHED;
 * live output only appears later, when the shell actually produces something.
 * "Over" is therefore: the incoming stream has been quiet for EPI_BELL_QUIET_MS,
 * or EPI_BELL_MAX_MS has elapsed, whichever comes first. The cap matters for a
 * session that is attached while a program is still churning out output, where
 * the quiet moment might never arrive.
 *
 * This suppresses the SOUND only, for that window only. The audible-bell
 * setting is untouched; the terminal is restored to whatever it is currently
 * set to, so a bell rung after startup behaves normally and turning the setting
 * off still silences everything.
 * ------------------------------------------------------------------ */

#define EPI_BELL_QUIET_MS 200     /* stream idle for this long => replay done */
#define EPI_BELL_MAX_MS   3000    /* never suppress for longer than this */

/* Set on a VteTerminal while its replay burst is being fed. */
#define EPI_BELL_SUPPRESS_KEY "epi-bell-suppressed"

static gboolean
epimone_terminal_bell_suppressed (VteTerminal *t)
{
  return g_object_get_data (G_OBJECT (t), EPI_BELL_SUPPRESS_KEY) != NULL;
}

/* Apply the audible bell to one terminal, honouring both the user setting and
 * any in-progress replay suppression. */
static void epimone_terminal_apply_bell (VteTerminal *t);

static void
epimone_terminal_set_bell_suppressed (VteTerminal *t, gboolean suppressed)
{
  g_object_set_data (G_OBJECT (t), EPI_BELL_SUPPRESS_KEY,
                     suppressed ? GINT_TO_POINTER (1) : NULL);
  epimone_terminal_apply_bell (t);
}

/* Replay burst has gone quiet (or hit the cap): let bells through again. */
static gboolean
epimone_bridge_bell_quiet_cb (gpointer data)
{
  EpiBridge *b = data;

  b->bell_quiet_id = 0;
  if (b->term != NULL)
    epimone_terminal_set_bell_suppressed (b->term, FALSE);
  return G_SOURCE_REMOVE;
}

/* Called for every chunk that arrives from the daemon. While the replay window
 * is open, each chunk pushes the "quiet" deadline out; once the cap is reached
 * the window closes immediately, so a continuously noisy session cannot keep
 * the bell muted indefinitely. */
static void
epimone_bridge_bell_window (EpiBridge *b)
{
  if (b->term == NULL || !epimone_terminal_bell_suppressed (b->term))
    return;   /* window already closed: nothing to do on the hot path */

  if (g_get_monotonic_time () - b->attached_at >= EPI_BELL_MAX_MS * 1000)
    {
      g_clear_handle_id (&b->bell_quiet_id, g_source_remove);
      epimone_terminal_set_bell_suppressed (b->term, FALSE);
      return;
    }

  g_clear_handle_id (&b->bell_quiet_id, g_source_remove);
  b->bell_quiet_id = g_timeout_add (EPI_BELL_QUIET_MS,
                                    epimone_bridge_bell_quiet_cb, b);
}

/* Queue daemon output (ring replay + live) toward VTE's PTY. */
static void
forward_to_slave (EpiBridge *b, const guint8 *data, gsize len)
{
  if (b->pty_slave < 0)
    return;
  epimone_bridge_bell_window (b);
  g_byte_array_append (b->to_slave, data, len);
  if (flush_buf (b->pty_slave, b->to_slave) < 0)
    return;
  if (b->to_slave->len > 0 && b->slave_out_id == 0)
    b->slave_out_id = g_unix_fd_add (b->pty_slave, G_IO_OUT | G_IO_ERR | G_IO_HUP,
                                     on_slave_writable, b);
}

static gboolean
on_slave_readable (int fd, GIOCondition cond, gpointer data)
{
  EpiBridge *b = data;
  guint8 buf[EPI_PUMP_CHUNK];
  ssize_t n;

  (void) fd;
  if (cond & (G_IO_ERR | G_IO_HUP) && !(cond & G_IO_IN))
    {
      /* VTE closed the master (pane going away). */
      b->slave_in_id = 0;
      return G_SOURCE_REMOVE;
    }

  n = read (b->pty_slave, buf, sizeof buf);
  if (n > 0)
    forward_to_sock (b, buf, (gsize) n);
  else if (n == 0)
    {
      b->slave_in_id = 0;
      return G_SOURCE_REMOVE;
    }
  return G_SOURCE_CONTINUE;
}

static gboolean
on_sock_readable (int fd, GIOCondition cond, gpointer data)
{
  EpiBridge *b = data;
  guint8 buf[EPI_PUMP_CHUNK];
  ssize_t n;

  (void) fd;
  n = read (b->sock_fd, buf, sizeof buf);
  if (n > 0)
    {
      forward_to_slave (b, buf, (gsize) n);
      return G_SOURCE_CONTINUE;
    }
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
    return G_SOURCE_CONTINUE;
  if (n < 0 && errno == EINTR)
    return G_SOURCE_CONTINUE;

  /* EOF or error: the daemon closed the data channel (shell exited). */
  b->sock_in_id = 0;
  schedule_death (b);
  return G_SOURCE_REMOVE;
}

/* The data socket hit EOF: collapse the pane, and reap the session ONLY if the
 * shell really exited. EOF has a second cause (the daemon forcibly detaches
 * this client when another client attaches to the same session) and the two
 * are indistinguishable at the socket. Killing unconditionally here would
 * destroy live sessions whenever a second GUI instance restores this window's
 * groups: every stolen pane's EOF handler would kill the session its new
 * owner had just attached. So ask the daemon which case this is, and leave a
 * live session alone; the pane still collapses, which is ordinary detach
 * semantics. Deferred to an idle so the bridge is never freed from inside its
 * own fd callback. */
static gboolean
death_idle (gpointer data)
{
  EpiBridge *b = data;
  EpimonePage *page = b->page;
  GtkWidget *term = GTK_WIDGET (b->term);
  guint64 id = b->session_id;

  b->death_id = 0;

  if (id != 0)
    {
      gboolean alive = FALSE;
      GPtrArray *sessions = epimone_client_list_sessions (NULL);

      if (sessions != NULL)
        {
          for (guint i = 0; i < sessions->len; i++)
            {
              EpiSessionInfo *si = g_ptr_array_index (sessions, i);

              if (si->id == id)
                {
                  alive = si->alive;
                  break;
                }
            }
          g_ptr_array_unref (sessions);
        }

      if (!alive)
        epimone_client_kill_session (id, NULL);
    }

  epimone_page_remove_terminal (page, term);   /* destroys term -> frees bridge */
  return G_SOURCE_REMOVE;
}

static void
schedule_death (EpiBridge *b)
{
  if (b->dead)
    return;
  b->dead = TRUE;
  b->death_id = g_idle_add (death_idle, b);
}

static void
bridge_free (gpointer data)
{
  EpiBridge *b = data;

  if (b->death_id)
    g_source_remove (b->death_id);
  if (b->sock_in_id)
    g_source_remove (b->sock_in_id);
  if (b->sock_out_id)
    g_source_remove (b->sock_out_id);
  if (b->slave_in_id)
    g_source_remove (b->slave_in_id);
  if (b->slave_out_id)
    g_source_remove (b->slave_out_id);
  g_clear_handle_id (&b->resize_id, g_source_remove);

  if (b->sock_fd >= 0)
    close (b->sock_fd);          /* closing the data channel detaches the session */
  if (b->pty_slave >= 0)
    close (b->pty_slave);

  g_clear_handle_id (&b->bell_quiet_id, g_source_remove);

  if (b->to_sock)
    g_byte_array_unref (b->to_sock);
  if (b->to_slave)
    g_byte_array_unref (b->to_slave);

  g_free (b);
}

static int
set_nonblock_cloexec (int fd)
{
  int fl = fcntl (fd, F_GETFL, 0);
  if (fl < 0 || fcntl (fd, F_SETFL, fl | O_NONBLOCK) < 0)
    return -1;
  fl = fcntl (fd, F_GETFD, 0);
  if (fl < 0 || fcntl (fd, F_SETFD, fl | FD_CLOEXEC) < 0)
    return -1;
  return 0;
}

/* ------------------------------------------------------------------ *
 * title / focus / copy plumbing
 * ------------------------------------------------------------------ */

static void
epimone_page_update_title (EpimonePage *self)
{
  const char *base = NULL;
  char *title;

  /* A user-chosen name wins outright: while it is set the tab stops following
   * the shell's OSC 0/2 titles, or the next prompt would overwrite the rename. */
  if (self->custom_title != NULL)
    {
      base = self->custom_title;
    }
  else if (self->focused != NULL)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      base = vte_terminal_get_window_title (self->focused);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }

  if (base == NULL || base[0] == '\0')
    base = "Terminal";

  /* Prepend a marker while zoomed so panes never look like they vanished. */
  if (self->zoomed_leaf != NULL)
    title = g_strconcat ("🔍 ", base, NULL);
  else
    title = g_strdup (base);

  if (g_strcmp0 (self->title, title) == 0)
    {
      g_free (title);
      return;
    }

  g_free (self->title);
  self->title = title;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
  /* The blob carries the tab title, so a title change needs a rewrite. Titles
   * change often (every OSC 0/2 the shell emits), which is why this goes to the
   * group-only scheduler: the shared debounce coalesces the churn, and
   * layout.json (which does not store titles) is not rewritten for it. */
  epimone_layout_schedule_group_save ();
}

/* The focused pane's working directory, with $HOME collapsed to "~". Drives the
 * header bar's dimmed second line. Kept separate from the title because VTE
 * reports the two through different channels: the title arrives as an OSC 0/2
 * window-title escape, the directory as OSC 7 (current-directory-uri). Shells
 * usually emit both on a `cd`, but not always in the same sequence, so this is
 * refreshed from whichever arrives. */
static void
epimone_page_update_subtitle (EpimonePage *self)
{
  g_autofree char *cwd = epimone_page_dup_cwd (self);
  g_autofree char *home = NULL;
  char *subtitle;

  if (cwd == NULL)
    {
      if (self->subtitle == NULL)
        return;
      g_clear_pointer (&self->subtitle, g_free);
      g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
      return;
    }

  home = g_strdup (g_get_home_dir ());
  if (home != NULL && g_str_has_prefix (cwd, home))
    {
      const char *rest = cwd + strlen (home);

      /* Only collapse on a path boundary: with home "/home/user", a sibling
       * like "/home/username" is left alone. */
      if (*rest == '\0')
        subtitle = g_strdup ("~");
      else if (*rest == '/')
        subtitle = g_strconcat ("~", rest, NULL);
      else
        subtitle = g_strdup (cwd);
    }
  else
    {
      subtitle = g_strdup (cwd);
    }

  if (g_strcmp0 (self->subtitle, subtitle) == 0)
    {
      g_free (subtitle);
      return;
    }

  g_free (self->subtitle);
  self->subtitle = subtitle;
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_SUBTITLE]);
}

static void
epimone_page_set_focused (EpimonePage *self,
                          VteTerminal *terminal)
{
  gboolean changed = (self->focused != terminal);

  self->focused = terminal;
  epimone_page_update_title (self);
  epimone_page_update_subtitle (self);   /* focus moved: possibly another cwd */
  /* Single choke point for "which pane is active": every split, close, focus
   * move and pane adoption routes through here, so the dim scrims are kept in
   * step from one place. Runs even when focus did not change (the pane COUNT
   * may have, e.g. the first split's set_focused re-selects the same VTE). */
  epimone_page_update_dimming (self);

  if (changed)
    epimone_layout_schedule_save ();
}

static VteTerminal *
epimone_page_first_terminal (GtkWidget *widget)
{
  if (widget == NULL)
    return NULL;

  if (GTK_IS_PANED (widget))
    {
      VteTerminal *found = epimone_page_first_terminal (gtk_paned_get_start_child (GTK_PANED (widget)));
      if (found != NULL)
        return found;
      return epimone_page_first_terminal (gtk_paned_get_end_child (GTK_PANED (widget)));
    }

  return epimone_leaf_terminal (widget);   /* leaf -> its VTE */
}

/* Collect the VteTerminal of every leaf in the tree. */
static void
epimone_page_collect_terminals (GtkWidget *widget, GPtrArray *out)
{
  if (widget == NULL)
    return;
  if (GTK_IS_PANED (widget))
    {
      epimone_page_collect_terminals (gtk_paned_get_start_child (GTK_PANED (widget)), out);
      epimone_page_collect_terminals (gtk_paned_get_end_child (GTK_PANED (widget)), out);
    }
  else
    {
      VteTerminal *t = epimone_leaf_terminal (widget);
      if (t != NULL)
        g_ptr_array_add (out, t);
    }
}

/* How many terminal panes this page holds (1 for an unsplit page). Used by the
 * window's close confirmation, which must warn about a single tab split into
 * several panes just as much as about several tabs. */
guint
epimone_page_get_pane_count (EpimonePage *self)
{
  g_autoptr (GPtrArray) terminals = NULL;
  GtkWidget *root;

  g_return_val_if_fail (EPIMONE_IS_PAGE (self), 0);

  root = adw_bin_get_child (ADW_BIN (self));
  if (root == NULL)
    return 0;

  terminals = g_ptr_array_new ();
  epimone_page_collect_terminals (root, terminals);
  return terminals->len;
}

/* ------------------------------------------------------------------ *
 * pane zoom
 * ------------------------------------------------------------------ */

/* Recursively make every node in a subtree visible again (undo zoom hiding). */
static void
epimone_page_show_all (GtkWidget *w)
{
  if (w == NULL)
    return;
  gtk_widget_set_visible (w, TRUE);
  if (GTK_IS_PANED (w))
    {
      epimone_page_show_all (gtk_paned_get_start_child (GTK_PANED (w)));
      epimone_page_show_all (gtk_paned_get_end_child (GTK_PANED (w)));
    }
}

/* Zoom: walk from the focused leaf up to the root, hiding the sibling subtree
 * at every GtkPaned ancestor. Each paned then gives all space to the one
 * visible child, so the focused pane fills the tab. `leaf` is the focused
 * pane's leaf overlay (its parent is a GtkPaned). The tree is left intact. */
static void
epimone_page_apply_zoom (EpimonePage *self, GtkWidget *leaf)
{
  GtkWidget *w = leaf;
  GtkWidget *parent;
  gboolean hid_any = FALSE;

  if (leaf == NULL)
    return;
  parent = gtk_widget_get_parent (w);

  while (GTK_IS_PANED (parent))
    {
      GtkPaned *p = GTK_PANED (parent);
      GtkWidget *sibling = (gtk_paned_get_start_child (p) == w)
                             ? gtk_paned_get_end_child (p)
                             : gtk_paned_get_start_child (p);
      if (sibling != NULL)
        {
          gtk_widget_set_visible (sibling, FALSE);
          hid_any = TRUE;
        }
      w = parent;
      parent = gtk_widget_get_parent (w);
    }

  if (!hid_any)
    return;   /* single pane: nothing to zoom */

  self->zoomed_leaf = leaf;
  epimone_page_update_title (self);
}

static void
epimone_page_unzoom (EpimonePage *self)
{
  if (self->zoomed_leaf == NULL)
    return;
  epimone_page_show_all (adw_bin_get_child (ADW_BIN (self)));
  self->zoomed_leaf = NULL;
  epimone_page_update_title (self);
  /* Real split ratios are readable again, so capture them. */
  epimone_layout_schedule_group_save ();
}

gboolean
epimone_page_is_zoomed (EpimonePage *self)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), FALSE);
  return self->zoomed_leaf != NULL;
}

void
epimone_page_toggle_zoom (EpimonePage *self)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));

  if (self->zoomed_leaf != NULL)
    epimone_page_unzoom (self);
  else if (self->focused != NULL)
    epimone_page_apply_zoom (self, epimone_terminal_leaf (self->focused));
}

/* ------------------------------------------------------------------ *
 * resize forwarding
 * ------------------------------------------------------------------ */

/* Push the terminal's CURRENT grid to the daemon PTY, once. Runs off the
 * debounce timer set up by epimone_page_maybe_resize. */
static gboolean
epimone_page_resize_flush (gpointer data)
{
  EpiBridge *b = data;
  glong rows, cols;

  b->resize_id = 0;
  if (b->dead || b->session_id == 0)
    return G_SOURCE_REMOVE;

  rows = vte_terminal_get_row_count (b->term);
  cols = vte_terminal_get_column_count (b->term);
  if (rows <= 0 || cols <= 0)
    return G_SOURCE_REMOVE;
  if (rows == b->last_rows && cols == b->last_cols)
    return G_SOURCE_REMOVE;

  b->last_rows = rows;
  b->last_cols = cols;
  epimone_client_resize_session (b->session_id, (guint) rows, (guint) cols, NULL);
  return G_SOURCE_REMOVE;
}

/* Coalesce a burst of grid changes into ONE winsize push at the settled size.
 *
 * A divider drag re-allocates the pane on every motion; each distinct width
 * pushed makes the shell reprint its prompt on SIGWINCH, and because the shell
 * runs on the daemon's RAW PTY while this display VTE reflows on its own, those
 * reprints do not overwrite each other: they commit into the buffer as stacked
 * prompt fragments (measured: a drag to 2 cols left ~a dozen). The winsize is
 * correct on both sides at the settle; the garbage is retained history, not a
 * geometry error (a `clear` at the settled width renders cleanly). Coalescing to
 * a single push per drag removes the stack: the shell reprints once. The trailing
 * timer resets on every change, so nothing is sent until the size stops moving;
 * 150 ms is long enough to swallow a slow/paused human drag yet imperceptible
 * for a one-shot resize. (A residual single reprint can still land above the
 * prompt at prompt-wrapping widths; that is the deeper reflow-vs-reprint issue.) */
static void
epimone_page_maybe_resize (EpiBridge *b)
{
  if (b == NULL || b->dead || b->session_id == 0)
    return;

  /* While a divider is being interactively dragged, hold the PTY winsize push.
   * The widgets still resize visually (GtkPaned re-allocates regardless), but
   * pushing per motion makes the shell reprint its prompt on every intermediate
   * width, and those reprints stack in the display VTE. Mark the bridge dirty
   * and flush exactly once at drag-end (epimone_page_flush_pending_resizes). A
   * trailing debounce alone can't guarantee this (a paused drag outruns any
   * fixed window), so the drag itself is the coalescing boundary here. Non-drag
   * resizes (window, layout restore, zoom) leave divider_dragging clear and use
   * the debounce below. */
  if (b->page != NULL && b->page->divider_dragging)
    {
      b->resize_pending = TRUE;
      return;
    }

  g_clear_handle_id (&b->resize_id, g_source_remove);
  b->resize_id = g_timeout_add (150, epimone_page_resize_flush, b);
}

/* Drag-end: push once, now, for every bridge whose grid changed during the drag
 * that just ended. epimone_page_resize_flush dedups on last_rows/cols, so panes
 * the drag did not actually resize are no-ops; a drag on one divider only
 * settles the two panes it moved, siblings untouched. */
static void
epimone_page_flush_pending_resizes (EpimonePage *self)
{
  g_autoptr (GPtrArray) terminals = NULL;
  GtkWidget *root;

  root = adw_bin_get_child (ADW_BIN (self));
  if (root == NULL)
    return;

  terminals = g_ptr_array_new ();
  epimone_page_collect_terminals (root, terminals);
  for (guint i = 0; i < terminals->len; i++)
    {
      EpiBridge *b = bridge_of (g_ptr_array_index (terminals, i));
      if (b != NULL && b->resize_pending)
        {
          b->resize_pending = FALSE;
          g_clear_handle_id (&b->resize_id, g_source_remove);
          epimone_page_resize_flush (b);
        }
    }
}

/* Passive, observe-only tracker of the primary mouse button within this page.
 *
 * This is deliberately a GtkEventControllerLegacy in the CAPTURE phase that
 * always propagates: it never claims or consumes an event, so it cannot disturb
 * GtkPaned's own handle drag or terminal text selection. (A GtkGestureDrag on
 * the paned cannot do this job: GtkPaned claims the handle sequence, which
 * cancels a co-located gesture before it ever sees drag-end.) Paired with
 * epimone_paned_position_cb, a position change while the button is down marks an
 * interactive divider drag; the matching release flushes the coalesced winsize. */
static gboolean
epimone_page_root_event_cb (GtkEventControllerLegacy *ctrl,
                            GdkEvent                 *event,
                            gpointer                  user_data)
{
  EpimonePage *self = user_data;
  GdkEventType type = gdk_event_get_event_type (event);

  (void) ctrl;

  if ((type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE)
      && gdk_button_event_get_button (event) == GDK_BUTTON_PRIMARY)
    {
      if (type == GDK_BUTTON_PRESS)
        {
          /* Safety net: if a previous release was somehow missed (e.g. the grab
           * broke with the pointer off-screen) a stale flag would wedge the
           * suppression on. Recover by flushing before starting the next press. */
          if (self->divider_dragging)
            {
              self->divider_dragging = FALSE;
              epimone_page_flush_pending_resizes (self);
            }
          self->ptr_down = TRUE;
        }
      else
        {
          self->ptr_down = FALSE;
          if (self->divider_dragging)
            {
              self->divider_dragging = FALSE;
              epimone_page_flush_pending_resizes (self);
            }
        }
    }

  return GDK_EVENT_PROPAGATE;
}

static void
epimone_page_contents_changed_cb (VteTerminal *terminal, gpointer user_data)
{
  (void) user_data;
  epimone_page_maybe_resize (bridge_of (GTK_WIDGET (terminal)));
}

static void
epimone_page_char_size_changed_cb (VteTerminal *terminal,
                                   guint        w,
                                   guint        h,
                                   gpointer     user_data)
{
  (void) w;
  (void) h;
  (void) user_data;
  epimone_page_maybe_resize (bridge_of (GTK_WIDGET (terminal)));
}

/* ------------------------------------------------------------------ *
 * signal handlers shared by every terminal
 * ------------------------------------------------------------------ */

static void
epimone_page_title_changed_cb (VteTerminal *terminal, gpointer user_data)
{
  EpimonePage *self = epimone_page_for_terminal (GTK_WIDGET (terminal), user_data);

  if (terminal == self->focused)
    {
      epimone_page_update_title (self);
      epimone_page_update_subtitle (self);
    }
}

/* VTE reports a new working directory (OSC 7) independently of the window
 * title, so the subtitle has its own trigger. */
static void
epimone_page_directory_changed_cb (GObject *terminal, GParamSpec *pspec, gpointer user_data)
{
  EpimonePage *self = epimone_page_for_terminal (GTK_WIDGET (terminal), user_data);

  (void) pspec;

  if (VTE_TERMINAL (terminal) == self->focused)
    epimone_page_update_subtitle (self);
}

static void
epimone_page_focus_enter_cb (GtkEventControllerFocus *controller, gpointer user_data)
{
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
  EpimonePage *self = epimone_page_for_terminal (widget, user_data);

  epimone_page_set_focused (self, VTE_TERMINAL (widget));
}

static void
epimone_page_click_pressed_cb (GtkGestureClick *gesture,
                               int n_press, double x, double y,
                               gpointer user_data)
{
  GtkWidget *widget = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  (void) n_press; (void) x; (void) y; (void) user_data;
  gtk_widget_grab_focus (widget);
}

/* Put the keyboard focus back on the active pane.
 *
 * GTK does not hand focus back to the widget that had it when a popover goes
 * away: it drops the window's focus to NULL and then falls back to the first
 * focusable widget in the window. Left alone, dismissing the pane context
 * menu would strand focus on a header button, and since GtkWindow turns
 * Return into "activate the focused widget", the next Return would trigger
 * that button instead of reaching the shell. Anything that can strand focus
 * calls this rather than leaving the outcome to GTK's fallback. */
void
epimone_page_focus_terminal (EpimonePage *self)
{
  VteTerminal *term;

  g_return_if_fail (EPIMONE_IS_PAGE (self));

  term = self->focused;
  if (term == NULL)
    term = epimone_page_first_terminal (adw_bin_get_child (ADW_BIN (self)));
  if (term == NULL)
    return;   /* page with no panes left: nothing to focus */

  gtk_widget_grab_focus (GTK_WIDGET (term));
  epimone_page_set_focused (self, term);
}

/* Copy/paste/select-all operate on the focused pane's inner VteTerminal. These
 * are public so the *window* can invoke them from its win.copy/paste/select-all
 * actions: GtkApplication accelerators dispatch from the window, whose action
 * muxer reaches window/app actions but NOT widget-class actions installed on a
 * descendant page, so page-level term.* actions would never fire via
 * Ctrl+Shift+C/V/A. */
void
epimone_page_copy (EpimonePage *self)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused != NULL)
    vte_terminal_copy_clipboard_format (self->focused, VTE_FORMAT_TEXT);
}

void
epimone_page_paste (EpimonePage *self)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused != NULL)
    vte_terminal_paste_clipboard (self->focused);
}

void
epimone_page_select_all (EpimonePage *self)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused != NULL)
    vte_terminal_select_all (self->focused);
}

void
epimone_page_select_none (EpimonePage *self)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused != NULL)
    vte_terminal_unselect_all (self->focused);
}

/* Read-only state of the FOCUSED pane (not the tab): VTE's input-enabled,
 * inverted. A pane being watched can be latched so stray keystrokes cannot
 * reach its shell; the other panes of the tab are unaffected. */
gboolean
epimone_page_get_read_only (EpimonePage *self)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), FALSE);
  if (self->focused == NULL)
    return FALSE;
  return !vte_terminal_get_input_enabled (self->focused);
}

void
epimone_page_set_read_only (EpimonePage *self, gboolean read_only)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused != NULL)
    vte_terminal_set_input_enabled (self->focused, !read_only);
}

/* Reset the FOCUSED pane's terminal state (misbehaving client dumped raw
 * bytes); with @clear_history, also drop its local scrollback. Only the VTE
 * widget's view is cleared either way; the daemon's ring buffer is not
 * touched, so a detach/restore replays the history the clear removed. */
void
epimone_page_reset_terminal (EpimonePage *self, gboolean clear_history)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused != NULL)
    vte_terminal_reset (self->focused, TRUE, clear_history);
}

/* Flip a window action's enabled flag, which is what greys its menu item.
 * The actions live on the window; NULL-tolerant so an unrooted page is a no-op. */
static void
epimone_page_set_win_action_enabled (EpimonePage *self, const char *name, gboolean enabled)
{
  GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));
  GAction *action;

  if (!G_IS_ACTION_MAP (root))
    return;
  action = g_action_map_lookup_action (G_ACTION_MAP (root), name);
  if (G_IS_SIMPLE_ACTION (action))
    g_simple_action_set_enabled (G_SIMPLE_ACTION (action), enabled);
}

/* The menu is gone: whatever epimone_page_popup_menu disabled for display goes
 * back to enabled. Without this, opening the menu on a single-pane tab would
 * leave the actions off, and the accelerators would stay dead even after the
 * user splits the pane. Each action's own handler re-checks the pane count, so
 * enabled-while-single-pane just means the accelerator no-ops. */
static void
epimone_page_menu_closed_cb (GtkPopover *popover, gpointer user_data)
{
  EpimonePage *self = EPIMONE_PAGE (user_data);

  (void) popover;
  epimone_page_set_win_action_enabled (self, "open-in-new-tab", TRUE);
  epimone_page_set_win_action_enabled (self, "zoom", TRUE);
}

static void
epimone_page_popup_menu (EpimonePage *self, VteTerminal *terminal, double x, double y)
{
  graphene_point_t point;

  gtk_widget_grab_focus (GTK_WIDGET (terminal));
  epimone_page_set_focused (self, terminal);

  if (!gtk_widget_compute_point (GTK_WIDGET (terminal), GTK_WIDGET (self),
                                 &GRAPHENE_POINT_INIT ((float) x, (float) y), &point))
    return;

  /* Open in New Tab is visible but greyed out when there is only one pane
   * (nothing to move out of). Refreshed here, just before the menu reads it;
   * epimone_page_menu_closed_cb restores it. */
  {
    gboolean multi_pane = epimone_page_get_pane_count (self) >= 2;

    epimone_page_set_win_action_enabled (self, "open-in-new-tab", multi_pane);

    /* Zoom is meaningful only with 2+ panes (a pane zooms relative to its
     * siblings). Disable it on a single-pane tab, and, when 2+ panes exist,
     * label it "Unzoom Pane" while a pane is zoomed. A single pane can never be
     * zoomed, so the "Unzoom" label never arises in the disabled case. */
    epimone_page_set_win_action_enabled (self, "zoom", multi_pane);
    epimone_page_set_zoom_label (multi_pane && epimone_page_is_zoomed (self));
  }

  /* Read-Only's check mark must show the FOCUSED pane's latch, and the state
   * lives on a window action shared by every pane, so it is synced here,
   * just before the menu reads it. */
  {
    GtkRoot *root = gtk_widget_get_root (GTK_WIDGET (self));

    if (G_IS_ACTION_MAP (root))
      {
        GAction *action = g_action_map_lookup_action (G_ACTION_MAP (root),
                                                      "read-only");

        if (G_IS_SIMPLE_ACTION (action))
          g_simple_action_set_state (G_SIMPLE_ACTION (action),
                                     g_variant_new_boolean (
                                       epimone_page_get_read_only (self)));
      }
  }

  gtk_popover_set_pointing_to (GTK_POPOVER (self->popover),
                               &(GdkRectangle) { (int) point.x, (int) point.y, 1, 1 });
  gtk_popover_popup (GTK_POPOVER (self->popover));
}

static void
epimone_page_secondary_pressed_cb (GtkGestureClick *gesture,
                                   int n_press, double x, double y,
                                   gpointer user_data)
{
  GtkWidget *terminal = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (gesture));
  EpimonePage *self = epimone_page_for_terminal (terminal, user_data);

  (void) n_press;
  epimone_page_popup_menu (self, VTE_TERMINAL (terminal), x, y);
  gtk_gesture_set_state (GTK_GESTURE (gesture), GTK_EVENT_SEQUENCE_CLAIMED);
}

static gboolean
epimone_page_key_pressed_cb (GtkEventControllerKey *controller,
                             guint keyval, guint keycode,
                             GdkModifierType state, gpointer user_data)
{
  GtkWidget *terminal = gtk_event_controller_get_widget (GTK_EVENT_CONTROLLER (controller));
  EpimonePage *self = epimone_page_for_terminal (terminal, user_data);
  GdkModifierType mods = state & gtk_accelerator_get_default_mod_mask ();
  (void) keycode;

  /* This controller runs in the capture phase (before VTE) only to catch the
   * bare Menu key / Shift+F10 for the context menu. It must NEVER swallow the
   * shell's control keystrokes: any Ctrl/Alt/Super-modified key is propagated
   * unconditionally so Ctrl+C -> SIGINT (and Ctrl+D/Z/L/A/E/W/U/R) always reach
   * the terminal. (Ctrl+Shift+C/V are app accelerators handled upstream.) */
  if (mods & (GDK_CONTROL_MASK | GDK_ALT_MASK | GDK_SUPER_MASK))
    return GDK_EVENT_PROPAGATE;

  if (keyval == GDK_KEY_Menu ||
      (keyval == GDK_KEY_F10 && mods == GDK_SHIFT_MASK))
    {
      double cx = gtk_widget_get_width (terminal) / 2.0;
      double cy = gtk_widget_get_height (terminal) / 2.0;
      epimone_page_popup_menu (self, VTE_TERMINAL (terminal), cx, cy);
      return GDK_EVENT_STOP;
    }
  return GDK_EVENT_PROPAGATE;
}

/* ------------------------------------------------------------------ *
 * terminal + bridge construction
 * ------------------------------------------------------------------ */

/* ------------------------------------------------------------------ *
 * Appearance (colors / font / cursor)
 *
 * A process-wide registry of every live VteTerminal plus the current desired
 * appearance. Setters update the state and re-apply to all terminals; new
 * terminals apply the current state when built, so they inherit it. Opacity is
 * carried as the alpha of the background color (see the note in the setter).
 * ------------------------------------------------------------------ */

static GSList *epimone_all_terminals = NULL;   /* borrowed VteTerminal* */

/* Who to tell when colors or font change (see the note in epimone-page.h).
 * Cursor shape, bell and the scroll toggles do not notify: they change how a
 * live terminal behaves, not what already-rendered pixels look like. */
typedef struct
{
  EpimoneAppearanceListener listener;
  gpointer                  user_data;
} EpimoneAppearanceWatch;

static GSList *epimone_appearance_watches = NULL;   /* owned EpimoneAppearanceWatch* */

static void
epimone_appearance_notify (void)
{
  for (GSList *l = epimone_appearance_watches; l != NULL; l = l->next)
    {
      EpimoneAppearanceWatch *w = l->data;

      w->listener (w->user_data);
    }
}

static struct
{
  gboolean            have_colors;
  GdkRGBA             background;
  GdkRGBA             foreground;
  GdkRGBA             cursor;
  GdkRGBA             palette[16];
  gsize               palette_len;
  char               *font;          /* NULL/"" = system monospace */
  EpimoneCursorShape  cursor_shape;
  glong               scrollback_lines;   /* -1 = unlimited (VTE's convention) */
  gboolean            audible_bell;
  gboolean            scroll_on_output;
  gboolean            scroll_on_keystroke;
  EpimoneEraseBinding backspace_binding;
  EpimoneEraseBinding delete_binding;
  int                 cjk_ambiguous_width; /* VTE's raw 1 (narrow) or 2 (wide) */
  int                 default_columns;    /* grid a NEW terminal asks for */
  int                 default_rows;
} epimone_appearance = {
  .have_colors = FALSE,
  .palette_len = 0,
  .font = NULL,
  .cursor_shape = EPIMONE_CURSOR_BLOCK,
  /* Mirror the schema defaults, so a terminal built before the saved settings
   * are read (or with no settings at all) still starts out sane. */
  .scrollback_lines = 10000,
  .audible_bell = TRUE,
  .scroll_on_output = FALSE,
  .scroll_on_keystroke = TRUE,
  .backspace_binding = EPIMONE_ERASE_AUTO,
  .delete_binding = EPIMONE_ERASE_AUTO,
  .cjk_ambiguous_width = 1,
  .default_columns = 80,
  .default_rows = 24,
};

/* Working-directory-inheritance policy, read at spawn time (not applied to open
 * terminals). Default ALWAYS reproduces the previous inherit-everything
 * behaviour. */
static EpimonePreserveDirectory epimone_preserve_directory = EPIMONE_PRESERVE_ALWAYS;

static VteCursorShape
epimone_cursor_shape_to_vte (EpimoneCursorShape shape)
{
  switch (shape)
    {
    case EPIMONE_CURSOR_IBEAM:     return VTE_CURSOR_SHAPE_IBEAM;
    case EPIMONE_CURSOR_UNDERLINE: return VTE_CURSOR_SHAPE_UNDERLINE;
    case EPIMONE_CURSOR_BLOCK:
    default:                       return VTE_CURSOR_SHAPE_BLOCK;
    }
}

EpimoneCursorShape
epimone_cursor_shape_from_id (const char *id)
{
  if (g_strcmp0 (id, "ibeam") == 0)
    return EPIMONE_CURSOR_IBEAM;
  if (g_strcmp0 (id, "underline") == 0)
    return EPIMONE_CURSOR_UNDERLINE;
  return EPIMONE_CURSOR_BLOCK;
}

const char *
epimone_cursor_shape_to_id (EpimoneCursorShape shape)
{
  switch (shape)
    {
    case EPIMONE_CURSOR_IBEAM:     return "ibeam";
    case EPIMONE_CURSOR_UNDERLINE: return "underline";
    case EPIMONE_CURSOR_BLOCK:
    default:                       return "block";
    }
}

/* VteEraseBinding shares EpimoneEraseBinding's values 1:1 (see the header), so
 * the cast is exact; the switch is only there to stay honest if either enum is
 * ever reordered. */
static VteEraseBinding
epimone_erase_binding_to_vte (EpimoneEraseBinding binding)
{
  switch (binding)
    {
    case EPIMONE_ERASE_ASCII_BACKSPACE: return VTE_ERASE_ASCII_BACKSPACE;
    case EPIMONE_ERASE_ASCII_DELETE:    return VTE_ERASE_ASCII_DELETE;
    case EPIMONE_ERASE_DELETE_SEQUENCE: return VTE_ERASE_DELETE_SEQUENCE;
    case EPIMONE_ERASE_TTY:             return VTE_ERASE_TTY;
    case EPIMONE_ERASE_AUTO:
    default:                            return VTE_ERASE_AUTO;
    }
}

EpimoneEraseBinding
epimone_erase_binding_from_id (const char *id)
{
  if (g_strcmp0 (id, "ascii-backspace") == 0) return EPIMONE_ERASE_ASCII_BACKSPACE;
  if (g_strcmp0 (id, "ascii-delete") == 0)    return EPIMONE_ERASE_ASCII_DELETE;
  if (g_strcmp0 (id, "delete-sequence") == 0) return EPIMONE_ERASE_DELETE_SEQUENCE;
  if (g_strcmp0 (id, "tty") == 0)             return EPIMONE_ERASE_TTY;
  return EPIMONE_ERASE_AUTO;
}

const char *
epimone_erase_binding_to_id (EpimoneEraseBinding binding)
{
  switch (binding)
    {
    case EPIMONE_ERASE_ASCII_BACKSPACE: return "ascii-backspace";
    case EPIMONE_ERASE_ASCII_DELETE:    return "ascii-delete";
    case EPIMONE_ERASE_DELETE_SEQUENCE: return "delete-sequence";
    case EPIMONE_ERASE_TTY:             return "tty";
    case EPIMONE_ERASE_AUTO:
    default:                            return "auto";
    }
}

EpimonePreserveDirectory
epimone_preserve_directory_from_id (const char *id)
{
  if (g_strcmp0 (id, "never") == 0) return EPIMONE_PRESERVE_NEVER;
  if (g_strcmp0 (id, "safe") == 0)  return EPIMONE_PRESERVE_SAFE;
  return EPIMONE_PRESERVE_ALWAYS;
}

const char *
epimone_preserve_directory_to_id (EpimonePreserveDirectory policy)
{
  switch (policy)
    {
    case EPIMONE_PRESERVE_NEVER: return "never";
    case EPIMONE_PRESERVE_SAFE:  return "safe";
    case EPIMONE_PRESERVE_ALWAYS:
    default:                     return "always";
    }
}

/* The audible bell a terminal should currently have: the user's setting, unless
 * its replay burst is still being fed. */
static void
epimone_terminal_apply_bell (VteTerminal *t)
{
  vte_terminal_set_audible_bell (t,
    epimone_appearance.audible_bell && !epimone_terminal_bell_suppressed (t));
}

/* Push the whole current appearance onto one terminal. */
static void
epimone_apply_appearance_to (VteTerminal *t)
{
  if (epimone_appearance.have_colors)
    {
      vte_terminal_set_colors (t,
                               &epimone_appearance.foreground,
                               &epimone_appearance.background,
                               epimone_appearance.palette,
                               epimone_appearance.palette_len);
      vte_terminal_set_color_cursor (t, &epimone_appearance.cursor);
    }

  if (epimone_appearance.font != NULL && epimone_appearance.font[0] != '\0')
    {
      PangoFontDescription *d =
        pango_font_description_from_string (epimone_appearance.font);

      vte_terminal_set_font (t, d);
      pango_font_description_free (d);
    }
  else
    {
      vte_terminal_set_font (t, NULL);   /* NULL = the system monospace font */
    }

  vte_terminal_set_cursor_shape (
    t, epimone_cursor_shape_to_vte (epimone_appearance.cursor_shape));

  vte_terminal_set_scrollback_lines (t, epimone_appearance.scrollback_lines);
  epimone_terminal_apply_bell (t);
  vte_terminal_set_scroll_on_output (t, epimone_appearance.scroll_on_output);
  vte_terminal_set_scroll_on_keystroke (t, epimone_appearance.scroll_on_keystroke);

  vte_terminal_set_backspace_binding (
    t, epimone_erase_binding_to_vte (epimone_appearance.backspace_binding));
  vte_terminal_set_delete_binding (
    t, epimone_erase_binding_to_vte (epimone_appearance.delete_binding));
  vte_terminal_set_cjk_ambiguous_width (t, epimone_appearance.cjk_ambiguous_width);
}

void
epimone_terminals_apply_appearance (GtkWidget *terminal)
{
  g_return_if_fail (VTE_IS_TERMINAL (terminal));
  epimone_apply_appearance_to (VTE_TERMINAL (terminal));
}

void
epimone_terminals_add_appearance_listener (EpimoneAppearanceListener listener,
                                           gpointer                  user_data)
{
  EpimoneAppearanceWatch *w;

  g_return_if_fail (listener != NULL);

  w = g_new0 (EpimoneAppearanceWatch, 1);
  w->listener = listener;
  w->user_data = user_data;
  epimone_appearance_watches = g_slist_prepend (epimone_appearance_watches, w);
}

void
epimone_terminals_remove_appearance_listener (EpimoneAppearanceListener listener,
                                              gpointer                  user_data)
{
  for (GSList *l = epimone_appearance_watches; l != NULL; l = l->next)
    {
      EpimoneAppearanceWatch *w = l->data;

      if (w->listener == listener && w->user_data == user_data)
        {
          epimone_appearance_watches =
            g_slist_delete_link (epimone_appearance_watches, l);
          g_free (w);
          return;
        }
    }
}

static void
epimone_apply_appearance_to_all (void)
{
  for (GSList *l = epimone_all_terminals; l != NULL; l = l->next)
    epimone_apply_appearance_to (VTE_TERMINAL (l->data));
}

static void
epimone_terminal_destroyed_cb (GtkWidget *w, gpointer user_data)
{
  (void) user_data;

  epimone_all_terminals = g_slist_remove (epimone_all_terminals, w);
}

/* Track a newly built terminal and give it the current appearance, so a new tab
 * or split matches the ones already open. */
static void
epimone_register_terminal (VteTerminal *t)
{
  epimone_all_terminals = g_slist_prepend (epimone_all_terminals, t);
  g_signal_connect (t, "destroy", G_CALLBACK (epimone_terminal_destroyed_cb), NULL);

  epimone_apply_appearance_to (t);
}

/* Colors arrive as one set: a palette without its foreground/background is not
 * a usable theme, so a NULL in any of the three is ignored outright. The
 * background's alpha is the terminal opacity; it is stored as given and handed
 * to VTE unchanged. */
void
epimone_terminals_set_colors (const GdkRGBA *background,
                              const GdkRGBA *foreground,
                              const GdkRGBA *cursor,
                              const GdkRGBA *palette,
                              gsize          palette_len)
{
  if (background == NULL || foreground == NULL || cursor == NULL)
    return;

  epimone_appearance.background = *background;
  epimone_appearance.foreground = *foreground;
  epimone_appearance.cursor = *cursor;
  epimone_appearance.palette_len = MIN (palette_len, 16);
  for (gsize i = 0; i < epimone_appearance.palette_len; i++)
    epimone_appearance.palette[i] = palette[i];
  epimone_appearance.have_colors = TRUE;

  epimone_apply_appearance_to_all ();
  /* Each dim scrim paints in this background colour; repaint them all so a
   * palette switch tracks the new background instead of leaving the previous
   * theme's colour behind. The scrim draw func reads
   * epimone_appearance.background live, so invalidation is all that is needed
   * here; there is no cached fill to update. */
  epimone_pages_invalidate_scrims ();
  epimone_appearance_notify ();
}

void
epimone_terminals_set_font (const char *font_name)
{
  g_free (epimone_appearance.font);
  epimone_appearance.font = g_strdup (font_name);
  epimone_apply_appearance_to_all ();
  epimone_appearance_notify ();
}

void
epimone_terminals_set_cursor_shape (EpimoneCursorShape shape)
{
  epimone_appearance.cursor_shape = shape;
  epimone_apply_appearance_to_all ();
}

/* Anything negative means "unlimited", which VTE spells -1; 0 disables
 * scrollback. */
void
epimone_terminals_set_scrollback_lines (glong lines)
{
  epimone_appearance.scrollback_lines = (lines < 0) ? -1 : lines;
  epimone_apply_appearance_to_all ();
}

void
epimone_terminals_set_audible_bell (gboolean enabled)
{
  epimone_appearance.audible_bell = enabled;
  epimone_apply_appearance_to_all ();
}

void
epimone_terminals_set_scroll_on_output (gboolean enabled)
{
  epimone_appearance.scroll_on_output = enabled;
  epimone_apply_appearance_to_all ();
}

void
epimone_terminals_set_scroll_on_keystroke (gboolean enabled)
{
  epimone_appearance.scroll_on_keystroke = enabled;
  epimone_apply_appearance_to_all ();
}

void
epimone_terminals_set_backspace_binding (EpimoneEraseBinding binding)
{
  epimone_appearance.backspace_binding = binding;
  epimone_apply_appearance_to_all ();
}

void
epimone_terminals_set_delete_binding (EpimoneEraseBinding binding)
{
  epimone_appearance.delete_binding = binding;
  epimone_apply_appearance_to_all ();
}

/* VTE only understands widths 1 and 2; anything else means narrow. */
void
epimone_terminals_set_cjk_ambiguous_width (int width)
{
  epimone_appearance.cjk_ambiguous_width = (width == 2) ? 2 : 1;
  epimone_apply_appearance_to_all ();
}

void
epimone_terminals_set_preserve_directory (EpimonePreserveDirectory policy)
{
  epimone_preserve_directory = policy;
}

/* Unlike the setters above this does not touch open terminals: the grid is a
 * size *request* a terminal makes when it is built, so changing it can only
 * affect terminals created afterwards (in practice, the next new window). */
void
epimone_terminals_set_default_size (int columns, int rows)
{
  epimone_appearance.default_columns = MAX (columns, 1);
  epimone_appearance.default_rows = MAX (rows, 1);
}

/* Interior padding is CSS, not a VTE property: one app-level provider carrying
 * a `vte-terminal { padding: Npx; }` rule reaches every terminal at once,
 * including ones created later, and re-loading the provider replaces the rule
 * rather than stacking another one. */
void
epimone_terminals_set_padding (int px)
{
  static GtkCssProvider *provider;
  g_autofree char *css = NULL;

  px = CLAMP (px, 0, 32);

  if (provider == NULL)
    {
      provider = gtk_css_provider_new ();
      gtk_style_context_add_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

  css = g_strdup_printf ("vte-terminal { padding: %dpx; }", px);
  gtk_css_provider_load_from_string (provider, css);
}

static GtkWidget *
epimone_page_make_vte (EpimonePage *self)
{
  GtkWidget *terminal;
  GtkEventController *focus;
  GtkGesture *click;
  GtkGesture *secondary;
  GtkEventController *keys;

  terminal = vte_terminal_new ();
  /* Scrollback (and bell / scroll-on-* behaviour) comes from the shared
   * appearance state via epimone_register_terminal() at the end of this
   * function, so a new tab or split inherits whatever is currently configured
   * rather than a hardcoded default. */

  /* The configured default grid becomes this terminal's size request. For the
   * first terminal in a window that is what the window sizes itself around
   * (epimone-window.c sets no pixel default size); for a split, the pane
   * allocation immediately overrides it, which is the intended behaviour. */
  vte_terminal_set_size (VTE_TERMINAL (terminal),
                         epimone_appearance.default_columns,
                         epimone_appearance.default_rows);

  focus = gtk_event_controller_focus_new ();
  g_signal_connect (focus, "enter", G_CALLBACK (epimone_page_focus_enter_cb), self);
  gtk_widget_add_controller (terminal, focus);

  click = gtk_gesture_click_new ();
  g_signal_connect (click, "pressed", G_CALLBACK (epimone_page_click_pressed_cb), self);
  gtk_widget_add_controller (terminal, GTK_EVENT_CONTROLLER (click));

  secondary = gtk_gesture_click_new ();
  gtk_gesture_single_set_button (GTK_GESTURE_SINGLE (secondary), GDK_BUTTON_SECONDARY);
  gtk_event_controller_set_propagation_phase (GTK_EVENT_CONTROLLER (secondary), GTK_PHASE_CAPTURE);
  g_signal_connect (secondary, "pressed", G_CALLBACK (epimone_page_secondary_pressed_cb), self);
  gtk_widget_add_controller (terminal, GTK_EVENT_CONTROLLER (secondary));

  keys = gtk_event_controller_key_new ();
  gtk_event_controller_set_propagation_phase (keys, GTK_PHASE_CAPTURE);
  g_signal_connect (keys, "key-pressed", G_CALLBACK (epimone_page_key_pressed_cb), self);
  gtk_widget_add_controller (terminal, keys);

  g_signal_connect (terminal, "window-title-changed",
                    G_CALLBACK (epimone_page_title_changed_cb), self);
  g_signal_connect (terminal, "notify::current-directory-uri",
                    G_CALLBACK (epimone_page_directory_changed_cb), self);
  g_signal_connect (terminal, "contents-changed",
                    G_CALLBACK (epimone_page_contents_changed_cb), self);
  g_signal_connect (terminal, "char-size-changed",
                    G_CALLBACK (epimone_page_char_size_changed_cb), self);

  /* Track for live appearance updates and apply the current colors/font/etc. */
  epimone_register_terminal (VTE_TERMINAL (terminal));

  return terminal;
}

/* Attach `terminal` to daemon session `id` over `sock_fd`, bridging it to a
 * fresh local PTY that VTE drives. Takes ownership of sock_fd. */
static gboolean
epimone_page_attach_bridge (EpimonePage *self,
                            GtkWidget   *terminal,
                            guint64      id,
                            int          sock_fd)
{
  EpiBridge *b;
  int master = -1, slave = -1;
  struct termios tio;
  VtePty *pty;
  GError *err = NULL;

  if (openpty (&master, &slave, NULL, NULL, NULL) != 0)
    {
      close (sock_fd);
      return FALSE;
    }

  /* Raw discipline so the local PTY is a transparent byte pipe. */
  if (tcgetattr (slave, &tio) == 0)
    {
      cfmakeraw (&tio);
      tcsetattr (slave, TCSANOW, &tio);
    }
  set_nonblock_cloexec (slave);
  fcntl (master, F_SETFD, FD_CLOEXEC);

  pty = vte_pty_new_foreign_sync (master, NULL, &err);
  if (pty == NULL)
    {
      g_warning ("vte_pty_new_foreign_sync failed: %s", err ? err->message : "?");
      g_clear_error (&err);
      close (master);
      close (slave);
      close (sock_fd);
      return FALSE;
    }
  vte_terminal_set_pty (VTE_TERMINAL (terminal), pty);
  g_object_unref (pty);   /* the terminal now holds the only ref */

  b = g_new0 (EpiBridge, 1);
  b->page = self;
  b->term = VTE_TERMINAL (terminal);
  b->session_id = id;
  b->sock_fd = sock_fd;
  b->pty_slave = slave;
  b->to_sock = g_byte_array_new ();
  b->to_slave = g_byte_array_new ();
  b->last_rows = -1;
  b->last_cols = -1;

  /* Mute the bell for the ring replay that is about to arrive; the first chunk
   * opens the quiet window and the last one closes it. */
  b->attached_at = g_get_monotonic_time ();
  epimone_terminal_set_bell_suppressed (VTE_TERMINAL (terminal), TRUE);
  b->bell_quiet_id = g_timeout_add (EPI_BELL_QUIET_MS,
                                    epimone_bridge_bell_quiet_cb, b);

  b->sock_in_id = g_unix_fd_add (sock_fd, G_IO_IN | G_IO_ERR | G_IO_HUP,
                                 on_sock_readable, b);
  b->slave_in_id = g_unix_fd_add (slave, G_IO_IN | G_IO_ERR | G_IO_HUP,
                                  on_slave_readable, b);

  g_object_set_data_full (G_OBJECT (terminal), EPI_BRIDGE_KEY, b, bridge_free);
  return TRUE;
}

/* ------------------------------------------------------------------ *
 * Inactive-pane dimming
 *
 * Each leaf carries a scrim: a GtkDrawingArea layered by a GtkOverlay on top of
 * the leaf's GtkScrolledWindow, can-target FALSE so clicks, scroll and the
 * hover-reveal of the overlay scrollbar all pass straight through to the
 * terminal underneath. When a pane in a split tab does not hold focus its
 * scrim is shown; it fills the whole leaf with the terminal's OWN background
 * colour at EPI_DIM_ALPHA.
 *
 * That fill colour is the exact GdkRGBA handed to vte_terminal_set_colors, so
 * empty cells of an unfocused pane composite background-over-background and end
 * pixel-identical to a focused pane; only ink (foreground over background)
 * shifts toward the background. The scrim is a child of the overlay, i.e. of
 * one GtkPaned child, so it can never reach across the paned handle or its
 * padding: the divider is drawn by CSS on the separator node and is left
 * untouched here, on every theme and in every focus state.
 *
 * The draw func reads epimone_appearance.background LIVE on every paint, so a
 * palette change cannot strand a stale colour; a colour change only has to
 * invalidate the scrims (epimone_pages_invalidate_scrims, called from
 * epimone_terminals_set_colors).
 *
 * Alpha: 0.44. The fill bites only on ink, so this reads as a clear fade
 * rather than a wash: measured (offscreen GSK render, both integer scales) the
 * default palette's focused body text drops from 9.96:1 to 4.05:1 contrast
 * (#c8c8c8 -> ~#7d7d7d, darkening toward the #1e1e1e background), and
 * Solarized Light fades the same direction, 4.13:1 -> 2.04:1 (#657b83 ->
 * ~#a8b1ad, lightening toward #fdf6e3). 0.44 was picked over its neighbours
 * because 8-bit premultiplied-alpha compositing double-rounds bg-over-bg, and
 * at 0.44 that rounding lands EXACTLY on the background for both test palettes
 * (empty cells stay pixel-identical to the focused pane); 0.42/0.45 leave a
 * 1-LSB residue on the darkest palette. It is the strongest fade inside the
 * 0.40-0.45 band that keeps empty regions exact. Deliberately NOT driven from
 * libadwaita's --dim-opacity: that needs libadwaita 1.6, and Epimone's floor
 * is 1.5 (load-bearing for Ubuntu 24.04). */
#define EPI_DIM_ALPHA 0.44
#define EPI_SCRIM_KEY "epi-scrim"

static gboolean epimone_dim_inactive = FALSE;   /* current global setting */
static GSList  *epimone_all_pages = NULL;        /* borrowed EpimonePage* */

/* While TRUE every scrim is forced hidden regardless of focus, so a live-widget
 * capture (the overview's active-tab thumbnail) does not bake pane dimming into
 * its texture. The overview's PEEK-rendered thumbnails never carry dimming, so
 * suppressing it here is what makes the two thumbnail producers agree; a later
 * refresh of a card cannot then brighten it. Set only for the synchronous span
 * of a capture and cleared immediately, so no frame is ever presented undimmed. */
static gboolean epimone_capture_suppress_dim = FALSE;

static void
epimone_scrim_draw (GtkDrawingArea *area, cairo_t *cr,
                    int width, int height, gpointer user_data)
{
  (void) area;
  (void) user_data;

  if (!epimone_appearance.have_colors)
    return;

  /* Suppressed for the duration of a live-widget thumbnail capture, so the
   * captured texture carries no dimming and matches the overview's PEEK-rendered
   * thumbnails (which never dim). Gating the DRAW rather than the scrim's
   * visibility is deliberate: toggling visibility queues a resize that
   * invalidates the live view's cached render node, which the capture's
   * GtkWidgetPaintable then reads as empty. */
  if (epimone_capture_suppress_dim)
    return;

  /* Take the background's RGB and OUR alpha, ignoring the background's own
   * alpha (which VTE reads as terminal opacity): the scrim must match the
   * opaque background pixel it sits over so empty regions stay identical. */
  cairo_set_source_rgba (cr,
                         epimone_appearance.background.red,
                         epimone_appearance.background.green,
                         epimone_appearance.background.blue,
                         EPI_DIM_ALPHA);
  cairo_rectangle (cr, 0, 0, width, height);
  cairo_fill (cr);
}

/* The scrim drawing area of a leaf, or NULL for a bare / unwrapped leaf. */
static GtkWidget *
epimone_leaf_scrim (GtkWidget *leaf)
{
  if (leaf != NULL && GTK_IS_OVERLAY (leaf))
    return g_object_get_data (G_OBJECT (leaf), EPI_SCRIM_KEY);
  return NULL;
}

/* Collect every leaf widget (the split-tree node, not its VteTerminal) under a
 * subtree. Mirrors epimone_page_collect_terminals but stops at the leaf. */
static void
epimone_page_collect_leaves (GtkWidget *widget, GPtrArray *out)
{
  if (widget == NULL)
    return;
  if (GTK_IS_PANED (widget))
    {
      epimone_page_collect_leaves (gtk_paned_get_start_child (GTK_PANED (widget)), out);
      epimone_page_collect_leaves (gtk_paned_get_end_child (GTK_PANED (widget)), out);
    }
  else
    {
      g_ptr_array_add (out, widget);
    }
}

/* Recompute which leaves of this page wear their scrim. A leaf is dimmed when
 * the feature is on, the tab holds more than one pane, and the leaf is not the
 * focused one. A single-pane tab is therefore never dimmed. Cheap and
 * idempotent: called on every focus move, split, close and pane adoption. */
static void
epimone_page_update_dimming (EpimonePage *self)
{
  g_autoptr (GPtrArray) leaves = NULL;
  GtkWidget *root;
  GtkWidget *focused_leaf;

  root = adw_bin_get_child (ADW_BIN (self));
  if (root == NULL)
    return;

  leaves = g_ptr_array_new ();
  epimone_page_collect_leaves (root, leaves);

  focused_leaf = (self->focused != NULL)
                   ? epimone_terminal_leaf (self->focused)
                   : NULL;

  for (guint i = 0; i < leaves->len; i++)
    {
      GtkWidget *leaf = g_ptr_array_index (leaves, i);
      GtkWidget *scrim = epimone_leaf_scrim (leaf);
      gboolean dim = epimone_dim_inactive
                     && leaves->len > 1
                     && leaf != focused_leaf;

      if (scrim != NULL)
        gtk_widget_set_visible (scrim, dim);
    }
}

/* Invalidate every scrim so it repaints from the current palette background.
 * Visibility is untouched; only the fill colour can have changed. */
static void
epimone_pages_invalidate_scrims (void)
{
  for (GSList *l = epimone_all_pages; l != NULL; l = l->next)
    {
      EpimonePage *page = EPIMONE_PAGE (l->data);
      g_autoptr (GPtrArray) leaves = g_ptr_array_new ();
      GtkWidget *root = adw_bin_get_child (ADW_BIN (page));

      if (root == NULL)
        continue;
      epimone_page_collect_leaves (root, leaves);
      for (guint i = 0; i < leaves->len; i++)
        {
          GtkWidget *scrim = epimone_leaf_scrim (g_ptr_array_index (leaves, i));

          if (scrim != NULL)
            gtk_widget_queue_draw (scrim);
        }
    }
}

void
epimone_pages_set_dim_inactive (gboolean enabled)
{
  epimone_dim_inactive = enabled;
  for (GSList *l = epimone_all_pages; l != NULL; l = l->next)
    epimone_page_update_dimming (EPIMONE_PAGE (l->data));
}

/* Suppress scrim painting (@suppress TRUE) or restore it (@suppress FALSE) for
 * the span of a live-widget capture, so the captured thumbnail carries no
 * dimming and matches the overview's PEEK-rendered thumbnails. Only the scrim
 * DRAW is gated (see epimone_scrim_draw); visibility and layout are untouched,
 * so the live view's render node is not invalidated. Each scrim is queued for
 * redraw so the change reaches the pixels the capture reads. Because the caller
 * brackets a synchronous capture, the live window never presents a dimmed-off
 * frame. */
void
epimone_pages_suppress_dim_for_capture (gboolean suppress)
{
  if (epimone_capture_suppress_dim == suppress)
    return;
  epimone_capture_suppress_dim = suppress;

  /* Mark each scrim dirty so a live re-render (gtk_widget_snapshot_child) picks
   * up the changed draw gate. */
  for (GSList *l = epimone_all_pages; l != NULL; l = l->next)
    {
      EpimonePage *page = EPIMONE_PAGE (l->data);
      g_autoptr (GPtrArray) leaves = g_ptr_array_new ();
      GtkWidget *root = adw_bin_get_child (ADW_BIN (page));

      if (root == NULL)
        continue;
      epimone_page_collect_leaves (root, leaves);
      for (guint i = 0; i < leaves->len; i++)
        {
          GtkWidget *scrim = epimone_leaf_scrim (g_ptr_array_index (leaves, i));

          if (scrim != NULL)
            gtk_widget_queue_draw (scrim);
        }
    }
}

/* Wrap a terminal in a leaf: a GtkScrolledWindow (overlay scrollbar that hides
 * during use and reveals on hover/scroll; no horizontal scrolling; the
 * scrolled window drives VTE's scrollback natively, VTE being GtkScrollable)
 * inside a GtkOverlay whose overlay child is the dimming scrim (see above).
 * The GtkOverlay is the node that lives in the split tree; epimone_leaf_terminal
 * / epimone_terminal_leaf convert between the overlay and its VteTerminal. */
static GtkWidget *
epimone_page_wrap_terminal (GtkWidget *terminal)
{
  GtkWidget *sw = gtk_scrolled_window_new ();
  GtkWidget *overlay;
  GtkWidget *scrim;

  gtk_scrolled_window_set_has_frame (GTK_SCROLLED_WINDOW (sw), FALSE);
  gtk_scrolled_window_set_overlay_scrolling (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (sw),
                                  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  /* Pass the terminal's natural size through instead of reporting the scrolled
   * window's (much smaller) minimum. Without this the leaf swallows the grid
   * the terminal asked for, and a new window opens at GTK's minimum rather
   * than at default-columns x default-rows. Costs nothing once the window has
   * a size: it only feeds the initial natural-size request. */
  gtk_scrolled_window_set_propagate_natural_width (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_propagate_natural_height (GTK_SCROLLED_WINDOW (sw), TRUE);
  gtk_scrolled_window_set_child (GTK_SCROLLED_WINDOW (sw), terminal);

  overlay = gtk_overlay_new ();
  gtk_overlay_set_child (GTK_OVERLAY (overlay), sw);

  /* The scrim fills the overlay (default FILL alignment) and only ever covers
   * this leaf's scrolled window, never the surrounding GtkPaned handle. Hidden
   * until epimone_page_update_dimming decides the pane is unfocused. */
  scrim = gtk_drawing_area_new ();
  gtk_drawing_area_set_draw_func (GTK_DRAWING_AREA (scrim),
                                  epimone_scrim_draw, NULL, NULL);
  gtk_widget_set_can_target (scrim, FALSE);
  gtk_widget_set_visible (scrim, FALSE);
  gtk_overlay_add_overlay (GTK_OVERLAY (overlay), scrim);
  g_object_set_data (G_OBJECT (overlay), EPI_SCRIM_KEY, scrim);

  return overlay;
}

/* Leaf for an already-existing session (restore path). NULL on failure. */
GtkWidget *
epimone_page_create_terminal_for_session (EpimonePage *self, guint64 id)
{
  GtkWidget *terminal;
  int fd;
  GError *err = NULL;

  fd = epimone_client_attach_session (id, &err);
  if (fd < 0)
    {
      g_clear_error (&err);
      return NULL;
    }

  terminal = epimone_page_make_vte (self);
  if (!epimone_page_attach_bridge (self, terminal, id, fd))
    {
      g_object_ref_sink (terminal);
      g_object_unref (terminal);
      return NULL;
    }
  return epimone_page_wrap_terminal (terminal);
}

/* Leaf for a brand-new session in `cwd`. Always returns a widget; on failure
 * the terminal shows an error message. */
static GtkWidget *
epimone_page_create_terminal (EpimonePage *self, const char *cwd)
{
  GtkWidget *terminal = epimone_page_make_vte (self);
  guint64 id;
  int fd = -1;
  GError *err = NULL;

  id = epimone_client_create_session (cwd, &err);
  if (id != 0)
    fd = epimone_client_attach_session (id, &err);

  if (fd < 0 || !epimone_page_attach_bridge (self, terminal, id, fd))
    {
      /* Show the reason in the pane. A custom command that will not parse
       * fails here with a specific message, and printing it is the difference
       * between a fixable mistake and a pane that is simply dead. */
      g_autofree char *msg =
        g_strdup_printf ("\r\n  [epimone: could not start a session: %s]\r\n",
                         (err != NULL) ? err->message : "unknown error");

      if (err != NULL)
        g_warning ("session start failed: %s", err->message);
      vte_terminal_feed (VTE_TERMINAL (terminal), msg, -1);
    }
  else
    {
      /* Only on the create path: re-attaching to an existing session (restore)
       * goes through epimone_page_create_terminal_for_session and must not
       * replay the launch command. */
      epimone_client_send_launch_command (fd);
    }
  g_clear_error (&err);
  return epimone_page_wrap_terminal (terminal);
}

char *
epimone_page_dup_cwd (EpimonePage *self)
{
  const char *uri = NULL;

  if (self->focused == NULL)
    return NULL;

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  uri = vte_terminal_get_current_directory_uri (self->focused);
  G_GNUC_END_IGNORE_DEPRECATIONS

  if (uri == NULL || uri[0] == '\0')
    return NULL;
  return g_filename_from_uri (uri, NULL, NULL);
}

char *
epimone_page_dup_cwd_for_spawn (EpimonePage *self)
{
  const char *uri = NULL;
  g_autofree char *host = NULL;
  char *path;

  g_return_val_if_fail (EPIMONE_IS_PAGE (self), NULL);

  /* NEVER never inherits, so the URI need not even be read. */
  if (epimone_preserve_directory == EPIMONE_PRESERVE_NEVER)
    return NULL;
  if (self->focused == NULL)
    return NULL;

  G_GNUC_BEGIN_IGNORE_DEPRECATIONS
  uri = vte_terminal_get_current_directory_uri (self->focused);
  G_GNUC_END_IGNORE_DEPRECATIONS
  if (uri == NULL || uri[0] == '\0')
    return NULL;

  /* Capture the host too: SAFE needs it, unlike epimone_page_dup_cwd(). */
  path = g_filename_from_uri (uri, &host, NULL);
  if (path == NULL)
    return NULL;

  /* SAFE declines a directory reported from another host (a live SSH session)
   * whose path need not exist locally; ALWAYS takes it regardless. An absent or
   * "localhost" host is local; shell OSC 7 hooks usually emit the machine's own
   * name even locally, so that counts as local as well. */
  if (epimone_preserve_directory == EPIMONE_PRESERVE_SAFE &&
      host != NULL && host[0] != '\0' &&
      g_ascii_strcasecmp (host, "localhost") != 0 &&
      g_ascii_strcasecmp (host, g_get_host_name ()) != 0)
    {
      g_free (path);
      return NULL;
    }

  return path;
}

/* ------------------------------------------------------------------ *
 * split tree operations
 * ------------------------------------------------------------------ */

/* A divider moved: the tab's split ratios have changed, so the group blob needs
 * rewriting. Deliberately routed to the group-only scheduler: it shares the one
 * debounce timer, so a drag coalesces into a single write instead of one per
 * motion event, and layout.json keeps its own write triggers unchanged. */
static void
epimone_paned_position_cb (GObject *paned, GParamSpec *pspec, gpointer user_data)
{
  (void) pspec;
  (void) user_data;

  /* A position change while the primary button is down is an interactive divider
   * drag: mark the page so per-motion winsize pushes are held until release (see
   * epimone_page_maybe_resize). Programmatic changes (layout restore, zoom) run
   * with no button down and are left alone. */
  if (paned != NULL)
    {
      GtkWidget *anc = gtk_widget_get_ancestor (GTK_WIDGET (paned),
                                                EPIMONE_TYPE_PAGE);
      if (anc != NULL)
        {
          EpimonePage *page = EPIMONE_PAGE (anc);
          if (page->ptr_down)
            page->divider_dragging = TRUE;
        }
    }

  epimone_layout_schedule_group_save ();
}

GtkWidget *
epimone_page_new_paned (GtkOrientation orientation)
{
  GtkWidget *paned = gtk_paned_new (orientation);

  /* Non-wide handle: the hairline look and grab size come from the app CSS
   * (paned > separator), not the theme's wide grip. */
  gtk_paned_set_wide_handle (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_start_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_shrink_end_child (GTK_PANED (paned), FALSE);
  gtk_paned_set_resize_start_child (GTK_PANED (paned), TRUE);
  gtk_paned_set_resize_end_child (GTK_PANED (paned), TRUE);
  g_signal_connect (paned, "notify::position",
                    G_CALLBACK (epimone_paned_position_cb), NULL);
  return paned;
}

void
epimone_page_split (EpimonePage *self, GtkOrientation orientation)
{
  GtkWidget *old_leaf;
  GtkWidget *parent;
  GtkWidget *paned;
  GtkWidget *new_leaf;
  VteTerminal *new_term;
  g_autofree char *cwd = NULL;
  int size;

  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused == NULL)
    return;

  epimone_page_unzoom (self);   /* auto-unzoom before restructuring */

  /* Operate on the leaf that lives in the tree. */
  old_leaf = epimone_terminal_leaf (self->focused);
  parent = gtk_widget_get_parent (old_leaf);

  cwd = epimone_page_dup_cwd_for_spawn (self);
  new_leaf = epimone_page_create_terminal (self, cwd);

  paned = epimone_page_new_paned (orientation);

  if (orientation == GTK_ORIENTATION_HORIZONTAL)
    size = gtk_widget_get_width (old_leaf);
  else
    size = gtk_widget_get_height (old_leaf);

  g_object_ref (old_leaf);
  if (GTK_IS_PANED (parent))
    {
      if (gtk_paned_get_start_child (GTK_PANED (parent)) == old_leaf)
        gtk_paned_set_start_child (GTK_PANED (parent), paned);
      else
        gtk_paned_set_end_child (GTK_PANED (parent), paned);
    }
  else
    {
      adw_bin_set_child (ADW_BIN (self), paned);
    }
  gtk_paned_set_start_child (GTK_PANED (paned), old_leaf);
  gtk_paned_set_end_child (GTK_PANED (paned), new_leaf);
  g_object_unref (old_leaf);

  if (size > 0)
    gtk_paned_set_position (GTK_PANED (paned), size / 2);

  new_term = epimone_leaf_terminal (new_leaf);
  if (new_term != NULL)
    {
      gtk_widget_grab_focus (GTK_WIDGET (new_term));
      epimone_page_set_focused (self, new_term);
    }
  epimone_layout_schedule_save ();
}

/* Remove the pane holding `terminal` from the tree, collapsing its parent
 * paned so the sibling takes over. The caller decides session policy (detach
 * vs kill) beforehand; destroying the leaf here closes its data socket (a
 * clean detach). `terminal` is the pane's VteTerminal; the tree node is its
 * enclosing leaf overlay. */
static void
epimone_page_remove_terminal (EpimonePage *self, GtkWidget *terminal)
{
  GtkWidget *leaf;
  GtkWidget *parent;
  gboolean was_focused;

  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (terminal == NULL || !VTE_IS_TERMINAL (terminal))
    return;

  epimone_page_unzoom (self);   /* auto-unzoom, then close and heal */

  leaf = epimone_terminal_leaf (VTE_TERMINAL (terminal));
  parent = gtk_widget_get_parent (leaf);
  was_focused = (GTK_WIDGET (self->focused) == terminal);

  if (!GTK_IS_PANED (parent))
    {
      /* Last pane of the page. */
      self->focused = NULL;
      g_signal_emit (self, signals[SIGNAL_CLOSE_PAGE], 0);
      return;
    }

  {
    GtkPaned *paned = GTK_PANED (parent);
    gboolean leaf_is_start = (gtk_paned_get_start_child (paned) == leaf);
    GtkWidget *sibling = leaf_is_start ? gtk_paned_get_end_child (paned)
                                       : gtk_paned_get_start_child (paned);
    GtkWidget *grandparent = gtk_widget_get_parent (parent);
    VteTerminal *next_focus = was_focused ? epimone_page_first_terminal (sibling)
                                          : NULL;

    /* Emptying a GtkPaned that still holds the focus child makes GTK log a
     * focus-bookkeeping warning. Clear the toplevel focus first, do the
     * surgery, then move focus to the survivor. */
    if (was_focused)
      {
        GtkRoot *root_win = gtk_widget_get_root (GTK_WIDGET (self));
        if (GTK_IS_WINDOW (root_win))
          gtk_window_set_focus (GTK_WINDOW (root_win), NULL);
      }

    g_object_ref (sibling);
    gtk_paned_set_start_child (paned, NULL);
    gtk_paned_set_end_child (paned, NULL);

    if (GTK_IS_PANED (grandparent))
      {
        if (gtk_paned_get_start_child (GTK_PANED (grandparent)) == parent)
          gtk_paned_set_start_child (GTK_PANED (grandparent), sibling);
        else
          gtk_paned_set_end_child (GTK_PANED (grandparent), sibling);
      }
    else
      {
        adw_bin_set_child (ADW_BIN (self), sibling);
      }
    g_object_unref (sibling);

    if (next_focus != NULL)
      {
        gtk_widget_grab_focus (GTK_WIDGET (next_focus));
        epimone_page_set_focused (self, next_focus);
      }
  }

  epimone_layout_schedule_save ();
}

void
epimone_page_close_pane (EpimonePage *self)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused == NULL)
    return;
  /* User-initiated: detach (implicit via socket close), do not kill. */
  epimone_page_remove_terminal (self, GTK_WIDGET (self->focused));
}

/* Detach the focused pane's leaf from this page's tree, healing the split
 * around it exactly as closing the pane does, but the leaf itself SURVIVES:
 * the caller receives the leaf overlay with its terminal, bridge, PTY and
 * scrollback intact, ready to be rooted in another page ("Open in New Tab").
 * The session is never detached or re-attached (the data socket lives on the
 * terminal, and the terminal never dies), so nothing replays and the shell
 * never notices the move.
 *
 * Returns a strong reference the caller owns, or NULL when this page has
 * fewer than two panes: a lone pane has nothing to move out of. */
GtkWidget *
epimone_page_take_focused_leaf (EpimonePage *self)
{
  GtkWidget *leaf;
  GtkWidget *parent;

  g_return_val_if_fail (EPIMONE_IS_PAGE (self), NULL);
  if (self->focused == NULL)
    return NULL;

  /* Zoom hides sibling subtrees by flags on the very nodes about to be
   * restructured; moving a pane under those flags would strand hidden
   * widgets. Unzoom first, the rule every restructuring path here follows
   * (split, remove, focus_direction). */
  epimone_page_unzoom (self);

  leaf = epimone_terminal_leaf (self->focused);
  parent = gtk_widget_get_parent (leaf);
  if (!GTK_IS_PANED (parent))
    return NULL;   /* single pane */

  {
    GtkPaned *paned = GTK_PANED (parent);
    gboolean leaf_is_start = (gtk_paned_get_start_child (paned) == leaf);
    GtkWidget *sibling = leaf_is_start ? gtk_paned_get_end_child (paned)
                                       : gtk_paned_get_start_child (paned);
    GtkWidget *grandparent = gtk_widget_get_parent (parent);
    VteTerminal *next_focus = epimone_page_first_terminal (sibling);
    GtkRoot *root_win = gtk_widget_get_root (GTK_WIDGET (self));

    /* Same focus-bookkeeping dance as epimone_page_remove_terminal: emptying
     * a paned that still holds the window's focus widget makes GTK log a
     * warning. The taken pane is the focused one by definition. */
    if (GTK_IS_WINDOW (root_win))
      gtk_window_set_focus (GTK_WINDOW (root_win), NULL);

    g_object_ref (leaf);
    g_object_ref (sibling);
    gtk_paned_set_start_child (paned, NULL);
    gtk_paned_set_end_child (paned, NULL);

    if (GTK_IS_PANED (grandparent))
      {
        if (gtk_paned_get_start_child (GTK_PANED (grandparent)) == parent)
          gtk_paned_set_start_child (GTK_PANED (grandparent), sibling);
        else
          gtk_paned_set_end_child (GTK_PANED (grandparent), sibling);
      }
    else
      {
        adw_bin_set_child (ADW_BIN (self), sibling);
      }
    g_object_unref (sibling);

    /* The blob's focused-session field is written from self->focused on the
     * next sync; left alone it would name a session this tab no longer
     * contains. No focus grab; the caller decides where keyboard focus goes
     * (onto the moved pane in its new tab). */
    epimone_page_set_focused (self, next_focus);

    epimone_layout_schedule_save ();
    return leaf;
  }
}

/* Root @leaf, a pane taken from another page, in this page, which must be
 * empty (a fresh epimone_page_new_empty ()). Every event-driven binding
 * already resolves its page from the widget tree at event time
 * (epimone_page_for_terminal); the bridge's back pointer is the one
 * connect-time binding left, so it is repointed here. */
void
epimone_page_adopt_leaf (EpimonePage *self, GtkWidget *leaf)
{
  VteTerminal *term;
  EpiBridge *b;

  g_return_if_fail (EPIMONE_IS_PAGE (self));
  g_return_if_fail (GTK_IS_WIDGET (leaf));
  g_return_if_fail (adw_bin_get_child (ADW_BIN (self)) == NULL);

  adw_bin_set_child (ADW_BIN (self), leaf);

  term = epimone_leaf_terminal (leaf);
  b = term != NULL ? bridge_of (GTK_WIDGET (term)) : NULL;
  if (b != NULL)
    b->page = self;

  /* Sets the title from the pane's shell title. No custom name is carried
   * over: the custom name describes the SOURCE tab's identity and stays with
   * it; this tab starts on automatic, shell-provided titles. */
  if (term != NULL)
    epimone_page_set_focused (self, term);
}

void
epimone_page_focus_direction (EpimonePage *self, GtkDirectionType direction)
{
  g_autoptr (GPtrArray) terminals = NULL;
  GtkWidget *root;
  graphene_rect_t cur;
  VteTerminal *best = NULL;
  double best_primary = G_MAXDOUBLE;
  gboolean best_overlaps = FALSE;
  double cur_cx, cur_cy;

  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused == NULL)
    return;

  epimone_page_unzoom (self);   /* auto-unzoom before moving focus */

  root = adw_bin_get_child (ADW_BIN (self));
  if (root == NULL)
    return;
  if (!gtk_widget_compute_bounds (GTK_WIDGET (self->focused), GTK_WIDGET (self), &cur))
    return;

  cur_cx = cur.origin.x + cur.size.width / 2.0;
  cur_cy = cur.origin.y + cur.size.height / 2.0;

  terminals = g_ptr_array_new ();
  epimone_page_collect_terminals (root, terminals);

  for (guint i = 0; i < terminals->len; i++)
    {
      VteTerminal *candidate = g_ptr_array_index (terminals, i);
      graphene_rect_t r;
      double cx, cy, primary;
      gboolean in_direction = FALSE;
      gboolean overlaps = FALSE;

      if (candidate == self->focused)
        continue;
      if (!gtk_widget_compute_bounds (GTK_WIDGET (candidate), GTK_WIDGET (self), &r))
        continue;

      cx = r.origin.x + r.size.width / 2.0;
      cy = r.origin.y + r.size.height / 2.0;

      switch (direction)
        {
        case GTK_DIR_LEFT:
          in_direction = cx < cur_cx; primary = cur_cx - cx;
          overlaps = (r.origin.y < cur_cy) && (cur_cy < r.origin.y + r.size.height); break;
        case GTK_DIR_RIGHT:
          in_direction = cx > cur_cx; primary = cx - cur_cx;
          overlaps = (r.origin.y < cur_cy) && (cur_cy < r.origin.y + r.size.height); break;
        case GTK_DIR_UP:
          in_direction = cy < cur_cy; primary = cur_cy - cy;
          overlaps = (r.origin.x < cur_cx) && (cur_cx < r.origin.x + r.size.width); break;
        case GTK_DIR_DOWN:
          in_direction = cy > cur_cy; primary = cy - cur_cy;
          overlaps = (r.origin.x < cur_cx) && (cur_cx < r.origin.x + r.size.width); break;
        default:
          continue;
        }

      if (!in_direction)
        continue;
      if (best == NULL ||
          (overlaps && !best_overlaps) ||
          (overlaps == best_overlaps && primary < best_primary))
        {
          best = candidate;
          best_primary = primary;
          best_overlaps = overlaps;
        }
    }

  if (best != NULL)
    gtk_widget_grab_focus (GTK_WIDGET (best));
}

/* ------------------------------------------------------------------ *
 * layout accessors
 * ------------------------------------------------------------------ */

GtkWidget *
epimone_page_get_tree_root (EpimonePage *self)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), NULL);
  return adw_bin_get_child (ADW_BIN (self));
}

void
epimone_page_set_tree_root (EpimonePage *self, GtkWidget *root)
{
  VteTerminal *first;

  g_return_if_fail (EPIMONE_IS_PAGE (self));
  adw_bin_set_child (ADW_BIN (self), root);

  first = epimone_page_first_terminal (root);
  if (first != NULL)
    epimone_page_set_focused (self, first);
}

void
epimone_page_focus_session (EpimonePage *self, guint64 session_id)
{
  g_autoptr (GPtrArray) terminals = NULL;
  GtkWidget *root;

  g_return_if_fail (EPIMONE_IS_PAGE (self));
  root = adw_bin_get_child (ADW_BIN (self));
  if (root == NULL)
    return;

  terminals = g_ptr_array_new ();
  epimone_page_collect_terminals (root, terminals);
  for (guint i = 0; i < terminals->len; i++)
    {
      GtkWidget *t = g_ptr_array_index (terminals, i);
      if (epimone_terminal_session_id (t) == session_id)
        {
          gtk_widget_grab_focus (t);
          epimone_page_set_focused (self, VTE_TERMINAL (t));
          return;
        }
    }
}

guint64
epimone_page_get_focused_session (EpimonePage *self)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), 0);
  if (self->focused == NULL)
    return 0;
  return epimone_terminal_session_id (GTK_WIDGET (self->focused));
}

void
epimone_page_get_focused_grid (EpimonePage *self, guint *cols, guint *rows)
{
  if (cols != NULL)
    *cols = 0;
  if (rows != NULL)
    *rows = 0;
  g_return_if_fail (EPIMONE_IS_PAGE (self));
  if (self->focused == NULL)
    return;
  if (cols != NULL)
    *cols = (guint) vte_terminal_get_column_count (self->focused);
  if (rows != NULL)
    *rows = (guint) vte_terminal_get_row_count (self->focused);
}

gboolean
epimone_page_has_session (EpimonePage *self, guint64 session_id)
{
  g_autoptr (GPtrArray) terminals = NULL;
  GtkWidget *root;

  g_return_val_if_fail (EPIMONE_IS_PAGE (self), FALSE);
  if (session_id == 0)
    return FALSE;

  root = adw_bin_get_child (ADW_BIN (self));
  if (root == NULL)
    return FALSE;

  terminals = g_ptr_array_new ();
  epimone_page_collect_terminals (root, terminals);
  for (guint i = 0; i < terminals->len; i++)
    if (epimone_terminal_session_id (g_ptr_array_index (terminals, i)) == session_id)
      return TRUE;
  return FALSE;
}

void
epimone_page_set_initial_title (EpimonePage *self, const char *title)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));

  if (title == NULL || title[0] == '\0')
    return;
  if (g_strcmp0 (self->title, title) == 0)
    return;

  g_free (self->title);
  self->title = g_strdup (title);
  g_object_notify_by_pspec (G_OBJECT (self), properties[PROP_TITLE]);
}

void
epimone_page_set_custom_title (EpimonePage *self, const char *title)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));

  if (title != NULL && title[0] == '\0')
    title = NULL;
  if (g_strcmp0 (self->custom_title, title) == 0)
    return;

  g_free (self->custom_title);
  self->custom_title = g_strdup (title);

  /* Recompute the display title under the new precedence. update_title only
   * schedules a blob write when the visible string changes, and the custom
   * name must persist even when it happens to read the same as the shell
   * title, hence the explicit schedule. */
  epimone_page_update_title (self);
  epimone_layout_schedule_group_save ();
}

const char *
epimone_page_get_custom_title (EpimonePage *self)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), NULL);
  return self->custom_title;
}

const char *
epimone_page_get_shell_title (EpimonePage *self)
{
  const char *base = NULL;

  g_return_val_if_fail (EPIMONE_IS_PAGE (self), NULL);

  if (self->focused != NULL)
    {
      G_GNUC_BEGIN_IGNORE_DEPRECATIONS
      base = vte_terminal_get_window_title (self->focused);
      G_GNUC_END_IGNORE_DEPRECATIONS
    }
  return (base != NULL && base[0] != '\0') ? base : NULL;
}

/* ------------------------------------------------------------------ *
 * group bookkeeping (state only; the policy lives in epimone-layout.c)
 * ------------------------------------------------------------------ */

guint64
epimone_page_get_group_id (EpimonePage *self)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), 0);
  return self->group_id;
}

void
epimone_page_set_group_id (EpimonePage *self, guint64 group_id)
{
  g_return_if_fail (EPIMONE_IS_PAGE (self));

  if (self->group_id == group_id)
    return;

  /* A different group means the old enrollments say nothing about the new one. */
  self->group_id = group_id;
  g_clear_pointer (&self->enrolled, g_hash_table_unref);
}

gboolean
epimone_page_session_enrolled (EpimonePage *self, guint64 session_id)
{
  g_return_val_if_fail (EPIMONE_IS_PAGE (self), FALSE);

  if (self->enrolled == NULL)
    return FALSE;
  return g_hash_table_contains (self->enrolled, &session_id);
}

void
epimone_page_mark_enrolled (EpimonePage *self, guint64 session_id)
{
  guint64 *key;

  g_return_if_fail (EPIMONE_IS_PAGE (self));

  if (self->enrolled == NULL)
    self->enrolled = g_hash_table_new_full (g_int64_hash, g_int64_equal,
                                            g_free, NULL);
  if (g_hash_table_contains (self->enrolled, &session_id))
    return;

  key = g_new (guint64, 1);
  *key = session_id;
  g_hash_table_add (self->enrolled, key);
}

/* ------------------------------------------------------------------ *
 * GObject boilerplate
 * ------------------------------------------------------------------ */

static void
epimone_page_get_property (GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
  EpimonePage *self = EPIMONE_PAGE (object);
  switch (prop_id)
    {
    case PROP_TITLE:
      g_value_set_string (value, self->title);
      break;
    case PROP_SUBTITLE:
      g_value_set_string (value, self->subtitle);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
epimone_page_dispose (GObject *object)
{
  EpimonePage *self = EPIMONE_PAGE (object);

  epimone_all_pages = g_slist_remove (epimone_all_pages, self);

  if (self->popover != NULL)
    {
      gtk_widget_unparent (self->popover);
      self->popover = NULL;
    }
  G_OBJECT_CLASS (epimone_page_parent_class)->dispose (object);
}

static void
epimone_page_finalize (GObject *object)
{
  EpimonePage *self = EPIMONE_PAGE (object);
  g_clear_pointer (&self->title, g_free);
  g_clear_pointer (&self->subtitle, g_free);
  g_clear_pointer (&self->custom_title, g_free);
  g_clear_pointer (&self->enrolled, g_hash_table_unref);
  G_OBJECT_CLASS (epimone_page_parent_class)->finalize (object);
}

static void
epimone_page_init (EpimonePage *self)
{
  self->title = g_strdup ("Terminal");

  /* Join the live-page registry so the dim-inactive setter and palette-change
   * invalidation can reach this page (removed again in dispose). */
  epimone_all_pages = g_slist_prepend (epimone_all_pages, self);

  /* Passive primary-button tracker for divider-drag winsize coalescing. Capture
   * phase + always-propagate = observe only; it never claims a sequence, so
   * GtkPaned's handle drag and terminal selection are untouched. Owned by the
   * widget; torn down automatically with it. */
  {
    GtkEventController *btn = gtk_event_controller_legacy_new ();
    gtk_event_controller_set_propagation_phase (btn, GTK_PHASE_CAPTURE);
    g_signal_connect (btn, "event",
                      G_CALLBACK (epimone_page_root_event_cb), self);
    gtk_widget_add_controller (GTK_WIDGET (self), btn);
  }

  self->popover = gtk_popover_menu_new_from_model (epimone_page_get_menu_model ());
  /* GTK renders the items' accelerator labels Shift-first; rewrite them into
   * the conventional order ON MAP, not at construction: the popover is built
   * while this page is still unparented, and when the page is adopted into a
   * window the "win." actions resolve and GtkPopoverMenu re-renders every
   * item, clobbering any construction-time fix. Map runs after all of that
   * and before the first visible frame, every time the menu opens, so it
   * also self-heals after a rebind re-render. */
  g_signal_connect (self->popover, "map",
                    G_CALLBACK (epimone_shortcuts_fix_accel_labels_cb), NULL);
  gtk_popover_set_has_arrow (GTK_POPOVER (self->popover), FALSE);
  gtk_widget_set_halign (self->popover, GTK_ALIGN_START);
  gtk_widget_set_parent (self->popover, GTK_WIDGET (self));
  /* Closing the menu must return focus to the pane it was opened on; GTK's own
   * fallback would strand it on the first focusable widget in the window (see
   * epimone_page_focus_terminal). Covers every way the menu goes away:
   * Escape, clicking outside, and activating an item. */
  g_signal_connect_swapped (self->popover, "closed",
                            G_CALLBACK (epimone_page_focus_terminal), self);
  /* And un-grey Open in New Tab, whatever state the popup left it in. */
  g_signal_connect (self->popover, "closed",
                    G_CALLBACK (epimone_page_menu_closed_cb), self);
}

static void
epimone_page_class_init (EpimonePageClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->get_property = epimone_page_get_property;
  object_class->dispose = epimone_page_dispose;
  object_class->finalize = epimone_page_finalize;

  /* Copy/paste/select-all are invoked from the window's win.* actions (see
   * epimone_page_copy/paste/select_all) so the app accelerators actually
   * reach them; they are no longer page widget-class actions. */

  properties[PROP_TITLE] =
    g_param_spec_string ("title", NULL, NULL, "Terminal",
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  /* NULL default: no directory reported yet, which the header renders as an
   * empty subtitle rather than a placeholder line. */
  properties[PROP_SUBTITLE] =
    g_param_spec_string ("subtitle", NULL, NULL, NULL,
                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS);
  g_object_class_install_properties (object_class, N_PROPS, properties);

  signals[SIGNAL_CLOSE_PAGE] =
    g_signal_new ("close-page", G_TYPE_FROM_CLASS (klass),
                  G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

GtkWidget *
epimone_page_new_empty (void)
{
  return g_object_new (EPIMONE_TYPE_PAGE, NULL);
}

GtkWidget *
epimone_page_new_with_cwd (const char *cwd)
{
  EpimonePage *self = g_object_new (EPIMONE_TYPE_PAGE, NULL);
  GtkWidget *leaf = epimone_page_create_terminal (self, cwd);
  VteTerminal *term = epimone_leaf_terminal (leaf);

  adw_bin_set_child (ADW_BIN (self), leaf);
  if (term != NULL)
    epimone_page_set_focused (self, term);
  return GTK_WIDGET (self);
}

