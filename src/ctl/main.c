/*
 * epimone-ctl: a tiny standalone client / debug harness for epimone-daemon.
 *
 *   epimone-ctl create [cwd]
 *   epimone-ctl list
 *   epimone-ctl attach <id>          (raw passthrough; Ctrl+] detaches)
 *   epimone-ctl kill <id>
 *   epimone-ctl resize <id> <rows> <cols>
 *   epimone-ctl peek <id> [bytes]    (raw tail to stdout; no attach, no detach)
 *   epimone-ctl group-new [blob|-]   (blob of "-" is read from stdin)
 *   epimone-ctl group-set <gid> <blob|->
 *   epimone-ctl group-list
 *   epimone-ctl group-blob <gid>     (exact bytes to stdout)
 *   epimone-ctl group-add <gid> <sid>
 *   epimone-ctl group-remove <sid>
 *   epimone-ctl instance-id
 */
#include "../epimone-protocol.h"
#include "../daemon/paths.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define CTRL_RBRACKET 0x1d   /* Ctrl+] */

static int connect_daemon (void);
static int write_full (int fd, const void *buf, size_t len);
static int read_full (int fd, void *buf, size_t len);
static int send_frame (int fd, epi_buf *b);
static int recv_frame (int fd, uint32_t *type, uint8_t **body, size_t *blen);

/* ---- helpers ------------------------------------------------------ */

static int
connect_daemon (void)
{
  char path[512];
  struct sockaddr_un addr;
  int fd;

  if (epi_socket_path (path, sizeof path) != 0)
    {
      fprintf (stderr, "epimone-ctl: cannot determine socket path\n");
      return -1;
    }

  fd = socket (AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    {
      perror ("socket");
      return -1;
    }
  memset (&addr, 0, sizeof addr);
  addr.sun_family = AF_UNIX;
  if (strlen (path) >= sizeof addr.sun_path)
    {
      fprintf (stderr, "epimone-ctl: socket path too long\n");
      close (fd);
      return -1;
    }
  strcpy (addr.sun_path, path);
  if (connect (fd, (struct sockaddr *) &addr, sizeof addr) != 0)
    {
      fprintf (stderr, "epimone-ctl: cannot connect to %s: %s\n",
               path, strerror (errno));
      close (fd);
      return -1;
    }
  return fd;
}

static int
write_full (int fd, const void *buf, size_t len)
{
  const uint8_t *p = (const uint8_t *) buf;
  size_t off = 0;
  while (off < len)
    {
      ssize_t n = write (fd, p + off, len - off);
      if (n > 0)
        off += (size_t) n;
      else if (n < 0 && errno == EINTR)
        continue;
      else
        return -1;
    }
  return 0;
}

static int
read_full (int fd, void *buf, size_t len)
{
  uint8_t *p = (uint8_t *) buf;
  size_t off = 0;
  while (off < len)
    {
      ssize_t n = read (fd, p + off, len - off);
      if (n > 0)
        off += (size_t) n;
      else if (n == 0)
        return -1;   /* short */
      else if (errno == EINTR)
        continue;
      else
        return -1;
    }
  return 0;
}

static int
send_frame (int fd, epi_buf *b)
{
  int rc;
  if (b->oom)
    {
      epi_buf_free (b);
      return -1;
    }
  rc = write_full (fd, b->data, b->len);
  epi_buf_free (b);
  return rc;
}

static int
recv_frame (int fd, uint32_t *type, uint8_t **body, size_t *blen)
{
  uint8_t hdr[4];
  uint32_t flen;
  uint8_t *raw;

  if (read_full (fd, hdr, 4) != 0)
    return -1;
  flen = (uint32_t) hdr[0] | ((uint32_t) hdr[1] << 8)
       | ((uint32_t) hdr[2] << 16) | ((uint32_t) hdr[3] << 24);
  if (flen < 4)
    return -1;

  raw = (uint8_t *) malloc (flen);
  if (raw == NULL)
    return -1;
  if (read_full (fd, raw, flen) != 0)
    {
      free (raw);
      return -1;
    }
  *type = (uint32_t) raw[0] | ((uint32_t) raw[1] << 8)
        | ((uint32_t) raw[2] << 16) | ((uint32_t) raw[3] << 24);
  *blen = flen - 4;
  *body = (uint8_t *) malloc (*blen ? *blen : 1);
  if (*body == NULL)
    {
      free (raw);
      return -1;
    }
  memcpy (*body, raw + 4, *blen);
  free (raw);
  return 0;
}

/* Read a reply and, if it is an ERROR, print it. Returns the type. */
static uint32_t
read_reply (int fd, uint8_t **body, size_t *blen)
{
  uint32_t type;
  if (recv_frame (fd, &type, body, blen) != 0)
    {
      fprintf (stderr, "epimone-ctl: no reply from daemon\n");
      return 0;
    }
  if (type == EPI_MSG_ERROR)
    {
      epi_rd r;
      uint32_t code;
      char *msg;
      epi_rd_init (&r, *body, *blen);
      code = epi_rd_u32 (&r);
      msg = epi_rd_str (&r);
      fprintf (stderr, "epimone-ctl: error %u: %s\n", code, msg ? msg : "?");
      free (msg);
    }
  return type;
}

/* ---- subcommands -------------------------------------------------- */

static int
cmd_create (int argc, char **argv)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;
  char cwdbuf[4096];
  const char *cwd;

  if (fd < 0)
    return 1;

  if (argc > 0)
    cwd = argv[0];
  else
    cwd = getcwd (cwdbuf, sizeof cwdbuf);

  epi_msg_start (&b, EPI_MSG_CREATE);
  epi_buf_put_str (&b, cwd ? cwd : "");
  epi_buf_put_u32 (&b, 0);   /* argc: use $SHELL */
  epi_buf_put_u32 (&b, 0);   /* envc: inherit */
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }

  type = read_reply (fd, &body, &blen);
  if (type == EPI_MSG_ID)
    {
      epi_rd r;
      epi_rd_init (&r, body, blen);
      printf ("%llu\n", (unsigned long long) epi_rd_u64 (&r));
    }
  free (body);
  close (fd);
  return (type == EPI_MSG_ID) ? 0 : 1;
}

