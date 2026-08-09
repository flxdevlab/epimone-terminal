#include "server.h"
#include "log.h"
#include "../epimone-protocol.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/random.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KILL_GRACE_MS   2000
#define MAX_WBUF        (8 * 1024 * 1024)   /* drop a client that lags past this */
#define PTY_READ_CHUNK  65536

/* Refuse to buffer an absurd inbound frame. No legitimate request comes close:
 * the largest are a CREATE carrying a full environment and a GROUP_SET carrying
 * a 64 KiB blob. Without a bound, a client could declare a 4 GiB frame length and
 * make the daemon grow its read buffer to match. */
#define MAX_FRAME       (8 * 1024 * 1024)

/* ------------------------------------------------------------------ *
 * epoll helpers
 * ------------------------------------------------------------------ */

static int
ep_add (epi_server *srv, int fd, uint32_t events, struct epi_source *src)
{
  struct epoll_event ev;
  memset (&ev, 0, sizeof ev);
  ev.events = events;
  ev.data.ptr = src;
  return epoll_ctl (srv->epfd, EPOLL_CTL_ADD, fd, &ev);
}

static int
ep_mod (epi_server *srv, int fd, uint32_t events, struct epi_source *src)
{
  struct epoll_event ev;
  memset (&ev, 0, sizeof ev);
  ev.events = events;
  ev.data.ptr = src;
  return epoll_ctl (srv->epfd, EPOLL_CTL_MOD, fd, &ev);
}

static void
ep_del (epi_server *srv, int fd)
{
  epoll_ctl (srv->epfd, EPOLL_CTL_DEL, fd, NULL);
}

/* ------------------------------------------------------------------ *
 * connection buffer helpers
 * ------------------------------------------------------------------ */

/* Append bytes to a connection's outbound buffer, compacting first. */
static void
conn_queue (connection *c, const uint8_t *data, size_t len)
{
  if (len == 0)
    return;

  /* Compact already-flushed bytes off the front. */
  if (c->wpos > 0)
    {
      memmove (c->wbuf, c->wbuf + c->wpos, c->wlen - c->wpos);
      c->wlen -= c->wpos;
      c->wpos = 0;
    }

  if (c->wlen + len > c->wcap)
    {
      size_t nc = c->wcap ? c->wcap : 4096;
      uint8_t *nd;
      while (nc < c->wlen + len)
        nc *= 2;
      nd = (uint8_t *) realloc (c->wbuf, nc);
      if (nd == NULL)
        {
          epi_warn ("conn: OOM queueing %zu bytes, closing", len);
          c->want_close = 1;
          return;
        }
      c->wbuf = nd;
      c->wcap = nc;
    }

  memcpy (c->wbuf + c->wlen, data, len);
  c->wlen += len;

  if (c->wlen - c->wpos > MAX_WBUF)
    {
      epi_warn ("conn: output backlog exceeded %d bytes, dropping client",
                MAX_WBUF);
      c->want_close = 1;
    }
}

static void
conn_queue_frame (connection *c, epi_buf *b)
{
  if (!b->oom)
    conn_queue (c, b->data, b->len);
  epi_buf_free (b);
}

static void
send_error (connection *c, uint32_t code, const char *msg)
{
  epi_buf b;
  epi_msg_start (&b, EPI_MSG_ERROR);
  epi_buf_put_u32 (&b, code);
  epi_buf_put_str (&b, msg);
  epi_msg_end (&b);
  conn_queue_frame (c, &b);
}

static void
send_ok (connection *c)
{
  epi_buf b;
  epi_msg_start (&b, EPI_MSG_OK);
  epi_msg_end (&b);
  conn_queue_frame (c, &b);
}

static void
put_session_info (epi_buf *b, epi_session *s)
{
  epi_buf_put_u64 (b, s->id);
  epi_buf_put_u32 (b, (uint32_t) s->pid);
  epi_buf_put_u8 (b, s->alive ? 1 : 0);
  epi_buf_put_u8 (b, s->client ? 1 : 0);
  epi_buf_put_u64 (b, (uint64_t) s->created_at);
  epi_buf_put_str (b, s->cwd ? s->cwd : "");
}

/* Recompute epoll interest for a connection. */
static void
conn_update (epi_server *srv, connection *c)
{
  uint32_t events = EPOLLIN | EPOLLRDHUP;
  if (c->wlen > c->wpos)
    events |= EPOLLOUT;
  ep_mod (srv, c->fd, events, &c->source);
}

/* Recompute epoll interest for a session's PTY master. */
static void
pty_update (epi_server *srv, epi_session *s)
{
  uint32_t events;
  if (s->pty_master < 0)
    return;
  events = EPOLLIN;
  if (s->pin_len > 0)
    events |= EPOLLOUT;
  ep_mod (srv, s->pty_master, events, &s->pty_source);
}

/* ------------------------------------------------------------------ *
 * session bookkeeping
 * ------------------------------------------------------------------ */

