#include "ring.h"

#include <stdlib.h>
#include <string.h>

int
epi_ring_init (epi_ring *r, size_t cap)
{
  size_t start = (cap < EPI_RING_INIT_CAP) ? cap : EPI_RING_INIT_CAP;

  r->buf = NULL;
  r->cap = 0;
  r->max_cap = cap;
  r->head = 0;
  r->size = 0;

  if (cap == 0)
    return 0;

  r->buf = (uint8_t *) malloc (start);
  if (r->buf == NULL)
    {
      r->max_cap = 0;
      return -1;
    }
  r->cap = start;
  return 0;
}

void
epi_ring_free (epi_ring *r)
{
  free (r->buf);
  r->buf = NULL;
  r->cap = 0;
  r->max_cap = 0;
  r->head = 0;
  r->size = 0;
}

/* Enlarge the buffer so that `need` bytes fit, doubling until they do but never
 * going past max_cap. Returns 0 if the buffer can now hold `need` bytes, -1 if
 * it cannot (already at the ceiling, or the allocation failed), in which case
 * the caller falls back to overwriting the oldest bytes. The ring is left valid
 * and unchanged on failure. */
static int
ring_grow (epi_ring *r, size_t need)
{
  size_t nc;
  uint8_t *nb;

  if (need <= r->cap)
    return 0;
  if (r->cap >= r->max_cap)
    return -1;

  nc = r->cap ? r->cap : (size_t) EPI_RING_INIT_CAP;
  while (nc < need && nc < r->max_cap)
    nc *= 2;
  if (nc > r->max_cap)
    nc = r->max_cap;

  if (r->head == 0)
    {
      /* Not wrapped. Below the ceiling this is always the case, since the ring
       * grows instead of overwriting, so head never advances. The live bytes
       * already start at offset 0 and every index stays valid across the
       * resize. */
      nb = (uint8_t *) realloc (r->buf, nc);
      if (nb == NULL)
        return -1;
    }
  else
    {
      /* Wrapped. Positions are taken modulo cap, so changing cap changes what
       * every index means; the contents have to be re-laid out linearly. */
      size_t first = r->cap - r->head;

      nb = (uint8_t *) malloc (nc);
      if (nb == NULL)
        return -1;
      if (first >= r->size)
        {
          memcpy (nb, r->buf + r->head, r->size);
        }
      else
        {
          memcpy (nb, r->buf + r->head, first);
          memcpy (nb + first, r->buf, r->size - first);
        }
      free (r->buf);
      r->head = 0;
    }

  r->buf = nb;
  r->cap = nc;
  return (need <= nc) ? 0 : -1;
}

void
epi_ring_write (epi_ring *r, const uint8_t *data, size_t len)
{
  size_t tail;
  size_t first;

  if (r->cap == 0 || len == 0)
    return;

  /* Make room rather than overwrite, while there is still headroom below the
   * ceiling. Failure is not an error: it just means the ring is full, and the
   * code below then wraps exactly as a fixed-capacity ring would. */
  ring_grow (r, r->size + len);

  /* If the incoming data is larger than the ring, keep only its tail. */
  if (len >= r->cap)
    {
      memcpy (r->buf, data + (len - r->cap), r->cap);
      r->head = 0;
      r->size = r->cap;
      return;
    }

  /* Write starting at the current tail position, wrapping as needed. */
  tail = (r->head + r->size) % r->cap;
  first = r->cap - tail;
  if (first > len)
    first = len;
  memcpy (r->buf + tail, data, first);
  if (first < len)
    memcpy (r->buf, data + first, len - first);

  if (r->size + len <= r->cap)
    {
      r->size += len;
    }
  else
    {
      /* Overwrote some old bytes; advance head past the discarded region. */
      size_t overflow = r->size + len - r->cap;
      r->head = (r->head + overflow) % r->cap;
      r->size = r->cap;
    }
}

uint8_t *
epi_ring_snapshot (const epi_ring *r, size_t *out_len)
{
  uint8_t *out;
  size_t first;

  *out_len = 0;
  if (r->size == 0)
    return NULL;

  out = (uint8_t *) malloc (r->size);
  if (out == NULL)
    return NULL;

  first = r->cap - r->head;
  if (first >= r->size)
    {
      memcpy (out, r->buf + r->head, r->size);
    }
  else
    {
      memcpy (out, r->buf + r->head, first);
      memcpy (out + first, r->buf, r->size - first);
    }
  *out_len = r->size;
  return out;
}

uint8_t *
epi_ring_snapshot_tail (const epi_ring *r, size_t max_bytes, size_t *out_len)
{
  uint8_t *out;
  size_t n;
  size_t start;
  size_t first;

  *out_len = 0;
  if (r->cap == 0 || r->size == 0 || max_bytes == 0)
    return NULL;

  n = (r->size < max_bytes) ? r->size : max_bytes;

  /* Skip the leading size - n bytes: start at the first byte to keep. */
  start = (r->head + (r->size - n)) % r->cap;

  out = (uint8_t *) malloc (n);
  if (out == NULL)
    return NULL;

  first = r->cap - start;
  if (first >= n)
    {
      memcpy (out, r->buf + start, n);
    }
  else
    {
      memcpy (out, r->buf + start, first);
      memcpy (out + first, r->buf, n - first);
    }
  *out_len = n;
  return out;
}