static int
cmd_list (void)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;

  if (fd < 0)
    return 1;

  epi_msg_start (&b, EPI_MSG_LIST);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }

  type = read_reply (fd, &body, &blen);
  if (type == EPI_MSG_LIST_REPLY)
    {
      epi_rd r;
      uint32_t count, i;
      epi_rd_init (&r, body, blen);
      count = epi_rd_u32 (&r);
      printf ("%-6s %-8s %-6s %-8s %-20s %s\n",
              "ID", "PID", "ALIVE", "ATTACHED", "CREATED", "CWD");
      for (i = 0; i < count && !r.err; i++)
        {
          uint64_t id = epi_rd_u64 (&r);
          uint32_t pid = epi_rd_u32 (&r);
          uint8_t alive = epi_rd_u8 (&r);
          uint8_t attached = epi_rd_u8 (&r);
          uint64_t created = epi_rd_u64 (&r);
          char *cwd = epi_rd_str (&r);
          char ts[20];
          time_t t = (time_t) created;
          struct tm tm;
          localtime_r (&t, &tm);
          strftime (ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);
          printf ("%-6llu %-8u %-6s %-8s %-20s %s\n",
                  (unsigned long long) id, pid,
                  alive ? "yes" : "no", attached ? "yes" : "no",
                  ts, cwd ? cwd : "");
          free (cwd);
        }
    }
  free (body);
  close (fd);
  return (type == EPI_MSG_LIST_REPLY) ? 0 : 1;
}