static epi_session *
find_session (epi_server *srv, uint64_t id, int allow_removed)
{
  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    if (s->id == id && (allow_removed || !s->removed))
      return s;
  return NULL;
}

/* ------------------------------------------------------------------ *
 * group bookkeeping
 * ------------------------------------------------------------------ */

static epi_group *
find_group (epi_server *srv, uint64_t id)
{
  for (epi_group *g = srv->groups; g != NULL; g = g->next)
    if (g->id == id)
      return g;
  return NULL;
}

static uint32_t
count_groups (epi_server *srv)
{
  uint32_t n = 0;
  for (epi_group *g = srv->groups; g != NULL; g = g->next)
    n++;
  return n;
}

/* Whether a session counts as a member of gid.
 *
 * Sessions marked `removed` (killed, awaiting reap) are excluded, so the
 * membership GROUP_LIST reports is exactly the set LIST shows. The GUI relies on
 * that to tell a live member from an id in the blob that no longer exists. A
 * session that merely exited is NOT removed: it stays a member, so its
 * arrangement survives and can be shown with the dead pane in place. */
static int
session_in_group (const epi_session *s, uint64_t gid)
{
  return !s->removed && s->group_id == gid;
}

/* How many sessions currently name this group. Membership lives on the sessions,
 * so this walk is the authoritative answer. */
static uint32_t
count_group_members (epi_server *srv, uint64_t gid)
{
  uint32_t n = 0;
  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    if (session_in_group (s, gid))
      n++;
  return n;
}

static void
unlink_group (epi_server *srv, epi_group *g)
{
  for (epi_group **pp = &srv->groups; *pp != NULL; pp = &(*pp)->next)
    if (*pp == g)
      {
        *pp = g->next;
        break;
      }
  epi_group_free (g);
}

/* Destroy the group if it has just lost its last member.
 *
 * Called after anything that can detach a session from a group. A group naming
 * nothing is garbage: the arrangement it holds describes sessions that no longer
 * exist. Note this triggers on the 1 -> 0 transition only, so a group created but
 * not yet populated survives until its first member arrives, which is the normal
 * GROUP_NEW-then-GROUP_ADD construction sequence. */
static void
gc_group_if_empty (epi_server *srv, uint64_t gid)
{
  epi_group *g;

  if (gid == 0)
    return;
  g = find_group (srv, gid);
  if (g == NULL)
    return;
  if (count_group_members (srv, gid) > 0)
    return;

  epi_info ("group %llu: last member gone, removing",
            (unsigned long long) gid);
  unlink_group (srv, g);
}

static void conn_close (epi_server *srv, connection *c);

/* Remove a session from the daemon and queue it for deferred free. Freeing is
 * deferred to the end of the current epoll batch so that any other event still
 * pending in the same batch cannot dereference a freed session. */
static void
remove_session (epi_server *srv, epi_session *s)
{
  epi_session **pp;

  if (s->defunct)
    return;   /* already scheduled */

  if (s->client != NULL)
    {
      s->client->session = NULL;
      s->client->want_close = 1;
      shutdown (s->client->fd, SHUT_RDWR);   /* wake it so its fd closes */
      s->client = NULL;
    }
  if (s->pty_master >= 0)
    {
      ep_del (srv, s->pty_master);
      close (s->pty_master);
      s->pty_master = -1;
    }

  for (pp = &srv->sessions; *pp != NULL; pp = &(*pp)->next)
    if (*pp == s)
      {
        *pp = s->next;
        break;
      }

  s->defunct = 1;
  s->dead_next = srv->dead;
  srv->dead = s;

  /* The session is off the list now, so this sees the post-removal membership.
   * If it was the group's last member, the group goes with it. */
  gc_group_if_empty (srv, s->group_id);
}

/* Free everything queued by remove_session during the current batch. */
static void
reap_dead_sessions (epi_server *srv)
{
  while (srv->dead != NULL)
    {
      epi_session *s = srv->dead;
      srv->dead = s->dead_next;
      epi_session_free (s);
    }
}

/* Detach the currently attached client (if any) without touching the
 * session. */
static void
detach_client (epi_server *srv, epi_session *s)
{
  connection *c = s->client;
  if (c == NULL)
    return;
  s->client = NULL;
  c->session = NULL;
  c->want_close = 1;
  shutdown (c->fd, SHUT_RDWR);   /* prompt the client's fd to close */
  conn_update (srv, c);
}

/* ------------------------------------------------------------------ *
 * request handlers
 * ------------------------------------------------------------------ */

static char **
read_str_array (epi_rd *r, uint32_t *out_count)
{
  uint32_t count = epi_rd_u32 (r);
  char **arr;
  uint32_t i;

  *out_count = 0;
  if (r->err)
    return NULL;
  if (count == 0)
    return NULL;
  if (count > 4096)   /* sanity bound */
    {
      r->err = 1;
      return NULL;
    }

  arr = (char **) calloc ((size_t) count + 1, sizeof *arr);
  if (arr == NULL)
    {
      r->err = 1;
      return NULL;
    }
  for (i = 0; i < count; i++)
    {
      arr[i] = epi_rd_str (r);
      if (arr[i] == NULL)
        {
          for (uint32_t j = 0; j < i; j++)
            free (arr[j]);
          free (arr);
          r->err = 1;
          return NULL;
        }
    }
  arr[count] = NULL;
  *out_count = count;
  return arr;
}

