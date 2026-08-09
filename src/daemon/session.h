#ifndef EPIMONE_SESSION_H
#define EPIMONE_SESSION_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "ring.h"

/* Forward decls to avoid pulling server.h into session.h. */
struct connection;

/* epoll source tags (see server.c). Declared here so a session can embed the
 * source used to register its PTY master fd. */
enum epi_src_kind {
  EPI_SRC_LISTEN,
  EPI_SRC_SIGNAL,
  EPI_SRC_CONN,
  EPI_SRC_PTY
};

struct epi_source {
  enum epi_src_kind kind;
  void             *obj;
};

typedef struct epi_session {
  uint64_t   id;
  uint64_t   group_id;      /* owning group, 0 = ungrouped */
  pid_t      pid;
  int        pty_master;    /* -1 once closed */
  bool       alive;         /* child still running */
  bool       killing;       /* KILL requested; awaiting reap */
  bool       removed;       /* hidden from LIST (killed) */
  bool       defunct;       /* unlinked; awaiting deferred free */
  time_t     created_at;
  int        exit_status;
  char      *cwd;

  epi_ring   out;           /* PTY output ring buffer */

  /* Bytes queued to be written to the PTY master (client keyboard input). */
  uint8_t   *pin;
  size_t     pin_len;
  size_t     pin_cap;

  struct connection *client;   /* attached data connection, or NULL */

  struct epi_source  pty_source;
  struct timespec    kill_deadline;   /* when to escalate to SIGKILL */

  struct epi_session *next;
  struct epi_session *dead_next;      /* deferred-free list link */
} epi_session;

/* Spawn a new session: forkpty + exec. argv/env may be NULL (=> $SHELL /
 * inherit environ). argv/env are NULL-terminated arrays when non-NULL.
 *
 * exec_path may be NULL or empty, meaning "execute argv[0]", the ordinary
 * case. When non-empty it is the file executed instead, leaving argv[0] free
 * to be something else; that is how a login shell is spawned, since a login
 * shell is one whose argv[0] begins with a dash ("-bash").
 *
 * Returns a new session (caller assigns ->id) or NULL on failure. */
epi_session *epi_session_spawn (const char *cwd, char *const argv[],
                                char *const env[], const char *exec_path);

void epi_session_free (epi_session *s);

/* Queue keyboard input for the PTY. Returns true if there is now buffered
 * data still waiting to be written (i.e. the caller should watch EPOLLOUT). */
bool epi_session_queue_input (epi_session *s, const uint8_t *data, size_t len);

/* Try to flush queued input to the PTY. Returns true if data remains. */
bool epi_session_flush_input (epi_session *s);

void epi_session_resize (epi_session *s, uint16_t rows, uint16_t cols);
void epi_session_hangup (epi_session *s);   /* SIGHUP the child */
void epi_session_kill (epi_session *s);     /* SIGKILL the child */

#endif /* EPIMONE_SESSION_H */
