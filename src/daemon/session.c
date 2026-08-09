#include "session.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <pwd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

extern char **environ;

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

/* Where a session starts when the client passes no cwd.
 *
 * A NULL/empty cwd means there was nothing to inherit (the first terminal at
 * startup). It does NOT mean "wherever the daemon happens to be": daemonize()
 * does chdir("/") (see main.c), so without this the first shell would open in
 * the filesystem root rather than the user's home.
 *
 * $HOME first (what the user's session actually set), then the passwd entry
 * (daemon started from a stripped environment). Both must be absolute.
 * Returns NULL when neither is usable; the caller then skips the chdir and
 * the child stays at the daemon's "/", the documented fallback.
 *
 * The returned pointer is into environ or getpwuid()'s static storage; it is
 * resolved in the parent before forkpty() and only read in the child, so it
 * stays valid for as long as it is used. */
static const char *
default_cwd (void)
{
  const char *home = getenv ("HOME");
  struct passwd *pw;

  if (home != NULL && home[0] == '/')
    return home;

  pw = getpwuid (getuid ());
  if (pw != NULL && pw->pw_dir != NULL && pw->pw_dir[0] == '/')
    return pw->pw_dir;

  return NULL;
}

/* Ensure TERM is present in the child before exec so the shell behaves. */
static void
ensure_term (void)
{
  if (getenv ("TERM") == NULL)
    setenv ("TERM", "xterm-256color", 1);
}

epi_session *
epi_session_spawn (const char *cwd, char *const argv[], char *const env[],
                   const char *exec_path)
{
  epi_session *s;
  struct winsize ws = { 24, 80, 0, 0 };
  int master = -1;
  pid_t pid;
  const char *shell;
  const char *start_cwd;
  char *default_argv[2];

  s = (epi_session *) calloc (1, sizeof *s);
  if (s == NULL)
    return NULL;

  if (epi_ring_init (&s->out, EPI_RING_CAP) != 0)
    {
      free (s);
      return NULL;
    }

  shell = getenv ("SHELL");
  if (shell == NULL || shell[0] == '\0')
    shell = "/bin/bash";
  default_argv[0] = (char *) shell;
  default_argv[1] = NULL;

  /* An explicit cwd (splits, new tabs, `epimone-ctl create <dir>`) is used as
   * given; only the no-cwd case falls back to home. Resolved here in the parent
   * so the recorded session cwd matches what the child will chdir to. */
  start_cwd = (cwd != NULL && cwd[0] != '\0') ? cwd : default_cwd ();

  pid = forkpty (&master, NULL, NULL, &ws);
  if (pid < 0)
    {
      epi_err ("forkpty failed: %s", strerror (errno));
      epi_ring_free (&s->out);
      free (s);
      return NULL;
    }

  if (pid == 0)
    {
      /* Child: forkpty already did setsid + login_tty on the slave. */
      char *const *child_argv = (argv != NULL) ? argv : default_argv;
      /* The file to execute. Normally argv[0], but the client may name it
       * separately so that argv[0] can differ from it; see epi_session_spawn's
       * contract and the login-shell "-bash" convention. */
      const char *child_exec = (exec_path != NULL && exec_path[0] != '\0')
                                 ? exec_path
                                 : child_argv[0];

      /* Give the shell a clean signal state. The daemon blocks SIGINT/SIGTERM/
       * SIGCHLD for its signalfd and ignores SIGPIPE; the signal mask and
       * dispositions are inherited across fork+exec, so without this the shell
       * (and every foreground job it runs) would start with SIGINT blocked and
       * SIGQUIT/SIGPIPE ignored: Ctrl+C would never deliver SIGINT, Ctrl+\
       * never SIGQUIT. Unblock everything and restore defaults. */
      {
        sigset_t empty;
        int sig;

        sigemptyset (&empty);
        sigprocmask (SIG_SETMASK, &empty, NULL);
        for (sig = 1; sig < NSIG; sig++)
          signal (sig, SIG_DFL);
      }

      if (start_cwd != NULL)
        {
          if (chdir (start_cwd) != 0)
            { /* fall through and start in the inherited directory */ }
        }

      if (env != NULL)
        {
          /* Extra KEY=VALUE vars from the client, merged onto the inherited
           * environment (used for shell-integration injection: EPIMONE,
           * EPIMONE_SHELL_INTEGRATION_DIR, ZDOTDIR, ...). putenv keeps the
           * pointers, which stay valid in this forked child until execvp. */
          for (size_t i = 0; env[i] != NULL; i++)
            putenv (env[i]);
        }
      ensure_term ();

      execvp (child_exec, child_argv);
      _exit (127);
    }

  /* Parent. */
  if (set_nonblock_cloexec (master) != 0)
    {
      epi_warn ("failed to set PTY master flags: %s", strerror (errno));
      close (master);
      kill (pid, SIGHUP);
      epi_ring_free (&s->out);
      free (s);
      return NULL;
    }

  s->pid = pid;
  s->pty_master = master;
  s->alive = true;
  s->created_at = time (NULL);
  /* Record where the shell was actually started, so `epimone-ctl list` reports
   * the resolved home rather than an empty string for no-cwd sessions. */
  s->cwd = strdup ((start_cwd != NULL) ? start_cwd : "");
  s->pty_source.kind = EPI_SRC_PTY;
  s->pty_source.obj = s;

  return s;
}