static void
free_str_array (char **arr)
{
  if (arr == NULL)
    return;
  for (size_t i = 0; arr[i] != NULL; i++)
    free (arr[i]);
  free (arr);
}

static void
handle_create (epi_server *srv, connection *c, epi_rd *r)
{
  char *cwd = epi_rd_str (r);
  uint32_t argc = 0, envc = 0;
  char **argv = read_str_array (r, &argc);
  char **env = read_str_array (r, &envc);
  char *exec_path = NULL;
  epi_session *s;

  /* exec_path is an optional trailing field: a frame that stops after the env
   * array is a well-formed CREATE meaning "execute argv[0]". Only read it if
   * bytes actually remain, so `epimone-ctl` and any other client still sending
   * the original four-field body keeps working. */
  if (!r->err && r->pos < r->len)
    exec_path = epi_rd_str (r);

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed CREATE");
      goto out;
    }

  s = epi_session_spawn ((cwd && cwd[0]) ? cwd : NULL, argv, env, exec_path);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_SPAWN, "failed to spawn session");
      goto out;
    }

  s->id = ++srv->next_id;
  s->next = srv->sessions;
  srv->sessions = s;

  if (ep_add (srv, s->pty_master, EPOLLIN, &s->pty_source) != 0)
    {
      epi_err ("epoll add pty failed: %s", strerror (errno));
      remove_session (srv, s);
      send_error (c, EPI_ERR_INTERNAL, "epoll registration failed");
      goto out;
    }

  epi_info ("created session %llu (pid %d)",
            (unsigned long long) s->id, (int) s->pid);

  {
    epi_buf b;
    epi_msg_start (&b, EPI_MSG_ID);
    epi_buf_put_u64 (&b, s->id);
    epi_msg_end (&b);
    conn_queue_frame (c, &b);
  }

out:
  free (cwd);
  free (exec_path);
  free_str_array (argv);
  free_str_array (env);
}

static void
handle_list (epi_server *srv, connection *c)
{
  epi_buf b;
  uint32_t count = 0;

  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    if (!s->removed)
      count++;

  epi_msg_start (&b, EPI_MSG_LIST_REPLY);
  epi_buf_put_u32 (&b, count);
  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    if (!s->removed)
      put_session_info (&b, s);
  /* Optional trailing field, after the array. Pre-Phase-1 clients stop reading
   * once they have consumed `count` elements and never see it; session_info
   * itself stays byte-for-byte as it was, because it is a repeated struct with no
   * length prefix and appending to it would shift every element after the first.
   */
  epi_buf_put_u64 (&b, srv->instance_id);
  epi_msg_end (&b);
  conn_queue_frame (c, &b);
}

static void
handle_attach (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t id = epi_rd_u64 (r);
  epi_session *s;
  epi_buf b;
  uint8_t *snap;
  size_t snap_len = 0;

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed ATTACH");
      return;
    }
  s = find_session (srv, id, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }

  /* A second attach forcibly detaches the first. */
  if (s->client != NULL && s->client != c)
    {
      epi_info ("session %llu: forcibly detaching previous client",
                (unsigned long long) id);
      detach_client (srv, s);
    }

  /* This connection becomes the raw data channel. */
  c->is_data = 1;
  c->session = s;
  s->client = c;

  epi_msg_start (&b, EPI_MSG_ATTACHED);
  put_session_info (&b, s);
  epi_msg_end (&b);
  conn_queue_frame (c, &b);

  /* Replay the ring buffer, then live output follows. */
  snap = epi_ring_snapshot (&s->out, &snap_len);
  if (snap != NULL)
    {
      conn_queue (c, snap, snap_len);
      free (snap);
    }

  epi_info ("session %llu: client attached (%zu bytes replayed)",
            (unsigned long long) id, snap_len);
}

static void
handle_kill (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t id = epi_rd_u64 (r);
  epi_session *s;

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed KILL");
      return;
    }
  s = find_session (srv, id, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }

  detach_client (srv, s);

  if (!s->alive)
    {
      epi_info ("killing dead session %llu (reaping)",
                (unsigned long long) id);
      remove_session (srv, s);
      send_ok (c);
      return;
    }

  /* SIGHUP now; escalate to SIGKILL after the grace period. Hidden from
   * LIST immediately; freed once the child is reaped. */
  epi_info ("killing session %llu (SIGHUP)", (unsigned long long) id);
  epi_session_hangup (s);
  s->killing = 1;
  s->removed = 1;
  clock_gettime (CLOCK_MONOTONIC, &s->kill_deadline);
  s->kill_deadline.tv_sec += KILL_GRACE_MS / 1000;
  /* Being marked removed already ends its membership, so the group may be empty
   * as of now rather than whenever the child is finally reaped. */
  gc_group_if_empty (srv, s->group_id);
  send_ok (c);
}

