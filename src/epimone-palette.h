#pragma once
#include <gdk/gdk.h>

G_BEGIN_DECLS

/* A terminal color scheme, defined as data. Colors are "#rrggbb" strings so
 * this table stays dependency-light; epimone_palette_get_rgba() parses them.
 *
 * TODO: a palette-file loader and an "Import palette…" action appending user
 * palettes to this built-in set. Not built yet. */
typedef struct
{
  const char *id;            /* stable key stored in GSettings "theme" */
  const char *name;          /* human label shown on the palette card */
  const char *background;
  const char *foreground;
  const char *cursor;
  const char *ansi[16];      /* the 16 ANSI colors, indices 0..15 */
} EpimonePalette;

/* The built-in palette table. @n_out receives the count. */
const EpimonePalette *epimone_palettes    (gsize *n_out);

/* Look up a palette by id, or NULL if unknown. */
const EpimonePalette *epimone_palette_by_id (const char *id);

/* Parse a palette's colors into GdkRGBA. @ansi_out must hold 16 entries.
 * Returns FALSE (leaving outputs untouched) if any color fails to parse. */
gboolean epimone_palette_get_rgba (const EpimonePalette *p,
                                   GdkRGBA             *bg,
                                   GdkRGBA             *fg,
                                   GdkRGBA             *cursor,
                                   GdkRGBA              ansi_out[16]);

G_END_DECLS
