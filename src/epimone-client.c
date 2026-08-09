#include "epimone-client.h"
#include "epimone-protocol.h"

#include <gio/gio.h>   /* GSettings: the spawn path reads Shell & Profiles */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define EPIMONE_CLIENT_ERROR (epimone_client_error_quark ())

static GQuark
epimone_client_error_quark (void)
{
  return g_quark_from_static_string ("epimone-client-error");
}

char *
epimone_client_default_shell (void)
{
  const char *shell = g_getenv ("SHELL");
  struct passwd *pw;

  if (shell != NULL && shell[0] == '/')
    return g_strdup (shell);

  /* $SHELL missing or not absolute (a stripped environment, or a session
   * manager that never set it): fall back to the passwd entry, which is the
   * shell the account is actually configured with. */
  pw = getpwuid (getuid ());
  if (pw != NULL && pw->pw_shell != NULL && pw->pw_shell[0] == '/')
    return g_strdup (pw->pw_shell);

  return g_strdup ("/bin/sh");
}

void
epimone_session_info_free (EpiSessionInfo *info)
{
  if (info == NULL)
    return;
  g_free (info->cwd);
  g_free (info);
}

char *
epimone_client_socket_path (void)
{
  const char *xdg = g_getenv ("XDG_RUNTIME_DIR");

  /* Mirror the daemon's paths.c logic exactly so both sides agree. */
  if (xdg != NULL && xdg[0] == '/')
    return g_build_filename (xdg, "epimone", EPIMONE_SOCKET_NAME, NULL);

  return g_strdup_printf ("/tmp/epimone-%u/%s",
                          (unsigned) getuid (), EPIMONE_SOCKET_NAME);
}

/* ------------------------------------------------------------------ *
 * low-level blocking socket I/O
 * ------------------------------------------------------------------ */

static int
client_connect (GError **error)
{
  g_autofree char *path = epimone_client_socket_path ();
  struct sockaddr_un addr;
  int fd;

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
                   "socket: %s", g_strerror (errno));
      return -1;
    }

  memset (&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen (path) >= sizeof addr.sun_path)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "socket path too long");
      close (fd);
      return -1;
    }
  strcpy (addr.sun_path, path);

  if (connect (fd, (struct sockaddr *) &addr, sizeof addr) != 0)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
                   "connect %s: %s", path, g_strerror (errno));
      close (fd);
      return -1;
    }
  return fd;
}

static gboolean
write_all (int fd, const void *buf, size_t len)
{
  const guint8 *p = buf;
  size_t off = 0;
  while (off < len)
    {
      ssize_t n = write (fd, p + off, len - off);
      if (n > 0)
        off += (size_t) n;
      else if (n < 0 && errno == EINTR)
        continue;
      else
        return FALSE;
    }
  return TRUE;
}

static gboolean
read_all (int fd, void *buf, size_t len)
{
  guint8 *p = buf;
  size_t off = 0;
  while (off < len)
    {
      ssize_t n = read (fd, p + off, len - off);
      if (n > 0)
        off += (size_t) n;
      else if (n == 0)
        return FALSE;
      else if (errno == EINTR)
        continue;
      else
        return FALSE;
    }
  return TRUE;
}

static gboolean
send_frame (int fd, epi_buf *b, GError **error)
{
  gboolean ok;
  if (b->oom)
    {
      epi_buf_free (b);
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "out of memory");
      return FALSE;
    }
  ok = write_all (fd, b->data, b->len);
  epi_buf_free (b);
  if (!ok)
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
                 "write: %s", g_strerror (errno));
  return ok;
}

/* Read one framed reply. On success sets *type, and returns a malloc'd body
 * of *body_len bytes (may be NULL when empty). */