static void
handle_resize (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t id = epi_rd_u64 (r);
  uint16_t rows = epi_rd_u16 (r);
  uint16_t cols = epi_rd_u16 (r);
  epi_session *s;

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed RESIZE");
      return;
    }
  s = find_session (srv, id, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }
  epi_session_resize (s, rows, cols);
  send_ok (c);
}

static void
handle_detach (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t id = epi_rd_u64 (r);
  epi_session *s;

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed DETACH");
      return;
    }
  s = find_session (srv, id, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }
  detach_client (srv, s);
  epi_info ("session %llu: detached by request", (unsigned long long) id);
  send_ok (c);
}

/* Read-only tail read of a session's ring, for previews.
 *
 * Deliberately side-effect free: it does not set c->is_data, does not bind
 * c->session, does not touch s->client, and does not read or write the PTY. A
 * session with a client attached is left attached and undisturbed; ATTACH
 * cannot be reused here because it forcibly detaches the incumbent and
 * replays the whole ring. */
static void
handle_peek (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t id = epi_rd_u64 (r);
  uint32_t max_bytes = EPI_PEEK_DEFAULT_BYTES;
  epi_session *s;
  epi_buf b;
  uint8_t *tail;
  size_t tail_len = 0;

  /* max_bytes is an optional trailing field, on the same terms as CREATE's
   * exec_path: a frame that stops after the id means "use the default". */
  if (!r->err && r->pos < r->len)
    max_bytes = epi_rd_u32 (r);

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed PEEK");
      return;
    }
  s = find_session (srv, id, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }

  /* A dead-but-listed session is peekable: its ring is exactly what a preview
   * wants to show. max_bytes of 0 is a legal request for nothing and answers
   * with an empty slice; a max_bytes past the ring is clamped to what is
   * held, so the reply is never larger than the ring. */
  tail = epi_ring_snapshot_tail (&s->out, (size_t) max_bytes, &tail_len);

  epi_msg_start (&b, EPI_MSG_PEEK_REPLY);
  epi_buf_put_u64 (&b, s->id);
  epi_buf_put_blob (&b, tail, tail_len);
  /* Optional trailing field: how much the ring holds in total, so a caller can
   * tell "this is the whole session" from "this is the tail of a long one". */
  epi_buf_put_u64 (&b, (uint64_t) s->out.size);
  epi_msg_end (&b);
  conn_queue_frame (c, &b);
  free (tail);
}

/* ------------------------------------------------------------------ *
 * group requests
 *
 * The blob passes through these handlers untouched: read into a buffer, memcpy'd
 * into the group, memcpy'd back out on GROUP_LIST. Nothing here looks at a byte
 * of it, and nothing may start to; the GUI owns that encoding.
 * ------------------------------------------------------------------ */

static void
handle_group_new (epi_server *srv, connection *c, epi_rd *r)
{
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  epi_group *g;
  epi_buf b;

  /* The blob is an optional trailing field: GROUP_NEW with an empty body creates
   * a group with an empty blob, to be filled in later by GROUP_SET. */
  if (!r->err && r->pos < r->len)
    blob = epi_rd_blob (r, &blob_len);

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed GROUP_NEW");
      goto out;
    }
  if (blob_len > EPI_GROUP_BLOB_MAX)
    {
      send_error (c, EPI_ERR_TOO_LARGE, "blob exceeds maximum size");
      goto out;
    }
  if (count_groups (srv) >= EPI_GROUP_MAX)
    {
      send_error (c, EPI_ERR_TOO_LARGE, "too many groups");
      goto out;
    }

  g = epi_group_new (blob, blob_len);
  if (g == NULL)
    {
      send_error (c, EPI_ERR_INTERNAL, "out of memory");
      goto out;
    }
  g->id = ++srv->next_group_id;
  g->next = srv->groups;
  srv->groups = g;

  epi_info ("created group %llu (%zu byte blob)",
            (unsigned long long) g->id, blob_len);

  epi_msg_start (&b, EPI_MSG_GROUP_ID);
  epi_buf_put_u64 (&b, g->id);
  epi_msg_end (&b);
  conn_queue_frame (c, &b);

out:
  free (blob);
}

static void
handle_group_set (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t gid = epi_rd_u64 (r);
  uint8_t *blob = NULL;
  size_t blob_len = 0;
  epi_group *g;

  blob = epi_rd_blob (r, &blob_len);

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed GROUP_SET");
      goto out;
    }
  if (blob_len > EPI_GROUP_BLOB_MAX)
    {
      send_error (c, EPI_ERR_TOO_LARGE, "blob exceeds maximum size");
      goto out;
    }
  g = find_group (srv, gid);
  if (g == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such group");
      goto out;
    }
  if (epi_group_set_blob (g, blob, blob_len) != 0)
    {
      send_error (c, EPI_ERR_INTERNAL, "out of memory");
      goto out;
    }

  send_ok (c);