static int
simple_id_cmd (uint32_t msg_type, uint64_t id)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;

  if (fd < 0)
    return 1;

  epi_msg_start (&b, msg_type);
  epi_buf_put_u64 (&b, id);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }
  type = read_reply (fd, &body, &blen);
  free (body);
  close (fd);
  return (type == EPI_MSG_OK) ? 0 : 1;
}

static int
send_resize (uint64_t id, uint16_t rows, uint16_t cols)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;

  if (fd < 0)
    return 1;
  epi_msg_start (&b, EPI_MSG_RESIZE);
  epi_buf_put_u64 (&b, id);
  epi_buf_put_u16 (&b, rows);
  epi_buf_put_u16 (&b, cols);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }
  type = read_reply (fd, &body, &blen);
  free (body);
  close (fd);
  return (type == EPI_MSG_OK) ? 0 : 1;
}

/* Dump the tail of a session's scrollback to stdout, raw. Unlike attach this
 * leaves the session's attached client (if any) alone. The bytes are unfiltered
 * PTY output including escape sequences, so redirect to a file or a pager if the
 * terminal's state matters. */
static int
cmd_peek (uint64_t id, uint32_t max_bytes)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;
  int rc = 1;

  if (fd < 0)
    return 1;

  epi_msg_start (&b, EPI_MSG_PEEK);
  epi_buf_put_u64 (&b, id);
  epi_buf_put_u32 (&b, max_bytes);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }

  type = read_reply (fd, &body, &blen);
  if (type == EPI_MSG_PEEK_REPLY)
    {
      epi_rd r;
      uint8_t *tail;
      size_t tlen = 0;

      epi_rd_init (&r, body, blen);
      (void) epi_rd_u64 (&r);        /* echoed session id */
      tail = epi_rd_blob (&r, &tlen);
      if (r.err)
        {
          fprintf (stderr, "epimone-ctl: malformed PEEK reply\n");
        }
      else
        {
          /* tlen == 0 (nothing buffered yet) is a success with no output. */
          rc = (tlen == 0 || write_full (STDOUT_FILENO, tail, tlen) == 0) ? 0 : 1;
        }
      free (tail);
    }
  free (body);
  close (fd);
  return rc;
}

/* ---- groups ------------------------------------------------------- */

/* Read a blob argument. "-" means stdin, so binary blobs can be piped in for
 * testing; anything else is used as literal bytes. Returns 0 on success. */
static int
read_blob_arg (const char *arg, uint8_t **out, size_t *out_len)
{
  size_t cap, len = 0;
  uint8_t *buf;

  *out = NULL;
  *out_len = 0;

  if (arg == NULL)
    return 0;

  if (strcmp (arg, "-") != 0)
    {
      len = strlen (arg);
      if (len == 0)
        return 0;
      buf = (uint8_t *) malloc (len);
      if (buf == NULL)
        return -1;
      memcpy (buf, arg, len);
      *out = buf;
      *out_len = len;
      return 0;
    }

  cap = 65536;
  buf = (uint8_t *) malloc (cap);
  if (buf == NULL)
    return -1;
  for (;;)
    {
      ssize_t n;
      if (len == cap)
        {
          uint8_t *nb = (uint8_t *) realloc (buf, cap * 2);
          if (nb == NULL)
            {
              free (buf);
              return -1;
            }
          buf = nb;
          cap *= 2;
        }
      n = read (STDIN_FILENO, buf + len, cap - len);
      if (n > 0)
        len += (size_t) n;
      else if (n == 0)
        break;
      else if (errno == EINTR)
        continue;
      else
        {
          free (buf);
          return -1;
        }
    }
  *out = buf;
  *out_len = len;
  return 0;
}

/* Print a blob with non-printables escaped, so `group-list` is always safe to
 * look at even when the blob is binary. Use `group-blob` for exact bytes. */
static void
print_blob_escaped (const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; i++)
    {
      if (p[i] >= 0x20 && p[i] < 0x7f && p[i] != '\\')
        putchar (p[i]);
      else if (p[i] == '\\')
        fputs ("\\\\", stdout);
      else
        printf ("\\x%02x", p[i]);
    }
}

