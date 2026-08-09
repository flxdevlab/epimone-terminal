#ifndef EPIMONE_GROUP_H
#define EPIMONE_GROUP_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* EPI_GROUP_BLOB_MAX / EPI_GROUP_MAX live with the wire format, since clients
 * need them too. */
#include "../epimone-protocol.h"

/* A group is a named bag of sessions plus one OPAQUE BLOB.
 *
 * The blob is where the GUI stores a tab's arrangement (its split tree, pane
 * ratios and which session sits in which leaf) so that the arrangement has the
 * same lifetime as the sessions it describes. Closing a tab only detaches its
 * sessions, so the arrangement cannot live in the GUI's own widget state; it has
 * to survive here.
 *
 * The daemon NEVER PARSES OR INTERPRETS THE BLOB. It is stored, returned, and
 * replaced verbatim: arbitrary bytes, embedded NULs and invalid UTF-8 included.
 * That is deliberate: it keeps the daemon a dependency-free process supervisor
 * with no notion of UI, and it means the GUI can change its layout encoding
 * without touching the daemon at all.
 *
 * Membership is NOT stored here. It lives on the sessions themselves
 * (epi_session.group_id), so there is one source of truth and no member list to
 * fall out of sync with reality. Enumerating a group's members means walking the
 * session list, which is short. */

typedef struct epi_group {
  uint64_t   id;
  uint8_t   *blob;        /* opaque; NULL when blob_len == 0 */
  size_t     blob_len;
  time_t     created_at;

  struct epi_group *next;
} epi_group;

/* Allocate a group holding a copy of blob (which may be NULL/0). Caller assigns
 * ->id. Returns NULL on OOM. */
epi_group *epi_group_new (const uint8_t *blob, size_t blob_len);

void epi_group_free (epi_group *g);

/* Replace the stored blob with a copy of the given bytes. Returns 0 on success,
 * -1 on OOM, leaving the previous blob intact. */
int epi_group_set_blob (epi_group *g, const uint8_t *blob, size_t blob_len);

#endif /* EPIMONE_GROUP_H */
