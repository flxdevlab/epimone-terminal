#include "log.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int log_fd = 2;   /* stderr by default */

void
epi_log_set_fd (int fd)
{
  log_fd = fd;
}

void
epi_logf (const char *level, const char *fmt, ...)
{
  char line[2048];
  char ts[32];
  struct tm tm;
  time_t now;
  int n;
  va_list ap;

  now = time (NULL);
  localtime_r (&now, &tm);
  strftime (ts, sizeof ts, "%H:%M:%S", &tm);

  n = snprintf (line, sizeof line, "[%s] %s ", ts, level);
  if (n < 0 || (size_t) n >= sizeof line)
    return;

  va_start (ap, fmt);
  n += vsnprintf (line + n, sizeof line - (size_t) n, fmt, ap);
  va_end (ap);

  if (n < 0)
    return;
  if ((size_t) n >= sizeof line)
    n = (int) sizeof line - 1;

  line[n] = '\n';
  n++;

  if (write (log_fd, line, (size_t) n) < 0)
    { /* nothing useful to do if logging itself fails */ }
}