static int
cmd_group_new (const char *blob_arg)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL, *blob = NULL;
  size_t blen = 0, blob_len = 0;
  uint32_t type;

  if (fd < 0)
    return 1;
  if (read_blob_arg (blob_arg, &blob, &blob_len) != 0)
    {
      fprintf (stderr, "epimone-ctl: cannot read blob\n");
      close (fd);
      return 1;
    }

  epi_msg_start (&b, EPI_MSG_GROUP_NEW);
  epi_buf_put_blob (&b, blob, blob_len);
  epi_msg_end (&b);
  free (blob);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }

  type = read_reply (fd, &body, &blen);
  if (type == EPI_MSG_GROUP_ID)
    {
      epi_rd r;
      epi_rd_init (&r, body, blen);
      printf ("%llu\n", (unsigned long long) epi_rd_u64 (&r));
    }
  free (body);
  close (fd);
  return (type == EPI_MSG_GROUP_ID) ? 0 : 1;
}

static int
cmd_group_set (uint64_t gid, const char *blob_arg)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL, *blob = NULL;
  size_t blen = 0, blob_len = 0;
  uint32_t type;

  if (fd < 0)
    return 1;
  if (read_blob_arg (blob_arg, &blob, &blob_len) != 0)
    {
      fprintf (stderr, "epimone-ctl: cannot read blob\n");
      close (fd);
      return 1;
    }

  epi_msg_start (&b, EPI_MSG_GROUP_SET);
  epi_buf_put_u64 (&b, gid);
  epi_buf_put_blob (&b, blob, blob_len);
  epi_msg_end (&b);
  free (blob);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }

  type = read_reply (fd, &body, &blen);
  free (body);
  close (fd);
  return (type == EPI_MSG_OK) ? 0 : 1;
}

/* Send GROUP_LIST and hand the reply body back to the caller. */
static uint32_t
group_list_txn (int *fd_out, uint8_t **body, size_t *blen)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint32_t type;

  *fd_out = fd;
  *body = NULL;
  *blen = 0;
  if (fd < 0)
    return 0;

  epi_msg_start (&b, EPI_MSG_GROUP_LIST);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    return 0;
  type = read_reply (fd, body, blen);
  return type;
}

