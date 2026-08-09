#ifndef EPIMONE_LOG_H
#define EPIMONE_LOG_H

/* Minimal logging. Everything is written to the process's stderr, which the
 * daemon points at either the terminal (foreground) or a log file
 * (--daemonize). */

void epi_log_set_fd (int fd);
void epi_logf (const char *level, const char *fmt, ...)
  __attribute__ ((format (printf, 2, 3)));

#define epi_info(...)  epi_logf ("INFO", __VA_ARGS__)
#define epi_warn(...)  epi_logf ("WARN", __VA_ARGS__)
#define epi_err(...)   epi_logf ("ERR ", __VA_ARGS__)

#endif /* EPIMONE_LOG_H */
