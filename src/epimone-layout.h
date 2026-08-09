#ifndef EPIMONE_LAYOUT_H
#define EPIMONE_LAYOUT_H

#include <adwaita.h>

G_BEGIN_DECLS

/* Forward declaration rather than including epimone-window.h: that header pulls
 * in epimone-page.h, whose .c includes this one. Identical to the typedef
 * G_DECLARE_FINAL_TYPE emits there, which C11 permits to be repeated. */
typedef struct _EpimoneWindow EpimoneWindow;

/* Persisted window/tab/split layout, at $XDG_STATE_HOME/epimone/layout.json.
 *
 * Serialization uses GLib's GVariant text format (json-glib is not available
 * here; GVariant gives robust nested (recursive) serialization with no
 * hand-rolled parser). The top-level type is:
 *
 *   (u a(i a(t v)))
 *    |  |  |  | +-- v: a split-tree node (boxed variant), one of:
 *    |  |  |  |        leaf  "(st)"     = ("leaf", session_id)
 *    |  |  |  |        split "(ssdvv)"  = ("split", "h"|"v", ratio, start, end)
 *    |  |  |  +----- t: focused session id in this tab (0 = none)
 *    |  |  +-------- a(tv): the tabs of one window
 *    |  +----------- i: active tab index in this window
 *    +-------------- u: schema version
 */

/* ------------------------------------------------------------------ *
 * Daemon-side group state.
 *
 * A tab is a group in the daemon. The group owns that tab's sessions and holds
 * its arrangement as an opaque blob, so the arrangement survives the tab being
 * closed: closing a tab only detaches, and layout.json is a re-serialization of
 * live widget state, so it cannot describe a tab that no longer has widgets.
 *
 * The blob payload is GVariant text, type:
 *
 *   (u t s v q q s)
 *    | | | |  \ /  +-- s: custom (user-chosen) tab name, "" = none (v3+)
 *    | | | |   +----- q q: focused pane's grid cols, rows (v2+)
 *    | | | +-------- v: split-tree node, the SAME node format layout.json uses
 *    | | +---------- s: shell-provided tab title (layout.json does not record this)
 *    | +------------ t: focused session id within the tab (0 = none)
 *    +-------------- u: blob payload version
 *
 * The daemon never parses any of this; all GVariant work is GUI-side.
 * ------------------------------------------------------------------ */

/* Queue a debounced save of the current layout of all open windows. This marks
 * BOTH layout.json and the daemon-side group blobs as needing a write. */
void     epimone_layout_schedule_save (void);

/* Queue a debounced write of the daemon-side group blobs ONLY, sharing the same
 * timer and debounce interval as epimone_layout_schedule_save().
 *
 * Used by the triggers that matter to a tab's arrangement but that layout.json
 * has never recorded (divider drags, tab title changes, unzoom). Routing them
 * here leaves layout.json's write pattern unchanged while still capturing
 * them in the blob. */
void     epimone_layout_schedule_group_save (void);

/* Read the presentable parts of a group blob: the tab title, the focused session
 * id, and the focused pane's grid size. Returns FALSE if the blob is absent,
 * unparseable, or of an unknown version. *out_title is newly allocated; the grid
 * is 0x0 when the blob predates it (version 1).
 *
 * Exists so that callers wanting to describe or draw a group (the overview) do
 * not have to know the blob's encoding. Reading and writing it stay in one file. */
gboolean epimone_layout_blob_peek (const guint8 *blob,
                                   gsize         len,
                                   char        **out_title,
                                   guint64      *out_focused,
                                   guint        *out_cols,
                                   guint        *out_rows);

/* The custom (user-chosen) tab name recorded in a group blob, or NULL when the
 * group has none (or the blob predates the field / will not parse). Newly
 * allocated; caller frees. Kept separate from blob_peek so the shell title and
 * the name that overrides it stay individually readable; the rename dialog
 * needs both. */
char    *epimone_layout_blob_dup_custom_title (const guint8 *blob, gsize len);

/* Give group @group_id a custom tab name, or clear it (@name NULL or "").
 *
 * An attached group is renamed through its live page, so the tab and window
 * retitle immediately and the ordinary debounced sync persists it; a detached
 * group's stored blob is rewritten in place and pushed to the daemon at once.
 * Returns FALSE with @error set if the group does not exist, its blob will not
 * parse, or the daemon refuses the write. */
gboolean epimone_layout_rename_group (guint64      group_id,
                                      const char  *name,
                                      GError     **error);

/* One pane of a tab, as the overview needs it: which session it shows and where
 * it sits, as a fraction of the whole tab (0..1). Grid size is derived rather
 * than stored: the blob records only the FOCUSED pane's grid, so the tab's
 * total grid is recovered from that pane's grid divided by its fractional size,
 * and each pane's grid follows from its own fraction. */
typedef struct {
  guint64 session;
  double  x, y, w, h;
  guint   cols, rows;
} EpimoneLayoutLeaf;

/* A divider between two panes, in the same fractional space. `vertical` means
 * the seam itself runs top-to-bottom, i.e. it came from a left/right split. */
typedef struct {
  double   x, y, w, h;
  gboolean vertical;
} EpimoneLayoutSeam;

/* Flatten a group blob's split tree into panes and dividers, so a caller can
 * draw the whole tab's arrangement without knowing the blob's encoding;
 * reading and writing it stay in this one file, as blob_peek's note says.
 *
 * @out_leaves is an array of EpimoneLayoutLeaf and @out_seams of
 * EpimoneLayoutSeam; both are newly allocated with a free func set, and either
 * may be NULL if not wanted. Leaves whose session id is 0 (a pruned pane) are
 * skipped, and their space is given to the sibling, matching what restore does.
 * Returns FALSE, setting nothing, if the blob will not parse. */
gboolean epimone_layout_blob_geometry (const guint8 *blob,
                                       gsize         len,
                                       GPtrArray   **out_leaves,
                                       GPtrArray   **out_seams);

/* Rebuild a closed tab from its daemon group and add it to @win as a new tab:
 * deserialize the blob, reattach each surviving pane's session, restore the split
 * ratios, the tab title and the focused pane.
 *
 * Refuses (returning FALSE with @error set) when the blob will not parse, its
 * version is unknown, every pane's session is gone, or the daemon's instance id
 * does not match the recorded one. Succeeds without building anything if the
 * group is already on screen, in which case its existing tab is selected instead.
 *
 * The rebuilt tab keeps the SAME group id, so the ordinary debounced sync goes on
 * updating that group rather than creating a duplicate. */
gboolean epimone_layout_restore_group (EpimoneWindow *win,
                                       guint64        group_id,
                                       GError       **error);

/* Read the recorded daemon instance id and compare it with the running daemon's,
 * logging the outcome. Nothing acts on the result yet: session and group ids
 * restart at 1 after a daemon restart, and this is the check a future change
 * can use to reject stale references. Call once at startup. */
void     epimone_layout_check_instance (void);

/* Write the layout of all currently open windows now (synchronous). */
void     epimone_layout_save_now (void);

/* Rebuild windows/tabs/splits from the layout file, attaching each surviving
 * leaf to its saved session. Returns TRUE if at least one window was created. */
gboolean epimone_layout_restore (AdwApplication *app);

/* Persist once and freeze further saves (called as windows begin to close). */
void     epimone_layout_begin_shutdown (void);

G_END_DECLS

#endif /* EPIMONE_LAYOUT_H */