static int
cmd_group_list (void)
{
  int fd;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type = group_list_txn (&fd, &body, &blen);

  if (type == EPI_MSG_GROUP_LIST_REPLY)
    {
      epi_rd r;
      uint32_t count, i;

      epi_rd_init (&r, body, blen);
      printf ("instance %llu\n", (unsigned long long) epi_rd_u64 (&r));
      count = epi_rd_u32 (&r);
      printf ("%-6s %-20s %-8s %-16s %s\n",
              "GID", "CREATED", "BLOBLEN", "MEMBERS", "BLOB");
      for (i = 0; i < count && !r.err; i++)
        {
          uint64_t gid = epi_rd_u64 (&r);
          uint64_t created = epi_rd_u64 (&r);
          size_t bloblen = 0;
          uint8_t *blob = epi_rd_blob (&r, &bloblen);
          uint32_t members, m;
          char ts[20];
          time_t t = (time_t) created;
          struct tm tm;
          char mbuf[256];
          size_t moff = 0;

          localtime_r (&t, &tm);
          strftime (ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

          members = epi_rd_u32 (&r);
          mbuf[0] = '\0';
          for (m = 0; m < members && !r.err; m++)
            {
              uint64_t sid = epi_rd_u64 (&r);
              int n = snprintf (mbuf + moff, sizeof mbuf - moff,
                                "%s%llu", m ? "," : "",
                                (unsigned long long) sid);
              if (n > 0 && (size_t) n < sizeof mbuf - moff)
                moff += (size_t) n;
            }

          printf ("%-6llu %-20s %-8zu %-16s ",
                  (unsigned long long) gid, ts, bloblen,
                  mbuf[0] ? mbuf : "-");
          print_blob_escaped (blob, bloblen);
          putchar ('\n');
          free (blob);
        }
    }
  free (body);
  if (fd >= 0)
    close (fd);
  return (type == EPI_MSG_GROUP_LIST_REPLY) ? 0 : 1;
}

/* Write one group's blob to stdout as exact bytes, for byte-for-byte checks
 * (`epimone-ctl group-blob 1 | cmp - original.bin`). */
static int
cmd_group_blob (uint64_t gid)
{
  int fd;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type = group_list_txn (&fd, &body, &blen);
  int rc = 1;

  if (type == EPI_MSG_GROUP_LIST_REPLY)
    {
      epi_rd r;
      uint32_t count, i;

      epi_rd_init (&r, body, blen);
      (void) epi_rd_u64 (&r);            /* instance id */
      count = epi_rd_u32 (&r);
      rc = 1;
      for (i = 0; i < count && !r.err; i++)
        {
          uint64_t id = epi_rd_u64 (&r);
          size_t bloblen = 0;
          uint8_t *blob;
          uint32_t members, m;

          (void) epi_rd_u64 (&r);        /* created_at */
          blob = epi_rd_blob (&r, &bloblen);
          members = epi_rd_u32 (&r);
          for (m = 0; m < members && !r.err; m++)
            (void) epi_rd_u64 (&r);

          if (id == gid && !r.err)
            {
              rc = (bloblen == 0 || write_full (STDOUT_FILENO, blob, bloblen) == 0)
                     ? 0 : 1;
              free (blob);
              break;
            }
          free (blob);
        }
      if (rc != 0)
        fprintf (stderr, "epimone-ctl: no such group %llu\n",
                 (unsigned long long) gid);
    }
  free (body);
  if (fd >= 0)
    close (fd);
  return rc;
}

static int
cmd_group_add (uint64_t gid, uint64_t sid)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;

  if (fd < 0)
    return 1;
  epi_msg_start (&b, EPI_MSG_GROUP_ADD);
  epi_buf_put_u64 (&b, gid);
  epi_buf_put_u64 (&b, sid);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }
  type = read_reply (fd, &body, &blen);
  free (body);
  close (fd);
  return (type == EPI_MSG_OK) ? 0 : 1;
}

/* The daemon reports its instance id as an optional trailing field on
 * LIST_REPLY. Its absence means an older daemon that does not report one. */
static int
cmd_instance_id (void)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type, count, i;
  epi_rd r;

  if (fd < 0)
    return 1;
  epi_msg_start (&b, EPI_MSG_LIST);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }
  type = read_reply (fd, &body, &blen);
  if (type != EPI_MSG_LIST_REPLY)
    {
      free (body);
      close (fd);
      return 1;
    }

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
      free (cwd);
    }

  if (r.err || r.pos >= r.len)
    {
      fprintf (stderr, "epimone-ctl: daemon reports no instance id "
                       "(predates group support)\n");
      free (body);
      close (fd);
      return 1;
    }
  printf ("%llu\n", (unsigned long long) epi_rd_u64 (&r));
  free (body);
  close (fd);
  return 0;
}

/* ---- attach ------------------------------------------------------- */

static struct termios saved_termios;
static int            termios_saved = 0;
static int            winch_pipe[2] = { -1, -1 };

static void
restore_termios (void)
{
  if (termios_saved)
    tcsetattr (STDIN_FILENO, TCSANOW, &saved_termios);
}

static void
winch_handler (int signo)
{
  (void) signo;
  if (winch_pipe[1] >= 0)
    {
      char b = 'w';
      ssize_t n = write (winch_pipe[1], &b, 1);
      (void) n;
    }
}

