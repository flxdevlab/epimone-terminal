/*
 * Epimone daemon wire protocol.
 *
 * Transport: an AF_UNIX SOCK_STREAM socket at
 *   $XDG_RUNTIME_DIR/epimone/control.sock   (fallback /tmp/epimone-$UID/control.sock)
 *
 * Framing: every control message is length-prefixed, little-endian:
 *
 *     [u32 frame_len][u32 type][ ...body (frame_len - 4 bytes)... ]
 *
 * frame_len counts the bytes that follow it (the type word plus the body).
 * All integers on the wire are little-endian. Strings are encoded as
 * [u32 len][len bytes] with no trailing NUL. String arrays are
 * [u32 count] followed by count strings.
 *
 * Two kinds of connections:
 *
 *   Control connection: the client connects, sends one or more request
 *   frames, and reads reply frames. Used for CREATE, LIST, KILL, RESIZE,
 *   DETACH, PEEK and the GROUP_* messages. Short-lived; the client closes it
 *   when done.
 *
 *   Data connection: the client connects and sends a single ATTACH request.
 *   On success the daemon replies with one EPI_MSG_ATTACHED frame, and from
 *   that point the connection is a RAW bidirectional byte pipe:
 *       client -> daemon bytes are written to the PTY master (keyboard input)
 *       daemon -> client bytes are the PTY output (ring replay first, then live)
 *   No framing is used after EPI_MSG_ATTACHED. The client detaches by simply
 *   closing the connection (a disconnect is treated as a clean DETACH).
 *
 * Body layouts by type:
 *   CREATE   : str cwd; u32 argc, argc*str argv; u32 envc, envc*str env;
 *              [str exec_path]
 *              (argc==0 => spawn $SHELL; env is a list of KEY=VALUE strings
 *               MERGED onto the inherited daemon environ, envc==0 => inherit
 *               unchanged)
 *
 *              exec_path is OPTIONAL: a frame that ends after the env array is
 *              still valid and means "empty". Empty means the file to execute
 *              is argv[0], which is the ordinary case. A non-empty exec_path
 *              is the file actually executed while argv[0] is passed to the
 *              child unchanged, which is the only way to hand a process an
 *              argv[0] that is not its own path. Epimone uses this for login
 *              shells, which are signalled by a leading dash on argv[0]
 *              ("-bash") rather than by a --login/-l flag.
 *   LIST     : (empty)
 *   ATTACH   : u64 id
 *   KILL     : u64 id
 *   RESIZE   : u64 id; u16 rows; u16 cols
 *   DETACH   : u64 id
 *   PEEK     : u64 id; [u32 max_bytes]
 *
 *              A read-only tail read of a session's scrollback ring, for
 *              previews. PEEK has NO side effects: it does not attach, does not
 *              detach whoever is attached, and does not touch the live data
 *              path. ATTACH cannot serve this purpose because it forcibly
 *              detaches any existing client and replays the whole ring, so
 *              previewing a session someone has open would steal it.
 *
 *              max_bytes is OPTIONAL, following the same rule as CREATE's
 *              exec_path: a frame that ends after the id is well-formed and
 *              means EPI_PEEK_DEFAULT_BYTES. Any further fields added later go
 *              after max_bytes on the same terms.
 *
 *   Groups. A group is a set of sessions plus one OPAQUE BLOB, which is where
 *   the GUI keeps a tab's split arrangement so that the arrangement outlives the
 *   widgets (closing a tab only detaches). The daemon stores the blob and hands
 *   it back verbatim; it NEVER parses or interprets it, so the bytes may be
 *   anything (embedded NULs, invalid UTF-8, whatever the GUI's encoding
 *   produces). A blob may be at most EPI_GROUP_BLOB_MAX bytes and at most
 *   EPI_GROUP_MAX groups may exist at once.
 *
 *   Membership is stored on the sessions, not in the blob and not in a member
 *   list, so it cannot drift out of sync. A session belongs to at most one group.
 *
 *   GROUP_NEW    : [blob]                      -> GROUP_ID
 *   GROUP_SET    : u64 group_id; blob          -> OK
 *   GROUP_ADD    : u64 group_id; u64 session_id -> OK
 *   GROUP_REMOVE : u64 session_id              -> OK
 *   GROUP_LIST   : (empty)                     -> GROUP_LIST_REPLY
 *
 *              GROUP_NEW's blob is optional (a frame carrying nothing means an
 *              empty blob), on the same terms as CREATE's exec_path.
 *
 *              GROUP_ADD moves a session between groups in one step: it clears
 *              whatever group the session was in before. Use it rather than
 *              GROUP_REMOVE-then-GROUP_ADD, because a group that loses its last
 *              member is destroyed (see below) and the intermediate state of a
 *              two-step move can therefore destroy the group being moved out of.
 *
 *              LIFETIME: a group is destroyed the moment it loses its last
 *              member session, whether the session was killed, exited and was
 *              reaped away, reassigned by GROUP_ADD, or unassigned by
 *              GROUP_REMOVE. A group naming nothing is garbage, and the point of
 *              storing arrangement here is that arrangement and sessions die
 *              together. A group that has not yet been given ANY member (the
 *              window between GROUP_NEW and the first GROUP_ADD) is kept, since
 *              that is the normal construction sequence.
 *
 *              A session that has merely exited is still a member: it stays
 *              listed as dead with its ring intact, and its group stays too, so
 *              the arrangement can still be shown with the dead pane in place.
 *              Only removal from the daemon counts as losing a member.
 *
 *              When one member of several dies, the blob still names it. The
 *              daemon does NOT prune the blob (that would mean parsing it). The
 *              GUI reconciles instead: GROUP_LIST reports the ids that actually
 *              exist, so the GUI can treat any id in the blob that is not in
 *              that list as a pruned leaf, exactly as it already handles session
 *              id 0 in layout.json.
 *
 *   ID         : u64 id                       (reply to CREATE)
 *   LIST_REPLY : u32 count; count * session_info; [u64 instance_id]
 *
 *              instance_id is an OPTIONAL TRAILING field, appended after the
 *              session array. Clients that stop reading after count elements
 *              are unaffected, and a client that
 *              finds no trailing bytes knows it is talking to a daemon old
 *              enough not to report one.
 *
 *              NOTE: session_info itself is deliberately NOT extended with a
 *              group_id. It is a repeated struct with no per-element length
 *              prefix, so appending a field to it shifts every element after the
 *              first and silently breaks every existing parser. The
 *              optional-trailing-field convention cannot reach inside a repeated
 *              structure. Membership is therefore reported by GROUP_LIST, which
 *              answers "what groups exist and what is in them" in a single round
 *              trip anyway.
 *
 *   ATTACHED   : session_info                 (reply to ATTACH; raw stream follows)
 *   OK         : (empty)                       (generic success)
 *   ERROR      : u32 code; str message
 *   GROUP_ID   : u64 group_id                 (reply to GROUP_NEW)
 *   GROUP_LIST_REPLY : u64 instance_id; u32 count; count * group_info
 *
 * group_info: u64 group_id; u64 created_at; blob;
 *             u32 member_count; member_count * u64 session_id
 *
 *              Members are the sessions currently assigned to the group, in no
 *              guaranteed order, including ones that have exited but are still
 *              listed. Every field is mandatory: like session_info this is a
 *              repeated struct, so nothing here may be optional.
 *   PEEK_REPLY : u64 id; u32 len; len bytes; [u64 total_size]  (reply to PEEK)
 *
 *              len is the number of bytes actually returned: min(max_bytes,
 *              bytes held), so it may be shorter than requested and may be 0.
 *              The bytes are raw PTY output, escape sequences and all, exactly
 *              as they came off the master; nothing is stripped or
 *              interpreted. Encoded like a string (u32 length then that many
 *              bytes) but NOT NUL-terminated and NOT text; it can contain NULs
 *              and invalid UTF-8. The blob is self-delimiting, so later
 *              optional fields can be appended after it.
 *
 *              total_size is an OPTIONAL TRAILING field: the total number of
 *              bytes the ring currently holds, so a caller can tell a complete
 *              session from the tail of a long one. len < total_size means there
 *              is more scrollback above what was returned. Absent from daemons
 *              predating it, which a caller reads as "unknown".
 *
 * session_info: u64 id; u32 pid; u8 alive; u8 attached; u64 created_at; str cwd
 */