static gboolean
recv_frame (int fd, guint32 *type, guint8 **body, size_t *body_len,
            GError **error)
{
  guint8 hdr[4];
  guint32 flen;
  guint8 *raw;

  *body = NULL;
  *body_len = 0;

  if (!read_all (fd, hdr, 4))
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "no reply from daemon");
      return FALSE;
    }
  flen = (guint32) hdr[0] | ((guint32) hdr[1] << 8)
       | ((guint32) hdr[2] << 16) | ((guint32) hdr[3] << 24);
  if (flen < 4)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "short frame");
      return FALSE;
    }

  raw = g_malloc (flen);
  if (!read_all (fd, raw, flen))
    {
      g_free (raw);
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "truncated reply");
      return FALSE;
    }
  *type = (guint32) raw[0] | ((guint32) raw[1] << 8)
        | ((guint32) raw[2] << 16) | ((guint32) raw[3] << 24);
  *body_len = flen - 4;
  *body = g_memdup2 (raw + 4, *body_len ? *body_len : 1);
  g_free (raw);
  return TRUE;
}

/* Turn an ERROR reply body into a GError; returns TRUE if it was an error. */
static gboolean
reply_is_error (guint32 type, const guint8 *body, size_t body_len,
                GError **error)
{
  epi_rd r;
  guint32 code;
  char *msg;

  if (type != EPI_MSG_ERROR)
    return FALSE;

  epi_rd_init (&r, body, body_len);
  code = epi_rd_u32 (&r);
  msg = epi_rd_str (&r);
  g_set_error (error, EPIMONE_CLIENT_ERROR, code,
               "daemon error %u: %s", code, msg ? msg : "?");
  free (msg);
  return TRUE;
}

/* A one-shot control transaction: connect, send one request, read one reply. */
static gboolean
control_txn (epi_buf *req, guint32 *rtype, guint8 **rbody, size_t *rlen,
             GError **error)
{
  int fd = client_connect (error);
  if (fd < 0)
    {
      epi_buf_free (req);
      return FALSE;
    }
  if (!send_frame (fd, req, error))
    {
      close (fd);
      return FALSE;
    }
  if (!recv_frame (fd, rtype, rbody, rlen, error))
    {
      close (fd);
      return FALSE;
    }
  close (fd);
  return TRUE;
}

/* ------------------------------------------------------------------ *
 * daemon autostart
 * ------------------------------------------------------------------ */

static gboolean
daemon_answers (void)
{
  GError *err = NULL;
  int fd = client_connect (&err);
  if (fd < 0)
    {
      g_clear_error (&err);
      return FALSE;
    }
  close (fd);
  return TRUE;
}

/* Locate the epimone-daemon binary: prefer the one next to this executable
 * (so an uninstalled build works), else rely on PATH. */
static char *
find_daemon_binary (void)
{
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);

  if (exe != NULL)
    {
      g_autofree char *dir = g_path_get_dirname (exe);
      char *cand = g_build_filename (dir, "epimone-daemon", NULL);
      if (g_file_test (cand, G_FILE_TEST_IS_EXECUTABLE))
        return cand;
      g_free (cand);
    }

  return g_strdup ("epimone-daemon");   /* found via PATH */
}

/* Locate the shell-integration scripts: the installed
 * <prefix>/share/epimone/shell-integration, else the in-tree src dir (dev),
 * else the source path baked in at build time. Returns NULL if not found. */
static char *
find_shell_integration_dir (void)
{
  g_autofree char *exe = g_file_read_link ("/proc/self/exe", NULL);

  if (exe != NULL)
    {
      g_autofree char *bindir = g_path_get_dirname (exe);
      g_autofree char *prefix = g_path_get_dirname (bindir);
      char *cand;

      cand = g_build_filename (prefix, "share", "epimone", "shell-integration", NULL);
      if (g_file_test (cand, G_FILE_TEST_IS_DIR))
        return cand;
      g_free (cand);

      cand = g_build_filename (prefix, "src", "shell-integration", NULL);
      if (g_file_test (cand, G_FILE_TEST_IS_DIR))
        return cand;
      g_free (cand);
    }

#ifdef EPIMONE_SHELL_INTEGRATION_SRC
  if (g_file_test (EPIMONE_SHELL_INTEGRATION_SRC, G_FILE_TEST_IS_DIR))
    return g_strdup (EPIMONE_SHELL_INTEGRATION_SRC);
#endif

  return NULL;
}