out:
  free (blob);
}

static void
handle_group_add (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t gid = epi_rd_u64 (r);
  uint64_t sid = epi_rd_u64 (r);
  epi_session *s;
  uint64_t was;

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed GROUP_ADD");
      return;
    }
  if (find_group (srv, gid) == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such group");
      return;
    }
  s = find_session (srv, sid, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }

  /* One step, so a move between groups never passes through a state where the
   * session belongs to nothing, which would destroy the source group if this
   * were its last member and then leave the caller with a dangling gid. */
  was = s->group_id;
  s->group_id = gid;
  if (was != gid)
    gc_group_if_empty (srv, was);

  send_ok (c);
}

static void
handle_group_remove (epi_server *srv, connection *c, epi_rd *r)
{
  uint64_t sid = epi_rd_u64 (r);
  epi_session *s;
  uint64_t was;

  if (r->err)
    {
      send_error (c, EPI_ERR_PROTOCOL, "malformed GROUP_REMOVE");
      return;
    }
  s = find_session (srv, sid, 0);
  if (s == NULL)
    {
      send_error (c, EPI_ERR_NOT_FOUND, "no such session");
      return;
    }

  was = s->group_id;
  s->group_id = 0;
  gc_group_if_empty (srv, was);

  send_ok (c);
}

static void
handle_group_list (epi_server *srv, connection *c)
{
  epi_buf b;

  epi_msg_start (&b, EPI_MSG_GROUP_LIST_REPLY);
  epi_buf_put_u64 (&b, srv->instance_id);
  epi_buf_put_u32 (&b, count_groups (srv));

  for (epi_group *g = srv->groups; g != NULL; g = g->next)
    {
      epi_buf_put_u64 (&b, g->id);
      epi_buf_put_u64 (&b, (uint64_t) g->created_at);
      epi_buf_put_blob (&b, g->blob, g->blob_len);
      epi_buf_put_u32 (&b, count_group_members (srv, g->id));
      for (epi_session *s = srv->sessions; s != NULL; s = s->next)
        if (session_in_group (s, g->id))
          epi_buf_put_u64 (&b, s->id);
    }

  epi_msg_end (&b);
  conn_queue_frame (c, &b);
}

/* Dispatch one complete control frame. */
static void
dispatch_frame (epi_server *srv, connection *c, uint32_t type,
                const uint8_t *body, size_t body_len)
{
  epi_rd r;
  epi_rd_init (&r, body, body_len);

  switch (type)
    {
    case EPI_MSG_CREATE:  handle_create (srv, c, &r); break;
    case EPI_MSG_LIST:    handle_list (srv, c); break;
    case EPI_MSG_ATTACH:  handle_attach (srv, c, &r); break;
    case EPI_MSG_KILL:    handle_kill (srv, c, &r); break;
    case EPI_MSG_RESIZE:  handle_resize (srv, c, &r); break;
    case EPI_MSG_DETACH:  handle_detach (srv, c, &r); break;
    case EPI_MSG_PEEK:    handle_peek (srv, c, &r); break;
    case EPI_MSG_GROUP_NEW:    handle_group_new (srv, c, &r); break;
    case EPI_MSG_GROUP_SET:    handle_group_set (srv, c, &r); break;
    case EPI_MSG_GROUP_ADD:    handle_group_add (srv, c, &r); break;
    case EPI_MSG_GROUP_REMOVE: handle_group_remove (srv, c, &r); break;
    case EPI_MSG_GROUP_LIST:   handle_group_list (srv, c); break;
    default:
      send_error (c, EPI_ERR_PROTOCOL, "unknown message type");
      break;
    }
}

/* ------------------------------------------------------------------ *
 * connection I/O
 * ------------------------------------------------------------------ */

static void
conn_flush (epi_server *srv, connection *c)
{
  while (c->wpos < c->wlen)
    {
      ssize_t n = write (c->fd, c->wbuf + c->wpos, c->wlen - c->wpos);
      if (n > 0)
        {
          c->wpos += (size_t) n;
          continue;
        }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        break;
      if (n < 0 && errno == EINTR)
        continue;
      /* peer gone */
      c->want_close = 1;
      return;
    }
  if (c->wpos == c->wlen)
    {
      c->wpos = 0;
      c->wlen = 0;
    }
  (void) srv;
}

/* Feed raw client bytes to the bound session's PTY. */
static void
data_to_pty (epi_server *srv, connection *c, const uint8_t *data, size_t len)
{
  if (c->session == NULL)
    return;
  epi_session_queue_input (c->session, data, len);
  pty_update (srv, c->session);
}

