#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

#define EPIMONE_TYPE_PAGE (epimone_page_get_type())
G_DECLARE_FINAL_TYPE (EpimonePage, epimone_page, EPIMONE, PAGE, AdwBin)

/* A fresh page: creates one new daemon session in @cwd (NULL = daemon default)
 * and attaches to it. The new-tab path passes the active pane's directory so a
 * new tab inherits it. */
GtkWidget  *epimone_page_new_with_cwd    (const char       *cwd);

/* The working directory of the focused pane, resolved from VTE's
 * current-directory-uri (NULL if unavailable). Caller frees. */
char       *epimone_page_dup_cwd         (EpimonePage      *self);

/* Like epimone_page_dup_cwd(), but filtered through the preserve-directory
 * policy: this is the cwd a NEW tab or split should start in (NULL = daemon
 * default). Kept separate from epimone_page_dup_cwd() so the header/subtitle,
 * which shows the true directory, is never affected by the policy. */
char       *epimone_page_dup_cwd_for_spawn (EpimonePage    *self);

/* An empty page with no terminal, used to rebuild a saved split tree. */
GtkWidget  *epimone_page_new_empty       (void);

/* Number of terminal panes in this page; 1 when it has not been split. */
guint       epimone_page_get_pane_count  (EpimonePage      *self);
void        epimone_page_split           (EpimonePage      *self,
                                          GtkOrientation    orientation);
void        epimone_page_close_pane      (EpimonePage      *self);

/* Move-a-pane-out support ("Open in New Tab"): take the focused pane's leaf out
 * of this page's split tree, healing the tree, WITHOUT destroying the leaf;
 * bridge and scrollback stay intact. Returns a strong reference, or NULL when
 * the page has fewer than two panes. The counterpart roots that leaf in a
 * fresh, empty page and rebinds the terminal's page-tracking to it. */
GtkWidget  *epimone_page_take_focused_leaf (EpimonePage    *self);
void        epimone_page_adopt_leaf      (EpimonePage      *self,
                                          GtkWidget        *leaf);
void        epimone_page_focus_direction (EpimonePage      *self,
                                          GtkDirectionType  direction);

/* Put the keyboard focus back on this page's active pane. Called from every
 * place where GTK would otherwise leave focus stranded outside the terminal
 * (popover dismissal, tab switch, new tab). */
void        epimone_page_focus_terminal  (EpimonePage      *self);

/* Toggle zoom of the focused pane (fills the tab, or restores the layout). */
void        epimone_page_toggle_zoom     (EpimonePage      *self);

/* Whether a pane is currently zoomed. Zoom hides the focused pane's siblings
 * rather than restructuring the tree, so while it is on, every enclosing
 * GtkPaned reports a position that reflects the zoom and not the user's split
 * ratios; anything serializing ratios has to sit the zoom out. */
gboolean    epimone_page_is_zoomed       (EpimonePage      *self);

/* Clipboard / selection on the focused pane's inner VteTerminal. Invoked from
 * the window's win.copy/win.paste/win.select-all actions. */
void        epimone_page_copy            (EpimonePage      *self);
void        epimone_page_paste           (EpimonePage      *self);
void        epimone_page_select_all      (EpimonePage      *self);
void        epimone_page_select_none     (EpimonePage      *self);

/* Read-only latch and terminal reset, both scoped to the FOCUSED pane. The
 * latch is deliberately not persisted in the group blob: a restored pane that
 * silently rejected input would read as broken. */
gboolean    epimone_page_get_read_only   (EpimonePage      *self);
void        epimone_page_set_read_only   (EpimonePage      *self,
                                          gboolean          read_only);
void        epimone_page_reset_terminal  (EpimonePage      *self,
                                          gboolean          clear_history);

/* Layout save/restore support. */
GtkWidget  *epimone_page_create_terminal_for_session (EpimonePage *self,
                                                      guint64      session_id);
void        epimone_page_set_tree_root   (EpimonePage      *self,
                                          GtkWidget        *root);
GtkWidget  *epimone_page_get_tree_root   (EpimonePage      *self);
void        epimone_page_focus_session   (EpimonePage      *self,
                                          guint64           session_id);
guint64     epimone_page_get_focused_session (EpimonePage  *self);

/* A GtkPaned configured the way splits expect (used when rebuilding trees). */
GtkWidget  *epimone_page_new_paned       (GtkOrientation    orientation);

/* ------------------------------------------------------------------ *
 * Group bookkeeping. A tab is a group in the daemon: the group owns this tab's
 * sessions and holds its arrangement as an opaque blob, so the arrangement
 * survives the tab being closed. Managed entirely from epimone-layout.c; the
 * page just carries the ids.
 * ------------------------------------------------------------------ */