gboolean
epimone_client_ensure_daemon (GError **error)
{
  g_autofree char *bin = NULL;
  char *argv[3];
  GError *spawn_err = NULL;

  if (daemon_answers ())
    return TRUE;

  bin = find_daemon_binary ();
  argv[0] = bin;
  argv[1] = (char *) "--daemonize";
  argv[2] = NULL;

  if (!g_spawn_async (NULL, argv, NULL,
                      G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL |
                      G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, NULL, &spawn_err))
    {
      g_propagate_prefixed_error (error, spawn_err,
                                  "failed to launch epimone-daemon: ");
      return FALSE;
    }

  /* Wait up to ~2s for the socket to start answering. */
  for (int i = 0; i < 80; i++)
    {
      if (daemon_answers ())
        return TRUE;
      g_usleep (25 * 1000);
    }

  g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
               "epimone-daemon did not come up within timeout");
  return FALSE;
}

/* ------------------------------------------------------------------ *
 * public API
 * ------------------------------------------------------------------ */

/* The GSettings the spawn path reads. Cached: a new session can be created on
 * every split and new tab, and the schema lookup is not free. The GUI is
 * single-threaded, so a plain static is enough. */
static GSettings *
spawn_settings (void)
{
  static GSettings *settings = NULL;

  if (settings == NULL)
    settings = g_settings_new ("org.felix.Epimone");
  return settings;
}

/* Add Epimone's integration to a shell that is about to be spawned, so it
 * emits OSC 7 (cwd, which is what makes a new tab or split inherit the current
 * directory) and OSC 133 prompt marks, without editing the user's dotfiles:
 *
 *   bash -> `bash --rcfile <dir>/epimone.bash` (+ EPIMONE_BASH_RCFILE=1); the
 *           snippet sources the user's ~/.bashrc first.
 *   zsh  -> ZDOTDIR=<dir>/zsh (a shim that sources the user's real zsh files
 *           then the snippet); EPIMONE_ZDOTDIR_REAL preserves the user's dir.
 *
 * `login` reports whether the shell is being started as a login shell, which
 * changes what the shell reads at startup and therefore whether injection can
 * work at all (see the bash branch).
 *
 * Returns TRUE if integration was injected. A FALSE return is not a failure:
 * the shell still starts, it just will not report its cwd, so panes opened
 * from it fall back to $HOME (resolved daemon-side in default_cwd()). */
static gboolean
inject_shell_integration (GPtrArray  *argvv,
                          GPtrArray  *envv,
                          const char *shell,
                          gboolean    login)
{
  g_autofree char *sidir = find_shell_integration_dir ();
  g_autofree char *base = NULL;

  if (sidir == NULL)
    return FALSE;   /* scripts not found: plain shell, no integration */

  base = g_path_get_basename (shell);

  if (g_strcmp0 (base, "bash") == 0)
    {
      /* --rcfile only applies to an interactive NON-login bash. A login bash
       * reads /etc/profile and then ~/.bash_profile (or ~/.bash_login, or
       * ~/.profile) and ignores --rcfile entirely, so the injection silently
       * would not happen. Rather than pass a flag that does nothing, skip
       * injection and let login semantics be real: the alternative is either
       * a fake login shell or a login shell whose startup files never ran.
       *
       * Consequence: a login bash gets no OSC 7, so panes opened from it start
       * at $HOME. Same trade-off as a custom command or an unknown shell. */
      if (login)
        return FALSE;

      g_ptr_array_add (argvv, g_strdup ("--rcfile"));
      g_ptr_array_add (argvv, g_build_filename (sidir, "epimone.bash", NULL));
      g_ptr_array_add (envv, g_strdup ("EPIMONE_BASH_RCFILE=1"));
    }
  else if (g_strcmp0 (base, "zsh") == 0)
    {
      /* zsh needs no special case for login mode: a login zsh still reads
       * $ZDOTDIR/.zprofile and $ZDOTDIR/.zshrc, and the shim provides both,
       * so the integration composes with login shells as-is. */
      g_autofree char *zdotdir = g_build_filename (sidir, "zsh", NULL);
      const char *real = g_getenv ("ZDOTDIR");

      g_ptr_array_add (envv, g_strdup_printf ("ZDOTDIR=%s", zdotdir));
      g_ptr_array_add (envv, g_strdup_printf ("EPIMONE_ZDOTDIR_REAL=%s",
                                              (real && real[0]) ? real
                                                                : g_get_home_dir ()));
    }
  else
    {
      /* Some other shell (fish, dash, a custom build): there is no injection
       * mechanism for it, so leave it alone. */
      return FALSE;
    }

  g_ptr_array_add (envv, g_strdup ("EPIMONE=1"));
  g_ptr_array_add (envv, g_strdup_printf ("EPIMONE_SHELL_INTEGRATION_DIR=%s", sidir));
  return TRUE;
}

