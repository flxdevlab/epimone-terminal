#ifndef EPIMONE_RING_H
#define EPIMONE_RING_H

#include <stddef.h>
#include <stdint.h>

/* Circular byte buffer holding the most recent PTY output.
 *
 * The buffer grows on demand: it starts at EPI_RING_INIT_CAP and doubles as
 * output accumulates, up to a ceiling of EPI_RING_CAP. Below the ceiling
 * nothing is ever discarded (the ring grows instead of overwriting), so the
 * contents are always the most recent min(bytes_written, EPI_RING_CAP) bytes,
 * exactly what a ring allocated at EPI_RING_CAP up front would hold. Once the
 * ceiling is reached the buffer stops growing and the oldest bytes are
 * overwritten as before.
 *
 * Growing rather than pre-allocating matters because closing a pane, tab or
 * window only detaches: sessions accumulate and are held indefinitely, and most
 * of them never produce more than a prompt.
 *
 * Sizes never shrink; a session that once produced a megabyte keeps the
 * megabyte for as long as it exists. */

#define EPI_RING_CAP      (1024 * 1024)   /* ceiling: 1 MiB per session */
#define EPI_RING_INIT_CAP 4096            /* one page to start with */

typedef struct {
  uint8_t *buf;
  size_t   cap;      /* bytes currently allocated */
  size_t   max_cap;  /* ceiling cap may grow to */
  size_t   head;     /* index of the oldest valid byte */
  size_t   size;     /* number of valid bytes */
} epi_ring;

/* cap is the ceiling, not the initial allocation. */
int    epi_ring_init (epi_ring *r, size_t cap);
void   epi_ring_free (epi_ring *r);
void   epi_ring_write (epi_ring *r, const uint8_t *data, size_t len);

/* Linearize the ring contents (oldest -> newest) into a freshly malloc'd
 * buffer. Sets *out_len. Returns NULL when empty or on OOM. Caller frees. */
uint8_t *epi_ring_snapshot (const epi_ring *r, size_t *out_len);

/* Like epi_ring_snapshot but copies only the last max_bytes bytes, allocating
 * just that much. Sets *out_len to the number of bytes returned, which is
 * min(max_bytes, bytes held) and so may be shorter than requested. Returns NULL
 * when that works out to zero bytes (empty ring or max_bytes == 0) or on OOM. */
uint8_t *epi_ring_snapshot_tail (const epi_ring *r, size_t max_bytes,
                                 size_t *out_len);

#endif /* EPIMONE_RING_H */