static void
conn_read (epi_server *srv, connection *c)
{
  uint8_t tmp[PTY_READ_CHUNK];
  ssize_t n;

  n = read (c->fd, tmp, sizeof tmp);
  if (n == 0)
    {
      c->want_close = 1;
      return;
    }
  if (n < 0)
    {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
        return;
      c->want_close = 1;
      return;
    }

  if (c->is_data)
    {
      data_to_pty (srv, c, tmp, (size_t) n);
      return;
    }

  /* Control connection: accumulate and parse length-prefixed frames. */
  if (c->rlen + (size_t) n > c->rcap)
    {
      size_t nc = c->rcap ? c->rcap : 4096;
      uint8_t *nd;
      while (nc < c->rlen + (size_t) n)
        nc *= 2;
      nd = (uint8_t *) realloc (c->rbuf, nc);
      if (nd == NULL)
        {
          c->want_close = 1;
          return;
        }
      c->rbuf = nd;
      c->rcap = nc;
    }
  memcpy (c->rbuf + c->rlen, tmp, (size_t) n);
  c->rlen += (size_t) n;

  {
    size_t used = 0;
    while (c->rlen - used >= 4)
      {
        const uint8_t *p = c->rbuf + used;
        uint32_t flen = (uint32_t) p[0] | ((uint32_t) p[1] << 8)
                      | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
        uint32_t type;

        if (flen < 4)   /* must at least contain the type word */
          {
            send_error (c, EPI_ERR_PROTOCOL, "short frame");
            c->want_close = 1;
            break;
          }
        if (flen > MAX_FRAME)
          {
            send_error (c, EPI_ERR_TOO_LARGE, "frame too large");
            c->want_close = 1;
            break;
          }
        if (c->rlen - used - 4 < flen)
          break;        /* wait for the rest */

        type = (uint32_t) p[4] | ((uint32_t) p[5] << 8)
             | ((uint32_t) p[6] << 16) | ((uint32_t) p[7] << 24);

        dispatch_frame (srv, c, type, p + 8, flen - 4);
        used += 4 + flen;

        if (c->is_data)
          {
            /* ATTACH flipped this connection to a data channel; any trailing
             * bytes are raw keyboard input. */
            if (c->rlen > used)
              data_to_pty (srv, c, c->rbuf + used, c->rlen - used);
            used = c->rlen;
            free (c->rbuf);
            c->rbuf = NULL;
            c->rlen = c->rcap = 0;
            return;
          }
        if (c->want_close)
          break;
      }

    if (used > 0 && c->rbuf != NULL)
      {
        memmove (c->rbuf, c->rbuf + used, c->rlen - used);
        c->rlen -= used;
      }
  }
}

static void
conn_close (epi_server *srv, connection *c)
{
  connection **pp;

  if (c->session != NULL && c->session->client == c)
    {
      /* Client disconnect == clean DETACH; session keeps running. */
      c->session->client = NULL;
      epi_info ("session %llu: client disconnected (detached)",
                (unsigned long long) c->session->id);
    }

  ep_del (srv, c->fd);
  close (c->fd);

  for (pp = &srv->conns; *pp != NULL; pp = &(*pp)->next)
    if (*pp == c)
      {
        *pp = c->next;
        break;
      }

  free (c->rbuf);
  free (c->wbuf);
  free (c);
}