/* Decide what a new session runs, from the Shell & Profiles settings.
 *
 * Two shapes:
 *   custom command -> the parsed command line, run directly (not via a shell).
 *                     No integration: the command need not be a shell, and
 *                     login mode is meaningless for it.
 *   otherwise      -> the configured shell ('shell-path', empty meaning the
 *                     system default), with integration where possible.
 *
 * A login shell is requested the traditional way, by prefixing argv[0] with a
 * dash, which is why *out_exec then carries the real path: argv[0] is "-bash"
 * but the file to execute is still /bin/bash.
 *
 * Returns FALSE with @error set only when the custom command cannot be parsed;
 * the caller must then not spawn. */
static gboolean
build_session_spawn (GPtrArray *argvv, GPtrArray *envv, char **out_exec,
                     GError **error)
{
  GSettings *settings = spawn_settings ();
  g_autofree char *shell = NULL;
  g_autofree char *base = NULL;
  gboolean login;

  *out_exec = NULL;

  if (g_settings_get_boolean (settings, "use-custom-command"))
    {
      g_autofree char *command = g_settings_get_string (settings, "custom-command");

      if (command != NULL && command[0] != '\0')
        {
          g_auto(GStrv) parsed = NULL;
          int parsed_argc = 0;

          if (!g_shell_parse_argv (command, &parsed_argc, &parsed, error))
            return FALSE;

          for (int i = 0; i < parsed_argc; i++)
            g_ptr_array_add (argvv, g_strdup (parsed[i]));
          return TRUE;
        }
      /* Enabled but empty: nothing to run, so fall through to the shell rather
       * than spawning an empty argv. */
    }

  shell = g_settings_get_string (settings, "shell-path");
  if (shell == NULL || shell[0] == '\0')
    {
      g_free (shell);
      shell = epimone_client_default_shell ();
    }

  login = g_settings_get_boolean (settings, "login-shell");
  base = g_path_get_basename (shell);

  if (login)
    {
      /* A login shell is one whose argv[0] starts with a dash. Passing it this
       * way rather than as --login/-l works for every shell, including those
       * with no such flag, and is what the shell itself tests for. The file to
       * execute has to travel separately, since argv[0] is no longer it. */
      g_ptr_array_add (argvv, g_strdup_printf ("-%s", base));
      *out_exec = g_strdup (shell);
    }
  else
    {
      g_ptr_array_add (argvv, g_strdup (shell));
    }

  inject_shell_integration (argvv, envv, shell, login);
  return TRUE;
}

