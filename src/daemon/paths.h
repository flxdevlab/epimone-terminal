#ifndef EPIMONE_PATHS_H
#define EPIMONE_PATHS_H

#include <stddef.h>

/* Fill buf with the runtime directory that holds the control socket:
 *   $XDG_RUNTIME_DIR/epimone   (fallback /tmp/epimone-$UID)
 * The directory is created if needed with 0700 perms.
 * Returns 0 on success, -1 on error. */
int epi_runtime_dir (char *buf, size_t buflen);

/* Fill buf with the full control socket path. Returns 0/-1. */
int epi_socket_path (char *buf, size_t buflen);

/* Fill buf with the state directory used for the daemon log:
 *   $XDG_STATE_HOME/epimone   (fallback ~/.local/state/epimone)
 * Created if needed. Returns 0/-1. */
int epi_state_dir (char *buf, size_t buflen);

#endif /* EPIMONE_PATHS_H */
