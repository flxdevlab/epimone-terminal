#include "log.h"
#include "paths.h"
#include "server.h"
#include "../epimone-protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Probe an existing socket. Returns 1 if a daemon is answering, 0 if the
 * socket is stale (connect refused / missing), -1 on other errors. */
static int
socket_in_use (const char *path)
{
  struct sockaddr_un addr;
  int fd;
  int rc;

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return -1;

  memset (&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen (path) >= sizeof addr.sun_path)
    {
      close (fd);
      return -1;
    }
  strcpy (addr.sun_path, path);

  rc = connect (fd, (struct sockaddr *) &addr, sizeof addr);
  close (fd);

  if (rc == 0)
    return 1;                 /* someone is listening */
  if (errno == ECONNREFUSED || errno == ENOENT)
    return 0;                 /* stale or absent */
  return -1;
}

/* Fork into the background and redirect logging to the state-dir log file. */
static int
daemonize (void)
{
  char statedir[512];
  char logpath[600];
  pid_t pid;
  int fd;

  pid = fork ();
  if (pid < 0)
    return -1;
  if (pid > 0)
    _exit (0);                /* parent leaves */

  if (setsid () < 0)
    return -1;

  /* Second fork so we can never reacquire a controlling terminal. */
  pid = fork ();
  if (pid < 0)
    return -1;
  if (pid > 0)
    _exit (0);

  if (chdir ("/") != 0)
    { /* non-fatal */ }

  /* stdin/stdout -> /dev/null */
  fd = open ("/dev/null", O_RDWR);
  if (fd >= 0)
    {
      dup2 (fd, STDIN_FILENO);
      dup2 (fd, STDOUT_FILENO);
      if (fd > STDERR_FILENO)
        close (fd);
    }

  /* stderr -> log file (log module writes to whichever fd we hand it) */
  if (epi_state_dir (statedir, sizeof statedir) == 0)
    {
      snprintf (logpath, sizeof logpath, "%s/daemon.log", statedir);
      fd = open (logpath, O_WRONLY | O_CREAT | O_APPEND, 0600);
      if (fd >= 0)
        {
          dup2 (fd, STDERR_FILENO);
          if (fd != STDERR_FILENO)
            close (fd);
          epi_log_set_fd (STDERR_FILENO);
        }
    }

  return 0;
}

int
main (int argc, char *argv[])
{
  epi_server srv;
  char sockpath[512];
  int do_daemonize = 0;
  int rc;

  for (int i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--daemonize") == 0)
        do_daemonize = 1;
      else if (strcmp (argv[i], "--help") == 0 || strcmp (argv[i], "-h") == 0)
        {
          printf ("usage: epimone-daemon [--daemonize]\n");
          return 0;
        }
      else
        {
          fprintf (stderr, "epimone-daemon: unknown argument '%s'\n", argv[i]);
          return 2;
        }
    }

  if (epi_socket_path (sockpath, sizeof sockpath) != 0)
    {
      fprintf (stderr, "epimone-daemon: cannot determine socket path\n");
      return 1;
    }

  /* Single-instance: if the socket answers, another daemon owns it. */
  rc = socket_in_use (sockpath);
  if (rc == 1)
    {
      fprintf (stderr, "epimone-daemon: already running at %s\n", sockpath);
      return 0;
    }
  if (rc == 0)
    unlink (sockpath);        /* clean up a stale socket */

  if (do_daemonize && daemonize () != 0)
    {
      fprintf (stderr, "epimone-daemon: failed to daemonize\n");
      return 1;
    }

  if (epi_server_init (&srv, sockpath) != 0)
    {
      epi_server_shutdown (&srv);
      return 1;
    }

  epi_server_run (&srv);
  epi_server_shutdown (&srv);
  return 0;
}
