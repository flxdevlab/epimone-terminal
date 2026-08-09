#ifndef EPIMONE_CLIENT_H
#define EPIMONE_CLIENT_H

#include <glib.h>

G_BEGIN_DECLS

/* One entry returned by epimone_client_list_sessions(). */
typedef struct {
  guint64  id;
  guint    pid;
  gboolean alive;
  gboolean attached;
  gint64   created_at;
  char    *cwd;      /* owned */
} EpiSessionInfo;

void epimone_session_info_free (EpiSessionInfo *info);

/* PEEK a session's scrollback tail. Read-only and side-effect free: it does not
 * attach, and it leaves an attached client alone. Returns the bytes (caller frees)
 * or NULL; *out_len is how many. *out_total, when non-NULL, receives the total
 * bytes the ring holds, so len < total means there is more scrollback above the
 * tail; 0 means the daemon does not report it. */
guint8 *epimone_client_peek_session (guint64   id,
                                     guint32   max_bytes,
                                     gsize    *out_len,
                                     guint64  *out_total,
                                     GError  **error);

/* The command currently in the foreground of a session, e.g. "hashcat" rather
 * than "session 14". @pid is the session's child pid as reported by LIST.
 *
 * Read entirely from /proc, so no protocol addition is needed: the kernel records
 * the controlling terminal's foreground process group on the child itself, and the
 * group leader's pid equals that group id. Falls back to the child's own name
 * (normally the shell) when nothing else is in the foreground. Returns NULL if the
 * process is gone. Caller frees. */
char *epimone_client_foreground_command (guint pid);

/* Absolute path of the shell used when the 'shell-path' setting is empty
 * ("System default"): $SHELL, else the passwd entry's shell, else /bin/sh.
 * Never returns NULL. Caller frees. */
char *epimone_client_default_shell (void);

/* Absolute path to the daemon control socket. Matches the daemon's own path
 * logic exactly ($XDG_RUNTIME_DIR/epimone or /tmp/epimone-$UID). Caller frees. */
char *epimone_client_socket_path (void);

/* Ensure a daemon is answering on the control socket, spawning
 * `epimone-daemon --daemonize` if necessary and waiting (~2s) for it to come
 * up. Returns TRUE once the socket answers. */
gboolean epimone_client_ensure_daemon (GError **error);

/* CREATE a session running $SHELL in `cwd` (NULL => daemon default). Returns
 * the new session id, or 0 on failure. */
guint64 epimone_client_create_session (const char *cwd, GError **error);

/* LIST all sessions. Returns a GPtrArray of EpiSessionInfo* (free with
 * g_ptr_array_unref; element free func is set), or NULL on error. */
GPtrArray *epimone_client_list_sessions (GError **error);

/* ATTACH to a session. On success returns a connected, non-blocking data
 * channel fd (the ATTACHED reply has been consumed; the ring replay and live
 * stream follow as raw bytes). Returns -1 on failure. Caller owns the fd. */
int epimone_client_attach_session (guint64 id, GError **error);

/* Feed the configured 'launch-command' into a session as if the user had typed
 * it: the command text plus Return, written to the attached data channel `fd`.
 *
 * Deliberately not run as `shell -c <command>`. Sent as input, the session
 * stays an ordinary interactive shell, the text is visible and editable before
 * Return takes effect, the command can be interrupted, and shell integration
 * is unaffected.
 *
 * Does nothing when the key is empty. Call only for a session the GUI has just
 * created: re-attaching to a session that already exists must not re-run it. */
void epimone_client_send_launch_command (int fd);

/* ------------------------------------------------------------------ *
 * Groups.
 *
 * A group is a set of sessions plus one opaque blob, which is where the GUI
 * keeps a tab's arrangement so it outlives the widgets (closing a tab only
 * detaches). The daemon stores the bytes and never interprets them; building and
 * parsing the blob is entirely the GUI's business (see epimone-layout.c).
 *
 * These take raw bytes deliberately: nothing here knows what a blob contains.
 * ------------------------------------------------------------------ */

/* One entry returned by epimone_client_list_groups(). */
typedef struct {
  guint64  id;
  gint64   created_at;
  guint8  *blob;       /* owned; NULL when blob_len == 0 */
  gsize    blob_len;
  GArray  *members;    /* owned; guint64 session ids */
} EpiGroupInfo;

void epimone_group_info_free (EpiGroupInfo *info);

/* LIST all groups. Returns a GPtrArray of EpiGroupInfo* (element free func set),
 * or NULL on error. When non-NULL, *out_instance_id receives the daemon's
 * instance id (0 if it does not report one). */
GPtrArray *epimone_client_list_groups (guint64 *out_instance_id, GError **error);

/* Create a group holding `blob`. Returns the new group id, or 0 on failure. */
guint64 epimone_client_group_new (const guint8 *blob, gsize len, GError **error);

/* Replace a group's blob. Fails if the group is gone (the daemon destroys a
 * group once it loses its last member session). */
gboolean epimone_client_group_set (guint64       gid,
                                   const guint8 *blob,
                                   gsize         len,
                                   GError      **error);

/* Assign a session to a group. This is an atomic move: it clears whatever group
 * the session was in before, so it is the correct call even when a session is
 * changing groups rather than joining its first. */
gboolean epimone_client_group_add (guint64 gid, guint64 sid, GError **error);

/* Take a session out of whatever group it is in. */
gboolean epimone_client_group_remove (guint64 sid, GError **error);

/* The daemon's instance id, or 0 if it does not report one (a daemon predating
 * group support). Session and group ids restart at 1 on every daemon start, so
 * this is what makes a stale id reference detectable. */
guint64 epimone_client_instance_id (GError **error);

gboolean epimone_client_kill_session   (guint64 id, GError **error);
gboolean epimone_client_resize_session (guint64 id, guint rows, guint cols,
                                        GError **error);
gboolean epimone_client_detach_session (guint64 id, GError **error);

G_END_DECLS

#endif /* EPIMONE_CLIENT_H */