void
epi_session_free (epi_session *s)
{
  if (s == NULL)
    return;
  if (s->pty_master >= 0)
    close (s->pty_master);
  epi_ring_free (&s->out);
  free (s->pin);
  free (s->cwd);
  free (s);
}

bool
epi_session_queue_input (epi_session *s, const uint8_t *data, size_t len)
{
  if (len == 0)
    return s->pin_len > 0;

  if (s->pin_len + len > s->pin_cap)
    {
      size_t nc = s->pin_cap ? s->pin_cap : 4096;
      uint8_t *nd;
      while (nc < s->pin_len + len)
        nc *= 2;
      nd = (uint8_t *) realloc (s->pin, nc);
      if (nd == NULL)
        {
          epi_warn ("session %llu: dropping input (OOM)",
                    (unsigned long long) s->id);
          return s->pin_len > 0;
        }
      s->pin = nd;
      s->pin_cap = nc;
    }

  memcpy (s->pin + s->pin_len, data, len);
  s->pin_len += len;

  return epi_session_flush_input (s);
}

bool
epi_session_flush_input (epi_session *s)
{
  size_t off = 0;

  if (s->pty_master < 0)
    {
      s->pin_len = 0;
      return false;
    }

  while (off < s->pin_len)
    {
      ssize_t n = write (s->pty_master, s->pin + off, s->pin_len - off);
      if (n > 0)
        {
          off += (size_t) n;
          continue;
        }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (n < 0 && errno == EINTR)
        continue;
      /* Write error (e.g. child gone): discard the rest. */
      off = s->pin_len;
      break;
    }

  if (off > 0)
    {
      memmove (s->pin, s->pin + off, s->pin_len - off);
      s->pin_len -= off;
    }

  return s->pin_len > 0;
}

void
epi_session_resize (epi_session *s, uint16_t rows, uint16_t cols)
{
  struct winsize ws = { rows, cols, 0, 0 };

  if (s->pty_master < 0)
    return;
  if (ioctl (s->pty_master, TIOCSWINSZ, &ws) != 0)
    epi_warn ("session %llu: TIOCSWINSZ failed: %s",
              (unsigned long long) s->id, strerror (errno));
}

void
epi_session_hangup (epi_session *s)
{
  if (s->alive && s->pid > 0)
    kill (s->pid, SIGHUP);
}

void
epi_session_kill (epi_session *s)
{
  if (s->alive && s->pid > 0)
    kill (s->pid, SIGKILL);
}