#ifndef EPIMONE_PROTOCOL_H
#define EPIMONE_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define EPIMONE_PROTO_VERSION 1
#define EPIMONE_SOCKET_NAME   "control.sock"

/* How much tail a PEEK returns when the request omits max_bytes. Enough to fill
 * a preview several screens deep without dragging the whole ring across the
 * socket. */
#define EPI_PEEK_DEFAULT_BYTES (64 * 1024)

/* Group limits. Protocol constants rather than daemon-private policy: a client
 * needs them to know what it may send, so it can refuse locally with a useful
 * message instead of building a frame the daemon will reject. */
#define EPI_GROUP_BLOB_MAX (64 * 1024)
#define EPI_GROUP_MAX      4096

enum epi_msg_type {
  /* requests: client -> daemon */
  EPI_MSG_CREATE     = 1,
  EPI_MSG_LIST       = 2,
  EPI_MSG_ATTACH     = 3,
  EPI_MSG_KILL       = 4,
  EPI_MSG_RESIZE     = 5,
  EPI_MSG_DETACH     = 6,
  EPI_MSG_PEEK       = 7,
  EPI_MSG_GROUP_NEW    = 8,
  EPI_MSG_GROUP_SET    = 9,
  EPI_MSG_GROUP_ADD    = 10,
  EPI_MSG_GROUP_REMOVE = 11,
  EPI_MSG_GROUP_LIST   = 12,
  /* responses: daemon -> client */
  EPI_MSG_ID         = 100,
  EPI_MSG_LIST_REPLY = 101,
  EPI_MSG_ATTACHED   = 102,
  EPI_MSG_OK         = 103,
  EPI_MSG_ERROR      = 104,
  EPI_MSG_PEEK_REPLY = 105,
  EPI_MSG_GROUP_ID         = 106,
  EPI_MSG_GROUP_LIST_REPLY = 107
};