guint64
epimone_client_create_session (const char *cwd, GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  guint64 id = 0;
  GPtrArray *argvv = g_ptr_array_new_with_free_func (g_free);
  GPtrArray *envv = g_ptr_array_new_with_free_func (g_free);
  g_autofree char *exec_path = NULL;

  /* A custom command that will not parse is refused here rather than sent on:
   * spawning the mis-split argv would leave a pane running the wrong thing (or
   * nothing at all) with no indication why. */
  if (!build_session_spawn (argvv, envv, &exec_path, error))
    {
      g_ptr_array_free (argvv, TRUE);
      g_ptr_array_free (envv, TRUE);
      return 0;
    }

  epi_msg_start (&req, EPI_MSG_CREATE);
  epi_buf_put_str (&req, cwd ? cwd : "");
  epi_buf_put_u32 (&req, (guint32) argvv->len);
  for (guint i = 0; i < argvv->len; i++)
    epi_buf_put_str (&req, g_ptr_array_index (argvv, i));
  epi_buf_put_u32 (&req, (guint32) envv->len);
  for (guint i = 0; i < envv->len; i++)
    epi_buf_put_str (&req, g_ptr_array_index (envv, i));
  epi_buf_put_str (&req, exec_path ? exec_path : "");
  epi_msg_end (&req);

  g_ptr_array_free (argvv, TRUE);
  g_ptr_array_free (envv, TRUE);

  if (!control_txn (&req, &type, &body, &blen, error))
    return 0;

  if (reply_is_error (type, body, blen, error))
    ;
  else if (type == EPI_MSG_ID)
    {
      epi_rd r;
      epi_rd_init (&r, body, blen);
      id = epi_rd_u64 (&r);
    }
  else
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to CREATE");

  g_free (body);
  return id;
}

void
epimone_client_send_launch_command (int fd)
{
  g_autofree char *command = g_settings_get_string (spawn_settings (),
                                                    "launch-command");
  g_autofree char *line = NULL;
  size_t len, off = 0;

  if (fd < 0 || command == NULL || command[0] == '\0')
    return;

  /* Exactly what typing it would send: the text, then Return. */
  line = g_strconcat (command, "\n", NULL);
  len = strlen (line);

  /* The data channel is non-blocking. A command line is far smaller than the
   * socket buffer so this almost always completes in one write; the loop is
   * here so a busy channel delays the command instead of truncating it. */
  while (off < len)
    {
      ssize_t n = write (fd, line + off, len - off);

      if (n > 0)
        {
          off += (size_t) n;
          continue;
        }
      if (n < 0 && errno == EINTR)
        continue;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
          struct pollfd pfd = { fd, POLLOUT, 0 };

          if (poll (&pfd, 1, 200) > 0)
            continue;
          g_warning ("launch command: channel not writable, sent %zu of %zu bytes",
                     off, len);
          return;
        }
      g_warning ("launch command: write failed: %s", g_strerror (errno));
      return;
    }
}

GPtrArray *
epimone_client_list_sessions (GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  GPtrArray *out;
  epi_rd r;
  guint32 count, i;

  epi_msg_start (&req, EPI_MSG_LIST);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return NULL;

  if (reply_is_error (type, body, blen, error))
    {
      g_free (body);
      return NULL;
    }
  if (type != EPI_MSG_LIST_REPLY)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to LIST");
      g_free (body);
      return NULL;
    }

  out = g_ptr_array_new_with_free_func ((GDestroyNotify) epimone_session_info_free);
  epi_rd_init (&r, body, blen);
  count = epi_rd_u32 (&r);
  for (i = 0; i < count && !r.err; i++)
    {
      EpiSessionInfo *info = g_new0 (EpiSessionInfo, 1);
      info->id = epi_rd_u64 (&r);
      info->pid = epi_rd_u32 (&r);
      info->alive = epi_rd_u8 (&r) ? TRUE : FALSE;
      info->attached = epi_rd_u8 (&r) ? TRUE : FALSE;
      info->created_at = (gint64) epi_rd_u64 (&r);
      info->cwd = epi_rd_str (&r);
      if (info->cwd == NULL)
        info->cwd = g_strdup ("");
      g_ptr_array_add (out, info);
    }

  g_free (body);
  return out;
}