/* The daemon group backing this tab, or 0 before one has been created. */
guint64     epimone_page_get_group_id    (EpimonePage      *self);
void        epimone_page_set_group_id    (EpimonePage      *self,
                                          guint64           group_id);

/* Whether `session_id` has already been added to this page's group, so the
 * steady-state sync does not re-send GROUP_ADD for panes it has already
 * enrolled. Marking is separate from asking so the caller only records a
 * session once the daemon has accepted it. */
gboolean    epimone_page_session_enrolled (EpimonePage     *self,
                                           guint64          session_id);
void        epimone_page_mark_enrolled    (EpimonePage     *self,
                                           guint64          session_id);

/* The daemon session id bound to a terminal widget (0 if none). */
guint64     epimone_terminal_session_id  (GtkWidget        *terminal);

/* The focused pane's grid in character cells, for sizing a thumbnail honestly
 * rather than assuming 80x24. Both out params are set to 0 if unknown. */
void        epimone_page_get_focused_grid (EpimonePage    *self,
                                           guint          *cols,
                                           guint          *rows);

/* Whether any pane in this page is showing @session_id. */
gboolean    epimone_page_has_session     (EpimonePage      *self,
                                          guint64           session_id);

/* Seed the tab title from stored state when rebuilding a tab, before its
 * terminals have reported one of their own. Whatever the panes go on to emit
 * (OSC 0/2) replaces it, so this only fills the gap. */
void        epimone_page_set_initial_title (EpimonePage    *self,
                                            const char     *title);

/* A user-chosen tab name. While set it takes precedence over the shell's
 * OSC 0/2 titles (the tab stops following the shell entirely); NULL or ""
 * clears it, returning the tab to automatic titles. Persisted in the group
 * blob by the ordinary debounced sync. get returns NULL when automatic. */
void        epimone_page_set_custom_title  (EpimonePage    *self,
                                            const char     *title);
const char *epimone_page_get_custom_title  (EpimonePage    *self);

/* The focused pane's shell-provided (OSC 0/2) title, ignoring any custom name;
 * NULL when the shell has not reported one. This is what the group blob's
 * title field records, so that clearing a custom name, even on a detached
 * group, falls back to the last title the shell actually set. Borrowed. */
const char *epimone_page_get_shell_title   (EpimonePage    *self);

/* ------------------------------------------------------------------ *
 * Appearance: plain-C setters the settings window / startup call. They apply
 * live to every open terminal and are remembered so terminals created later
 * inherit the current state. (No Adw here; the core stays Adw-free.)
 * ------------------------------------------------------------------ */

/* Cursor shapes, mirrored to VteCursorShape inside the .c. */
typedef enum
{
  EPIMONE_CURSOR_BLOCK,
  EPIMONE_CURSOR_IBEAM,
  EPIMONE_CURSOR_UNDERLINE
} EpimoneCursorShape;

/* Backspace/Delete key erase bindings. Values mirror VteEraseBinding 1:1, so
 * the .c casts straight across; the enum lives here to keep this header VTE-free
 * like EpimoneCursorShape. */
typedef enum
{
  EPIMONE_ERASE_AUTO,
  EPIMONE_ERASE_ASCII_BACKSPACE,
  EPIMONE_ERASE_ASCII_DELETE,
  EPIMONE_ERASE_DELETE_SEQUENCE,
  EPIMONE_ERASE_TTY
} EpimoneEraseBinding;

/* Whether a new tab/split inherits the current pane's OSC 7 working directory:
 * NEVER (always the daemon default), SAFE (inherit only when the reported
 * directory is on the local host), ALWAYS (inherit unconditionally). Order
 * matches the Advanced page's combo. */
typedef enum
{
  EPIMONE_PRESERVE_NEVER,
  EPIMONE_PRESERVE_SAFE,
  EPIMONE_PRESERVE_ALWAYS
} EpimonePreserveDirectory;

/* Foreground/background/cursor + the 16 ANSI colors. @palette_len is normally
 * 16; values beyond 16 are ignored. */
void epimone_terminals_set_colors (const GdkRGBA *background,
                                   const GdkRGBA *foreground,
                                   const GdkRGBA *cursor,
                                   const GdkRGBA *palette,
                                   gsize          palette_len);

/* Pango font description string; NULL or "" means the system monospace font. */
void epimone_terminals_set_font (const char *font_name);

void epimone_terminals_set_cursor_shape (EpimoneCursorShape shape);