enum epi_err_code {
  EPI_ERR_NONE       = 0,
  EPI_ERR_PROTOCOL   = 1,
  EPI_ERR_NOT_FOUND  = 2,
  EPI_ERR_SPAWN      = 3,
  EPI_ERR_INTERNAL   = 4,
  EPI_ERR_TOO_LARGE  = 5   /* blob over the cap, or too many groups */
};

/* ------------------------------------------------------------------ *
 * Growable output buffer used to build frames.
 * ------------------------------------------------------------------ */
typedef struct {
  uint8_t *data;
  size_t   len;
  size_t   cap;
  int      oom;   /* set if an allocation failed */
} epi_buf;

static inline void
epi_buf_init (epi_buf *b)
{
  b->data = NULL;
  b->len = 0;
  b->cap = 0;
  b->oom = 0;
}

static inline void
epi_buf_free (epi_buf *b)
{
  free (b->data);
  epi_buf_init (b);
}

static inline int
epi_buf_reserve (epi_buf *b, size_t extra)
{
  size_t need = b->len + extra;
  size_t nc;
  uint8_t *nd;

  if (need <= b->cap)
    return 0;

  nc = b->cap ? b->cap : 64;
  while (nc < need)
    nc *= 2;

  nd = (uint8_t *) realloc (b->data, nc);
  if (nd == NULL)
    {
      b->oom = 1;
      return -1;
    }
  b->data = nd;
  b->cap = nc;
  return 0;
}

static inline void
epi_buf_put_bytes (epi_buf *b, const void *p, size_t n)
{
  if (epi_buf_reserve (b, n) != 0)
    return;
  memcpy (b->data + b->len, p, n);
  b->len += n;
}

static inline void
epi_buf_put_u8 (epi_buf *b, uint8_t v)
{
  epi_buf_put_bytes (b, &v, 1);
}

static inline void
epi_buf_put_u16 (epi_buf *b, uint16_t v)
{
  uint8_t t[2] = { (uint8_t) (v & 0xff), (uint8_t) ((v >> 8) & 0xff) };
  epi_buf_put_bytes (b, t, 2);
}

static inline void
epi_buf_put_u32 (epi_buf *b, uint32_t v)
{
  uint8_t t[4] = { (uint8_t) (v & 0xff), (uint8_t) ((v >> 8) & 0xff),
                   (uint8_t) ((v >> 16) & 0xff), (uint8_t) ((v >> 24) & 0xff) };
  epi_buf_put_bytes (b, t, 4);
}

static inline void
epi_buf_put_u64 (epi_buf *b, uint64_t v)
{
  epi_buf_put_u32 (b, (uint32_t) (v & 0xffffffffu));
  epi_buf_put_u32 (b, (uint32_t) ((v >> 32) & 0xffffffffu));
}

static inline void
epi_buf_put_str (epi_buf *b, const char *s)
{
  size_t n = s ? strlen (s) : 0;
  epi_buf_put_u32 (b, (uint32_t) n);
  if (n)
    epi_buf_put_bytes (b, s, n);
}