int
epimone_client_attach_session (guint64 id, GError **error)
{
  int fd = client_connect (error);
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  int flags;

  if (fd < 0)
    return -1;

  epi_msg_start (&req, EPI_MSG_ATTACH);
  epi_buf_put_u64 (&req, id);
  epi_msg_end (&req);
  if (!send_frame (fd, &req, error))
    {
      close (fd);
      return -1;
    }

  /* Read the single framed reply; everything after is the raw stream. */
  if (!recv_frame (fd, &type, &body, &blen, error))
    {
      close (fd);
      return -1;
    }
  if (type != EPI_MSG_ATTACHED)
    {
      if (!reply_is_error (type, body, blen, error))
        g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
                     "unexpected reply to ATTACH");
      g_free (body);
      close (fd);
      return -1;
    }
  g_free (body);

  /* Hand back a non-blocking fd for the GUI's event-loop pump. */
  flags = fcntl (fd, F_GETFL, 0);
  if (flags >= 0)
    fcntl (fd, F_SETFL, flags | O_NONBLOCK);

  return fd;
}

static gboolean
simple_id_txn (guint32 msg_type, guint64 id, GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  gboolean ok = FALSE;

  epi_msg_start (&req, msg_type);
  epi_buf_put_u64 (&req, id);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return FALSE;

  if (reply_is_error (type, body, blen, error))
    ok = FALSE;
  else if (type == EPI_MSG_OK)
    ok = TRUE;
  else
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply");

  g_free (body);
  return ok;
}

gboolean
epimone_client_kill_session (guint64 id, GError **error)
{
  return simple_id_txn (EPI_MSG_KILL, id, error);
}

guint8 *
epimone_client_peek_session (guint64 id, guint32 max_bytes, gsize *out_len,
                             guint64 *out_total, GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  guint8 *tail = NULL;
  epi_rd r;
  size_t tlen = 0;

  if (out_len != NULL)
    *out_len = 0;
  if (out_total != NULL)
    *out_total = 0;

  epi_msg_start (&req, EPI_MSG_PEEK);
  epi_buf_put_u64 (&req, id);
  epi_buf_put_u32 (&req, max_bytes);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return NULL;
  if (reply_is_error (type, body, blen, error))
    {
      g_free (body);
      return NULL;
    }
  if (type != EPI_MSG_PEEK_REPLY)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to PEEK");
      g_free (body);
      return NULL;
    }

  epi_rd_init (&r, body, blen);
  (void) epi_rd_u64 (&r);              /* echoed session id */
  tail = epi_rd_blob (&r, &tlen);
  /* total_size is an optional trailing field; absent on older daemons. */
  if (!r.err && r.pos < r.len && out_total != NULL)
    *out_total = epi_rd_u64 (&r);

  if (out_len != NULL)
    *out_len = tlen;
  g_free (body);
  return tail;
}

/* ------------------------------------------------------------------ *
 * foreground command
 * ------------------------------------------------------------------ */

/* /proc/<pid>/comm, or NULL. Truncated to 15 chars by the kernel, which is the
 * name people actually recognise ("hashcat", "ping", "vim"). */
static char *
read_comm (guint pid)
{
  g_autofree char *path = g_strdup_printf ("/proc/%u/comm", pid);
  char *text = NULL;
  gsize len = 0;

  if (!g_file_get_contents (path, &text, &len, NULL))
    return NULL;
  g_strchomp (text);            /* comm has a trailing newline */
  if (text[0] == '\0')
    {
      g_free (text);
      return NULL;
    }
  return text;
}

/* Field 8 of /proc/<pid>/stat is tpgid: the process group in the foreground of
 * this process's controlling terminal. Parsing has to start after the comm field,
 * which is parenthesised and may itself contain spaces and parentheses, hence
 * scanning to the LAST ')' rather than the first. */