static void
send_current_size (uint64_t id)
{
  struct winsize ws;
  if (ioctl (STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col)
    send_resize (id, ws.ws_row, ws.ws_col);
}

static int
cmd_attach (uint64_t id)
{
  int fd = connect_daemon ();
  epi_buf b;
  uint8_t *body = NULL;
  size_t blen = 0;
  uint32_t type;
  struct termios raw;
  int detached = 0;

  if (fd < 0)
    return 1;

  /* Push the current window size before attaching. */
  send_current_size (id);

  epi_msg_start (&b, EPI_MSG_ATTACH);
  epi_buf_put_u64 (&b, id);
  epi_msg_end (&b);
  if (send_frame (fd, &b) != 0)
    {
      close (fd);
      return 1;
    }

  /* Read the single framed reply; everything after is raw. */
  if (recv_frame (fd, &type, &body, &blen) != 0)
    {
      fprintf (stderr, "epimone-ctl: attach failed (no reply)\n");
      close (fd);
      return 1;
    }
  if (type != EPI_MSG_ATTACHED)
    {
      if (type == EPI_MSG_ERROR)
        {
          epi_rd r;
          char *msg;
          epi_rd_init (&r, body, blen);
          (void) epi_rd_u32 (&r);
          msg = epi_rd_str (&r);
          fprintf (stderr, "epimone-ctl: attach failed: %s\n",
                   msg ? msg : "?");
          free (msg);
        }
      free (body);
      close (fd);
      return 1;
    }
  free (body);
  body = NULL;

  /* Set up raw terminal mode if stdin is a tty. */
  if (isatty (STDIN_FILENO) && tcgetattr (STDIN_FILENO, &saved_termios) == 0)
    {
      raw = saved_termios;
      cfmakeraw (&raw);
      tcsetattr (STDIN_FILENO, TCSANOW, &raw);
      termios_saved = 1;
    }

  if (pipe (winch_pipe) == 0)
    signal (SIGWINCH, winch_handler);

  fprintf (stderr, "[attached to session %llu; press Ctrl+] to detach]\r\n",
           (unsigned long long) id);

  for (;;)
    {
      struct pollfd pfds[3];
      int np = 0;
      int istdin, isock, iwinch;

      istdin = np;
      pfds[np].fd = STDIN_FILENO;
      pfds[np].events = POLLIN;
      np++;

      isock = np;
      pfds[np].fd = fd;
      pfds[np].events = POLLIN;
      np++;

      iwinch = -1;
      if (winch_pipe[0] >= 0)
        {
          iwinch = np;
          pfds[np].fd = winch_pipe[0];
          pfds[np].events = POLLIN;
          np++;
        }

      if (poll (pfds, (nfds_t) np, -1) < 0)
        {
          if (errno == EINTR)
            continue;
          break;
        }

      if (iwinch >= 0 && (pfds[iwinch].revents & POLLIN))
        {
          char drain[64];
          ssize_t n = read (winch_pipe[0], drain, sizeof drain);
          (void) n;
          send_current_size (id);
        }

      if (pfds[istdin].revents & POLLIN)
        {
          uint8_t buf[4096];
          ssize_t n = read (STDIN_FILENO, buf, sizeof buf);
          if (n <= 0)
            break;
          {
            ssize_t i;
            for (i = 0; i < n; i++)
              if (buf[i] == CTRL_RBRACKET)
                {
                  if (i > 0)
                    write_full (fd, buf, (size_t) i);
                  detached = 1;
                  break;
                }
            if (detached)
              break;
            if (write_full (fd, buf, (size_t) n) != 0)
              break;
          }
        }

      if (pfds[isock].revents & (POLLIN | POLLHUP))
        {
          uint8_t buf[65536];
          ssize_t n = read (fd, buf, sizeof buf);
          if (n <= 0)
            {
              fprintf (stderr, "\r\n[session closed by daemon]\r\n");
              break;
            }
          if (write_full (STDOUT_FILENO, buf, (size_t) n) != 0)
            break;
        }
    }

  restore_termios ();
  close (fd);
  if (winch_pipe[0] >= 0)
    close (winch_pipe[0]);
  if (winch_pipe[1] >= 0)
    close (winch_pipe[1]);

  if (detached)
    fprintf (stderr, "[detached]\n");
  return 0;
}

/* ---- main --------------------------------------------------------- */

static void
usage (void)
{
  fprintf (stderr,
           "usage:\n"
           "  epimone-ctl create [cwd]\n"
           "  epimone-ctl list\n"
           "  epimone-ctl attach <id>\n"
           "  epimone-ctl kill <id>\n"
           "  epimone-ctl resize <id> <rows> <cols>\n"
           "  epimone-ctl peek <id> [bytes]\n"
           "  epimone-ctl group-new [blob|-]\n"
           "  epimone-ctl group-set <gid> <blob|->\n"
           "  epimone-ctl group-list\n"
           "  epimone-ctl group-blob <gid>\n"
           "  epimone-ctl group-add <gid> <sid>\n"
           "  epimone-ctl group-remove <sid>\n"
           "  epimone-ctl instance-id\n"
           "\n"
           "A blob of \"-\" is read from stdin, so binary blobs can be piped in.\n"
           "group-list escapes non-printables; group-blob writes exact bytes.\n");
}

int
main (int argc, char *argv[])
{
  if (argc < 2)
    {
      usage ();
      return 2;
    }

  if (strcmp (argv[1], "create") == 0)
    return cmd_create (argc - 2, argv + 2);
  if (strcmp (argv[1], "list") == 0)
    return cmd_list ();
  if (strcmp (argv[1], "attach") == 0)
    {
      if (argc < 3)
        {
          usage ();
          return 2;
        }
      return cmd_attach (strtoull (argv[2], NULL, 10));
    }
  if (strcmp (argv[1], "kill") == 0)
    {
      if (argc < 3)
        {
          usage ();
          return 2;
        }
      return simple_id_cmd (EPI_MSG_KILL, strtoull (argv[2], NULL, 10));
    }
  if (strcmp (argv[1], "resize") == 0)
    {
      if (argc < 5)
        {
          usage ();
          return 2;
        }
      return send_resize (strtoull (argv[2], NULL, 10),
                          (uint16_t) atoi (argv[3]),
                          (uint16_t) atoi (argv[4]));
    }
  if (strcmp (argv[1], "peek") == 0)
    {
      unsigned long long bytes = EPI_PEEK_DEFAULT_BYTES;

      if (argc < 3)
        {
          usage ();
          return 2;
        }
      if (argc > 3)
        {
          bytes = strtoull (argv[3], NULL, 10);
          if (bytes > 0xffffffffull)
            bytes = 0xffffffffull;   /* max_bytes is a u32 on the wire */
        }
      return cmd_peek (strtoull (argv[2], NULL, 10), (uint32_t) bytes);
    }
  if (strcmp (argv[1], "group-new") == 0)
    return cmd_group_new (argc > 2 ? argv[2] : NULL);
  if (strcmp (argv[1], "group-set") == 0)
    {
      if (argc < 4)
        {
          usage ();
          return 2;
        }
      return cmd_group_set (strtoull (argv[2], NULL, 10), argv[3]);
    }
  if (strcmp (argv[1], "group-list") == 0)
    return cmd_group_list ();
  if (strcmp (argv[1], "group-blob") == 0)
    {
      if (argc < 3)
        {
          usage ();
          return 2;
        }
      return cmd_group_blob (strtoull (argv[2], NULL, 10));
    }
  if (strcmp (argv[1], "group-add") == 0)
    {
      if (argc < 4)
        {
          usage ();
          return 2;
        }
      return cmd_group_add (strtoull (argv[2], NULL, 10),
                            strtoull (argv[3], NULL, 10));
    }
  if (strcmp (argv[1], "group-remove") == 0)
    {
      if (argc < 3)
        {
          usage ();
          return 2;
        }
      return simple_id_cmd (EPI_MSG_GROUP_REMOVE,
                            strtoull (argv[2], NULL, 10));
    }
  if (strcmp (argv[1], "instance-id") == 0)
    return cmd_instance_id ();

  usage ();
  return 2;
}