static void
accept_conn (epi_server *srv)
{
  for (;;)
    {
      int fd = accept4 (srv->listen_fd, NULL, NULL,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
      connection *c;

      if (fd < 0)
        {
          if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
          if (errno == EINTR)
            continue;
          epi_warn ("accept failed: %s", strerror (errno));
          return;
        }

      c = (connection *) calloc (1, sizeof *c);
      if (c == NULL)
        {
          close (fd);
          continue;
        }
      c->fd = fd;
      c->srv = srv;
      c->source.kind = EPI_SRC_CONN;
      c->source.obj = c;
      c->next = srv->conns;
      srv->conns = c;

      if (ep_add (srv, fd, EPOLLIN | EPOLLRDHUP, &c->source) != 0)
        {
          epi_warn ("epoll add conn failed: %s", strerror (errno));
          conn_close (srv, c);
        }
    }
}

/* ------------------------------------------------------------------ *
 * PTY I/O
 * ------------------------------------------------------------------ */

/* The session's child has exited. If a client is still attached, flush any
 * remaining output and then close the data channel so the client observes
 * EOF (the GUI maps this to "shell exited"). The session itself stays listed
 * as dead until an explicit KILL. */
static void
client_eof (epi_server *srv, epi_session *s)
{
  connection *c = s->client;

  if (c == NULL)
    return;

  conn_flush (srv, c);
  c->want_close = 1;
  if (c->wpos == c->wlen)
    shutdown (c->fd, SHUT_RDWR);   /* nothing pending: send FIN now */
  conn_update (srv, c);            /* else drain via EPOLLOUT, then close */
}

static void
pty_readable (epi_server *srv, epi_session *s)
{
  uint8_t tmp[PTY_READ_CHUNK];

  if (s->defunct || s->pty_master < 0)
    return;

  for (;;)
    {
      ssize_t n = read (s->pty_master, tmp, sizeof tmp);
      if (n > 0)
        {
          epi_ring_write (&s->out, tmp, (size_t) n);
          if (s->client != NULL)
            {
              conn_queue (s->client, tmp, (size_t) n);
              conn_flush (srv, s->client);
              conn_update (srv, s->client);
            }
          continue;
        }
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return;
      if (n < 0 && errno == EINTR)
        continue;

      /* n == 0 or EIO: the slave side is gone. Stop watching the master. */
      ep_del (srv, s->pty_master);
      close (s->pty_master);
      s->pty_master = -1;
      if (s->removed)
        remove_session (srv, s);
      else
        client_eof (srv, s);   /* signal shell exit to an attached client */
      return;
    }
}

/* ------------------------------------------------------------------ *
 * signals
 * ------------------------------------------------------------------ */

static void
reap_children (epi_server *srv)
{
  int status;
  pid_t pid;

  while ((pid = waitpid (-1, &status, WNOHANG)) > 0)
    {
      epi_session *s = NULL;
      for (epi_session *it = srv->sessions; it != NULL; it = it->next)
        if (it->pid == pid)
          {
            s = it;
            break;
          }
      if (s == NULL)
        continue;

      s->alive = false;
      s->exit_status = status;
      epi_info ("session %llu (pid %d) exited",
                (unsigned long long) s->id, (int) pid);

      /* A killed session is dropped entirely once reaped. */
      if (s->removed)
        remove_session (srv, s);
      /* Otherwise keep it listed as dead with its ring buffer intact. */
    }
}

static void
handle_signalfd (epi_server *srv)
{
  struct signalfd_siginfo si;

  for (;;)
    {
      ssize_t n = read (srv->sig_fd, &si, sizeof si);
      if (n != (ssize_t) sizeof si)
        return;

      switch (si.ssi_signo)
        {
        case SIGCHLD:
          reap_children (srv);
          break;
        case SIGTERM:
        case SIGINT:
          epi_info ("received signal %u, shutting down", si.ssi_signo);
          srv->running = 0;
          return;
        default:
          break;
        }
    }
}

/* ------------------------------------------------------------------ *
 * kill-escalation timing
 * ------------------------------------------------------------------ */

static long
ms_until (const struct timespec *deadline)
{
  struct timespec now;
  long ms;
  clock_gettime (CLOCK_MONOTONIC, &now);
  ms = (deadline->tv_sec - now.tv_sec) * 1000
     + (deadline->tv_nsec - now.tv_nsec) / 1000000;
  return ms;
}

/* Returns the epoll timeout (ms) for the nearest kill deadline, or -1. */
static int
next_timeout (epi_server *srv)
{
  long best = -1;
  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    if (s->killing && s->alive)
      {
        long ms = ms_until (&s->kill_deadline);
        if (ms < 0)
          ms = 0;
        if (best < 0 || ms < best)
          best = ms;
      }
  return (best < 0) ? -1 : (int) best;
}

static void
process_kill_deadlines (epi_server *srv)
{
  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    if (s->killing && s->alive && ms_until (&s->kill_deadline) <= 0)
      {
        epi_info ("session %llu: escalating to SIGKILL",
                  (unsigned long long) s->id);
        epi_session_kill (s);
        s->killing = 0;   /* avoid repeated SIGKILL; SIGCHLD will reap */
      }
}

/* ------------------------------------------------------------------ *
 * lifecycle
 * ------------------------------------------------------------------ */

/* A value identifying this daemon process, so a client holding session or group
 * ids from an earlier daemon can tell that the ids no longer mean what it thinks.
 * Ids are handed out from a counter that starts at zero on every start, so after
 * a restart id 3 names an unrelated shell; this is what makes that detectable.
 *
 * Deliberately NOT an attempt to make ids stable across restarts; the sessions
 * themselves do not survive a restart, so there would be nothing to point at.
 *
 * getrandom(2) is libc, so this adds no dependency. It cannot meaningfully fail
 * for 8 bytes without GRND_NONBLOCK, but if it does, the clock and pid still give
 * a value that differs between daemon starts, which is all that is required. */
static uint64_t
make_instance_id (void)
{
  uint64_t v = 0;

  if (getrandom (&v, sizeof v, 0) == (ssize_t) sizeof v && v != 0)
    return v;

  {
    struct timespec ts;
    clock_gettime (CLOCK_REALTIME, &ts);
    v = ((uint64_t) ts.tv_sec << 32) ^ (uint64_t) ts.tv_nsec
      ^ ((uint64_t) getpid () << 16);
  }
  return v ? v : 1;   /* zero is reserved for "no instance id reported" */
}

int
epi_server_init (epi_server *srv, const char *sockpath)
{
  struct sockaddr_un addr;
  sigset_t mask;

  memset (srv, 0, sizeof *srv);
  srv->listen_fd = -1;
  srv->epfd = -1;
  srv->sig_fd = -1;
  srv->running = 1;
  srv->instance_id = make_instance_id ();
  snprintf (srv->sockpath, sizeof srv->sockpath, "%s", sockpath);

  signal (SIGPIPE, SIG_IGN);

  srv->epfd = epoll_create1 (EPOLL_CLOEXEC);
  if (srv->epfd < 0)
    {
      epi_err ("epoll_create1: %s", strerror (errno));
      return -1;
    }

  srv->listen_fd = socket (AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (srv->listen_fd < 0)
    {
      epi_err ("socket: %s", strerror (errno));
      return -1;
    }

  memset (&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen (sockpath) >= sizeof addr.sun_path)
    {
      epi_err ("socket path too long: %s", sockpath);
      return -1;
    }
  strcpy (addr.sun_path, sockpath);

  if (bind (srv->listen_fd, (struct sockaddr *) &addr, sizeof addr) != 0)
    {
      epi_err ("bind %s: %s", sockpath, strerror (errno));
      return -1;
    }
  if (listen (srv->listen_fd, 16) != 0)
    {
      epi_err ("listen: %s", strerror (errno));
      return -1;
    }

  srv->listen_source.kind = EPI_SRC_LISTEN;
  srv->listen_source.obj = srv;
  if (ep_add (srv, srv->listen_fd, EPOLLIN, &srv->listen_source) != 0)
    {
      epi_err ("epoll add listen: %s", strerror (errno));
      return -1;
    }

  /* Block the signals we want to receive via signalfd. */
  sigemptyset (&mask);
  sigaddset (&mask, SIGCHLD);
  sigaddset (&mask, SIGTERM);
  sigaddset (&mask, SIGINT);
  if (sigprocmask (SIG_BLOCK, &mask, NULL) != 0)
    {
      epi_err ("sigprocmask: %s", strerror (errno));
      return -1;
    }
  srv->sig_fd = signalfd (-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (srv->sig_fd < 0)
    {
      epi_err ("signalfd: %s", strerror (errno));
      return -1;
    }
  srv->sig_source.kind = EPI_SRC_SIGNAL;
  srv->sig_source.obj = srv;
  if (ep_add (srv, srv->sig_fd, EPOLLIN, &srv->sig_source) != 0)
    {
      epi_err ("epoll add signalfd: %s", strerror (errno));
      return -1;
    }

  epi_info ("listening on %s", sockpath);
  return 0;
}

void
epi_server_run (epi_server *srv)
{
  struct epoll_event events[64];

  while (srv->running)
    {
      int timeout = next_timeout (srv);
      int nfds = epoll_wait (srv->epfd, events, 64, timeout);

      if (nfds < 0)
        {
          if (errno == EINTR)
            continue;
          epi_err ("epoll_wait: %s", strerror (errno));
          break;
        }

      for (int i = 0; i < nfds; i++)
        {
          struct epi_source *src = events[i].data.ptr;
          uint32_t ev = events[i].events;

          switch (src->kind)
            {
            case EPI_SRC_LISTEN:
              accept_conn (srv);
              break;

            case EPI_SRC_SIGNAL:
              handle_signalfd (srv);
              break;

            case EPI_SRC_PTY:
              {
                epi_session *s = src->obj;
                if (s->defunct)
                  break;   /* freed earlier in this same batch */
                if (ev & EPOLLOUT)
                  {
                    epi_session_flush_input (s);
                    if (s->pty_master >= 0)
                      pty_update (srv, s);
                  }
                if (ev & (EPOLLIN | EPOLLHUP | EPOLLERR))
                  pty_readable (srv, s);
              }
              break;

            case EPI_SRC_CONN:
              {
                connection *c = src->obj;
                if (ev & EPOLLOUT)
                  conn_flush (srv, c);
                if (ev & (EPOLLIN | EPOLLRDHUP | EPOLLHUP | EPOLLERR))
                  conn_read (srv, c);
                if (!c->want_close)
                  conn_update (srv, c);
                if (c->want_close && c->wlen == c->wpos)
                  conn_close (srv, c);
              }
              break;
            }
        }

      reap_dead_sessions (srv);
      process_kill_deadlines (srv);
    }
}

void
epi_server_shutdown (epi_server *srv)
{
  epi_info ("hanging up all sessions");
  for (epi_session *s = srv->sessions; s != NULL; s = s->next)
    epi_session_hangup (s);

  /* Best-effort: free connections and sessions. */
  while (srv->conns != NULL)
    {
      connection *c = srv->conns;
      srv->conns = c->next;
      if (c->fd >= 0)
        close (c->fd);
      free (c->rbuf);
      free (c->wbuf);
      free (c);
    }
  while (srv->sessions != NULL)
    {
      epi_session *s = srv->sessions;
      srv->sessions = s->next;
      epi_session_free (s);
    }
  reap_dead_sessions (srv);
  while (srv->groups != NULL)
    {
      epi_group *g = srv->groups;
      srv->groups = g->next;
      epi_group_free (g);
    }

  if (srv->sig_fd >= 0)
    close (srv->sig_fd);
  if (srv->listen_fd >= 0)
    close (srv->listen_fd);
  if (srv->epfd >= 0)
    close (srv->epfd);
  if (srv->sockpath[0])
    unlink (srv->sockpath);
}