static int
read_tpgid (guint pid)
{
  g_autofree char *path = g_strdup_printf ("/proc/%u/stat", pid);
  g_autofree char *text = NULL;
  const char *p;
  int field;

  if (!g_file_get_contents (path, &text, NULL, NULL))
    return -1;

  p = strrchr (text, ')');
  if (p == NULL)
    return -1;
  p++;                          /* now positioned just before field 3 (state) */

  /* Walk to field 8; p currently sits before field 3, so five more fields. */
  for (field = 3; field < 8; field++)
    {
      while (*p == ' ')
        p++;
      while (*p != '\0' && *p != ' ')
        p++;
    }
  while (*p == ' ')
    p++;
  if (*p == '\0')
    return -1;
  return (int) g_ascii_strtoll (p, NULL, 10);
}

char *
epimone_client_foreground_command (guint pid)
{
  int tpgid;

  if (pid == 0)
    return NULL;

  tpgid = read_tpgid (pid);
  if (tpgid > 0 && (guint) tpgid != pid)
    {
      /* The foreground group leader's pid equals the group id. It can have exited
       * while the group lives on, so fall through to the shell if it is gone. */
      char *name = read_comm ((guint) tpgid);
      if (name != NULL)
        return name;
    }

  /* Nothing else in the foreground (or it just vanished): the child itself. */
  return read_comm (pid);
}

/* ------------------------------------------------------------------ *
 * groups
 * ------------------------------------------------------------------ */

void
epimone_group_info_free (EpiGroupInfo *info)
{
  if (info == NULL)
    return;
  g_free (info->blob);
  if (info->members != NULL)
    g_array_unref (info->members);
  g_free (info);
}

GPtrArray *
epimone_client_list_groups (guint64 *out_instance_id, GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  GPtrArray *out;
  epi_rd r;
  guint32 count, i;

  if (out_instance_id != NULL)
    *out_instance_id = 0;

  epi_msg_start (&req, EPI_MSG_GROUP_LIST);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return NULL;

  if (reply_is_error (type, body, blen, error))
    {
      g_free (body);
      return NULL;
    }
  if (type != EPI_MSG_GROUP_LIST_REPLY)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
                   "unexpected reply to GROUP_LIST");
      g_free (body);
      return NULL;
    }

  out = g_ptr_array_new_with_free_func ((GDestroyNotify) epimone_group_info_free);
  epi_rd_init (&r, body, blen);

  if (out_instance_id != NULL)
    *out_instance_id = epi_rd_u64 (&r);
  else
    (void) epi_rd_u64 (&r);

  count = epi_rd_u32 (&r);
  for (i = 0; i < count && !r.err; i++)
    {
      EpiGroupInfo *info = g_new0 (EpiGroupInfo, 1);
      size_t bl = 0;
      guint32 members, m;

      info->id = epi_rd_u64 (&r);
      info->created_at = (gint64) epi_rd_u64 (&r);
      info->blob = epi_rd_blob (&r, &bl);
      info->blob_len = bl;
      info->members = g_array_new (FALSE, FALSE, sizeof (guint64));

      members = epi_rd_u32 (&r);
      for (m = 0; m < members && !r.err; m++)
        {
          guint64 sid = epi_rd_u64 (&r);
          g_array_append_val (info->members, sid);
        }

      g_ptr_array_add (out, info);
    }

  g_free (body);
  return out;
}

/* Guard locally so an oversize blob produces a clear error here rather than a
 * generic wire rejection, and so the frame is never even built. */
static gboolean
blob_size_ok (gsize len, GError **error)
{
  if (len <= EPI_GROUP_BLOB_MAX)
    return TRUE;
  g_set_error (error, EPIMONE_CLIENT_ERROR, 1,
               "layout blob is %" G_GSIZE_FORMAT " bytes, over the %d byte limit",
               len, EPI_GROUP_BLOB_MAX);
  return FALSE;
}