/* Length-prefixed raw byte block. Same encoding as a string, but the payload is
 * arbitrary binary: no NUL terminator, no character-set assumption. Used for
 * PTY bytes in PEEK_REPLY. */
static inline void
epi_buf_put_blob (epi_buf *b, const void *p, size_t n)
{
  epi_buf_put_u32 (b, (uint32_t) n);
  if (n)
    epi_buf_put_bytes (b, p, n);
}

/* Begin a frame: reserves the length prefix and writes the type. */
static inline void
epi_msg_start (epi_buf *b, uint32_t type)
{
  epi_buf_init (b);
  epi_buf_put_u32 (b, 0);      /* placeholder for frame_len */
  epi_buf_put_u32 (b, type);
}

/* Finish a frame: patch the length prefix. */
static inline void
epi_msg_end (epi_buf *b)
{
  uint32_t n;

  if (b->oom || b->len < 4)
    return;
  n = (uint32_t) (b->len - 4);
  b->data[0] = (uint8_t) (n & 0xff);
  b->data[1] = (uint8_t) ((n >> 8) & 0xff);
  b->data[2] = (uint8_t) ((n >> 16) & 0xff);
  b->data[3] = (uint8_t) ((n >> 24) & 0xff);
}

/* ------------------------------------------------------------------ *
 * Reader cursor over a received frame body.
 * ------------------------------------------------------------------ */
typedef struct {
  const uint8_t *data;
  size_t         len;
  size_t         pos;
  int            err;   /* set on out-of-bounds read */
} epi_rd;

static inline void
epi_rd_init (epi_rd *r, const void *data, size_t len)
{
  r->data = (const uint8_t *) data;
  r->len = len;
  r->pos = 0;
  r->err = 0;
}

static inline uint8_t
epi_rd_u8 (epi_rd *r)
{
  if (r->pos + 1 > r->len)
    {
      r->err = 1;
      return 0;
    }
  return r->data[r->pos++];
}

static inline uint16_t
epi_rd_u16 (epi_rd *r)
{
  uint16_t v;
  if (r->pos + 2 > r->len)
    {
      r->err = 1;
      return 0;
    }
  v = (uint16_t) (r->data[r->pos] | (r->data[r->pos + 1] << 8));
  r->pos += 2;
  return v;
}

static inline uint32_t
epi_rd_u32 (epi_rd *r)
{
  uint32_t v;
  if (r->pos + 4 > r->len)
    {
      r->err = 1;
      return 0;
    }
  v = (uint32_t) r->data[r->pos]
    | ((uint32_t) r->data[r->pos + 1] << 8)
    | ((uint32_t) r->data[r->pos + 2] << 16)
    | ((uint32_t) r->data[r->pos + 3] << 24);
  r->pos += 4;
  return v;
}

static inline uint64_t
epi_rd_u64 (epi_rd *r)
{
  uint64_t lo = epi_rd_u32 (r);
  uint64_t hi = epi_rd_u32 (r);
  return lo | (hi << 32);
}

/* Reads a length-prefixed string into a freshly malloc'd NUL-terminated
 * buffer. Returns NULL on error or OOM; caller frees. */
static inline char *
epi_rd_str (epi_rd *r)
{
  uint32_t n = epi_rd_u32 (r);
  char *s;

  if (r->err || r->pos + n > r->len)
    {
      r->err = 1;
      return NULL;
    }
  s = (char *) malloc ((size_t) n + 1);
  if (s == NULL)
    {
      r->err = 1;
      return NULL;
    }
  if (n)
    memcpy (s, r->data + r->pos, n);
  s[n] = '\0';
  r->pos += n;
  return s;
}

/* Reads a length-prefixed raw byte block (see epi_buf_put_blob). Sets *out_len.
 * Returns NULL on a malformed frame, on OOM, or when the block is empty. An
 * empty block is not an error, so check r->err to tell them apart. Caller
 * frees. */
static inline uint8_t *
epi_rd_blob (epi_rd *r, size_t *out_len)
{
  uint32_t n = epi_rd_u32 (r);
  uint8_t *p;

  *out_len = 0;
  if (r->err || r->pos + n > r->len)
    {
      r->err = 1;
      return NULL;
    }
  if (n == 0)
    return NULL;
  p = (uint8_t *) malloc (n);
  if (p == NULL)
    {
      r->err = 1;
      return NULL;
    }
  memcpy (p, r->data + r->pos, n);
  r->pos += n;
  *out_len = n;
  return p;
}

#endif /* EPIMONE_PROTOCOL_H */
