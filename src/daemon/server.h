#ifndef EPIMONE_SERVER_H
#define EPIMONE_SERVER_H

#include <stdint.h>

#include "group.h"
#include "session.h"

typedef struct connection {
  int                fd;
  struct epi_source  source;
  int                is_data;     /* 0 = control, 1 = raw data channel */
  epi_session       *session;     /* bound session for data connections */

  uint8_t           *rbuf;        /* inbound (control frame) buffer */
  size_t             rlen;
  size_t             rcap;

  uint8_t           *wbuf;        /* outbound buffer */
  size_t             wpos;        /* flushed up to here */
  size_t             wlen;        /* valid up to here */
  size_t             wcap;

  int                want_close;  /* close once wbuf is drained */

  struct server     *srv;
  struct connection *next;
} connection;

typedef struct server {
  int               epfd;
  int               listen_fd;
  int               sig_fd;
  struct epi_source listen_source;
  struct epi_source sig_source;

  uint64_t          next_id;
  uint64_t          next_group_id;
  int               running;

  /* Identifies this daemon process. Session and group ids restart at 1 on every
   * daemon start, so a client holding ids from a previous daemon needs a way to
   * notice they now name something else. Reported by LIST and GROUP_LIST; never
   * zero for a running daemon. */
  uint64_t          instance_id;

  epi_session      *sessions;
  epi_session      *dead;         /* sessions unlinked mid-batch, freed after */
  epi_group        *groups;
  connection       *conns;

  char              sockpath[512];
} epi_server;

/* Create the listening socket at sockpath, epoll and signalfd. Returns 0 on
 * success, -1 on error. */
int  epi_server_init (epi_server *srv, const char *sockpath);

/* Run the event loop until a shutdown signal is received. */
void epi_server_run (epi_server *srv);

/* Hang up every child, tear down and unlink the socket. */
void epi_server_shutdown (epi_server *srv);

#endif /* EPIMONE_SERVER_H */