guint64
epimone_client_group_new (const guint8 *blob, gsize len, GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  guint64 gid = 0;

  if (!blob_size_ok (len, error))
    return 0;

  epi_msg_start (&req, EPI_MSG_GROUP_NEW);
  epi_buf_put_blob (&req, blob, len);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return 0;

  if (reply_is_error (type, body, blen, error))
    gid = 0;
  else if (type == EPI_MSG_GROUP_ID)
    {
      epi_rd r;
      epi_rd_init (&r, body, blen);
      gid = epi_rd_u64 (&r);
    }
  else
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to GROUP_NEW");

  g_free (body);
  return gid;
}

gboolean
epimone_client_group_set (guint64       gid,
                          const guint8 *blob,
                          gsize         len,
                          GError      **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  gboolean ok = FALSE;

  if (!blob_size_ok (len, error))
    return FALSE;

  epi_msg_start (&req, EPI_MSG_GROUP_SET);
  epi_buf_put_u64 (&req, gid);
  epi_buf_put_blob (&req, blob, len);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return FALSE;

  if (reply_is_error (type, body, blen, error))
    ok = FALSE;
  else if (type == EPI_MSG_OK)
    ok = TRUE;
  else
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to GROUP_SET");

  g_free (body);
  return ok;
}

gboolean
epimone_client_group_add (guint64 gid, guint64 sid, GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  gboolean ok = FALSE;

  epi_msg_start (&req, EPI_MSG_GROUP_ADD);
  epi_buf_put_u64 (&req, gid);
  epi_buf_put_u64 (&req, sid);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return FALSE;

  if (reply_is_error (type, body, blen, error))
    ok = FALSE;
  else if (type == EPI_MSG_OK)
    ok = TRUE;
  else
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to GROUP_ADD");

  g_free (body);
  return ok;
}

gboolean
epimone_client_group_remove (guint64 sid, GError **error)
{
  return simple_id_txn (EPI_MSG_GROUP_REMOVE, sid, error);
}

guint64
epimone_client_instance_id (GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  guint64 inst = 0;
  epi_rd r;
  guint32 count, i;

  epi_msg_start (&req, EPI_MSG_LIST);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return 0;

  if (reply_is_error (type, body, blen, error))
    {
      g_free (body);
      return 0;
    }
  if (type != EPI_MSG_LIST_REPLY)
    {
      g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to LIST");
      g_free (body);
      return 0;
    }

  /* The instance id is an optional field trailing the session array, so the
   * array has to be skipped to reach it. Its absence means an older daemon. */
  epi_rd_init (&r, body, blen);
  count = epi_rd_u32 (&r);
  for (i = 0; i < count && !r.err; i++)
    {
      char *cwd;
      (void) epi_rd_u64 (&r);
      (void) epi_rd_u32 (&r);
      (void) epi_rd_u8 (&r);
      (void) epi_rd_u8 (&r);
      (void) epi_rd_u64 (&r);
      cwd = epi_rd_str (&r);
      g_free (cwd);
    }
  if (!r.err && r.pos < r.len)
    inst = epi_rd_u64 (&r);

  g_free (body);
  return inst;
}

gboolean
epimone_client_detach_session (guint64 id, GError **error)
{
  return simple_id_txn (EPI_MSG_DETACH, id, error);
}

gboolean
epimone_client_resize_session (guint64 id, guint rows, guint cols,
                               GError **error)
{
  epi_buf req;
  guint32 type;
  guint8 *body = NULL;
  size_t blen = 0;
  gboolean ok = FALSE;

  epi_msg_start (&req, EPI_MSG_RESIZE);
  epi_buf_put_u64 (&req, id);
  epi_buf_put_u16 (&req, (guint16) rows);
  epi_buf_put_u16 (&req, (guint16) cols);
  epi_msg_end (&req);

  if (!control_txn (&req, &type, &body, &blen, error))
    return FALSE;

  if (reply_is_error (type, body, blen, error))
    ok = FALSE;
  else if (type == EPI_MSG_OK)
    ok = TRUE;
  else
    g_set_error (error, EPIMONE_CLIENT_ERROR, 1, "unexpected reply to RESIZE");

  g_free (body);
  return ok;
}
