#include "paths.h"
#include "../epimone-protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* Create path and any missing parent directories with 0700 perms. */
static int
ensure_dir (const char *path)
{
  char tmp[512];
  size_t len;
  size_t i;

  len = strlen (path);
  if (len == 0 || len >= sizeof tmp)
    return -1;
  memcpy (tmp, path, len + 1);

  /* Create each intermediate component (skip the leading '/'). */
  for (i = 1; i < len; i++)
    {
      if (tmp[i] == '/')
        {
          tmp[i] = '\0';
          if (mkdir (tmp, 0700) != 0 && errno != EEXIST)
            return -1;
          tmp[i] = '/';
        }
    }

  if (mkdir (tmp, 0700) != 0 && errno != EEXIST)
    return -1;
  return 0;
}

int
epi_runtime_dir (char *buf, size_t buflen)
{
  const char *xdg = getenv ("XDG_RUNTIME_DIR");
  int n;

  if (xdg != NULL && xdg[0] == '/')
    n = snprintf (buf, buflen, "%s/epimone", xdg);
  else
    n = snprintf (buf, buflen, "/tmp/epimone-%u", (unsigned) getuid ());

  if (n < 0 || (size_t) n >= buflen)
    return -1;

  return ensure_dir (buf);
}

int
epi_socket_path (char *buf, size_t buflen)
{
  char dir[512];
  int n;

  if (epi_runtime_dir (dir, sizeof dir) != 0)
    return -1;

  n = snprintf (buf, buflen, "%s/%s", dir, EPIMONE_SOCKET_NAME);
  if (n < 0 || (size_t) n >= buflen)
    return -1;
  return 0;
}

int
epi_state_dir (char *buf, size_t buflen)
{
  const char *xdg = getenv ("XDG_STATE_HOME");
  const char *home;
  int n;

  if (xdg != NULL && xdg[0] == '/')
    {
      n = snprintf (buf, buflen, "%s/epimone", xdg);
      if (n < 0 || (size_t) n >= buflen)
        return -1;
      return ensure_dir (buf);
    }

  home = getenv ("HOME");
  if (home == NULL || home[0] != '/')
    return -1;

  n = snprintf (buf, buflen, "%s/.local/state/epimone", home);
  if (n < 0 || (size_t) n >= buflen)
    return -1;
  return ensure_dir (buf);
}
