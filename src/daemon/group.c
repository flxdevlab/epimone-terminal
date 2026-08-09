#include "group.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Copy len bytes into a fresh buffer. Returns 0 and leaves *out NULL for a
 * zero-length blob, which is a legal blob meaning "nothing stored yet". */
static int
blob_dup (const uint8_t *src, size_t len, uint8_t **out, size_t *out_len)
{
  uint8_t *p;

  if (len == 0)
    {
      *out = NULL;
      *out_len = 0;
      return 0;
    }

  p = (uint8_t *) malloc (len);
  if (p == NULL)
    return -1;
  memcpy (p, src, len);
  *out = p;
  *out_len = len;
  return 0;
}

epi_group *
epi_group_new (const uint8_t *blob, size_t blob_len)
{
  epi_group *g = (epi_group *) calloc (1, sizeof *g);

  if (g == NULL)
    return NULL;
  if (blob_dup (blob, blob_len, &g->blob, &g->blob_len) != 0)
    {
      free (g);
      return NULL;
    }
  g->created_at = time (NULL);
  return g;
}

void
epi_group_free (epi_group *g)
{
  if (g == NULL)
    return;
  free (g->blob);
  free (g);
}

int
epi_group_set_blob (epi_group *g, const uint8_t *blob, size_t blob_len)
{
  uint8_t *nb = NULL;
  size_t nl = 0;

  /* Build the replacement first so a failed allocation cannot lose the blob the
   * group is already holding. */
  if (blob_dup (blob, blob_len, &nb, &nl) != 0)
    return -1;

  free (g->blob);
  g->blob = nb;
  g->blob_len = nl;
  return 0;
}