/* Backspace/Delete erase bindings and CJK ambiguous-cell width. Like the
 * appearance setters, each stores the value and applies it live to every open
 * terminal, so terminals built later inherit it too. Width is VTE's raw 1
 * (narrow) or 2 (wide); values outside that clamp to narrow. */
void epimone_terminals_set_backspace_binding   (EpimoneEraseBinding binding);
void epimone_terminals_set_delete_binding      (EpimoneEraseBinding binding);
void epimone_terminals_set_cjk_ambiguous_width (int width);

/* The working-directory-inheritance policy new tabs and splits obey. Stored
 * globally and read at spawn time by epimone_page_dup_cwd_for_spawn(); does not
 * touch open terminals. */
void epimone_terminals_set_preserve_directory (EpimonePreserveDirectory policy);

/* Lines of scrollback to keep. Negative means unlimited (VTE's convention);
 * 0 disables scrollback entirely. */
void epimone_terminals_set_scrollback_lines (glong lines);

/* Terminal behaviour toggles, each mapping straight onto the VTE property of
 * the same name. Like the appearance setters, these apply to every open
 * terminal at once and are inherited by any created later. */
void epimone_terminals_set_audible_bell        (gboolean enabled);
void epimone_terminals_set_scroll_on_output    (gboolean enabled);
void epimone_terminals_set_scroll_on_keystroke (gboolean enabled);

/* Grid size (character cells) a NEWLY created terminal requests. Unlike the
 * setters above this does not touch open terminals; it only affects terminals
 * built afterwards, so in practice it sizes the next new window. */
void epimone_terminals_set_default_size (int columns, int rows);

/* Dim the panes of a split tab that do not hold focus. The fade is painted in
 * the terminal's own background colour over each unfocused leaf, so only the
 * ink dims; a single-pane tab is exempt. Stored globally and applied live to
 * every open page, so pages built later inherit the current state. */
void epimone_pages_set_dim_inactive (gboolean enabled);

/* Temporarily force all inactive-pane dimming off (TRUE) and restore it (FALSE),
 * so the overview's synchronous live-widget capture does not bake dimming into a
 * thumbnail. Must be balanced within one main-loop iteration. */
void epimone_pages_suppress_dim_for_capture (gboolean suppress);

/* Even interior padding (px, clamped 0–32) between the pane edge and the text
 * grid. Applies to every terminal at once and to any created later. */
void epimone_terminals_set_padding (int px);

/* Apply the current palette, font, cursor and scrollback to one terminal that is
 * NOT part of the live registry (the overview's offscreen thumbnail renderer), so
 * its output looks like the user's terminal rather than VTE's defaults. */
void epimone_terminals_apply_appearance (GtkWidget *terminal);

/* Out-of-registry terminals get no broadcast, so anything that renders FROM one
 * (the overview's thumbnail pipeline) must be told when the rendered appearance
 * (colors or font) changes, both to re-skin its terminal and to invalidate
 * pixels it has already produced. Listeners fire after the new appearance is in
 * effect on every registered terminal. remove must be called before @user_data
 * dies; add/remove pairs are matched on both pointers. */
typedef void (*EpimoneAppearanceListener) (gpointer user_data);
void epimone_terminals_add_appearance_listener    (EpimoneAppearanceListener listener,
                                                   gpointer                  user_data);
void epimone_terminals_remove_appearance_listener (EpimoneAppearanceListener listener,
                                                   gpointer                  user_data);

/* Map the GSettings "cursor-shape" string ("block"/"ibeam"/"underline") to the
 * enum and back. from_id defaults to BLOCK for anything unrecognized. */
EpimoneCursorShape epimone_cursor_shape_from_id (const char *id);
const char        *epimone_cursor_shape_to_id   (EpimoneCursorShape shape);

/* Map the GSettings "backspace-binding"/"delete-binding" strings
 * ("auto"/"ascii-backspace"/"ascii-delete"/"delete-sequence"/"tty") to the enum
 * and back. from_id defaults to AUTO for anything unrecognized. */
EpimoneEraseBinding epimone_erase_binding_from_id (const char *id);
const char         *epimone_erase_binding_to_id   (EpimoneEraseBinding binding);

/* Map the GSettings "preserve-directory" string ("never"/"safe"/"always") to
 * the enum and back. from_id defaults to ALWAYS for anything unrecognized, so a
 * stray value keeps today's inherit-everything behaviour. */
EpimonePreserveDirectory epimone_preserve_directory_from_id (const char *id);
const char              *epimone_preserve_directory_to_id   (EpimonePreserveDirectory policy);

G_END_DECLS
