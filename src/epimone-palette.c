#include "epimone-palette.h"

/* Built-in color schemes. "epimone-black" (shown as "Epimone") is the default:
 * a dark grey bg (#1e1e1e), light grey fg, and a Tango-ish 16-color set tuned
 * so the standard Ubuntu PS1 reads right: a clean green (#33d17a, ANSI 2) and
 * blue (#62a0ea, ANSI 4).
 *
 * The remaining schemes are grouped popular-first, then the classic/system
 * sets, using the standard dark-variant values of each published scheme.
 * Order here is the grid order.
 *
 * Note: the id stays "epimone-black" (only the display name changed) so
 * existing GSettings "theme" values keep resolving. */
static const EpimonePalette epimone_palette_table[] = {
  {
    .id = "epimone-black", .name = "Epimone",
    .background = "#1e1e1e", .foreground = "#c8c8c8", .cursor = "#cccccc",
    .ansi = {
      "#000000", "#cc0000", "#33d17a", "#c4a000",
      "#62a0ea", "#75507b", "#06989a", "#d3d7cf",
      "#555753", "#ef2929", "#8ae234", "#fce94f",
      "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec",
    },
  },
  {
    .id = "dracula", .name = "Dracula",
    .background = "#282a36", .foreground = "#f8f8f2", .cursor = "#f8f8f2",
    .ansi = {
      "#21222c", "#ff5555", "#50fa7b", "#f1fa8c",
      "#bd93f9", "#ff79c6", "#8be9fd", "#f8f8f2",
      "#6272a4", "#ff6e6e", "#69ff94", "#ffffa5",
      "#d6acff", "#ff92df", "#a4ffff", "#ffffff",
    },
  },
  {
    .id = "nord", .name = "Nord",
    .background = "#2e3440", .foreground = "#d8dee9", .cursor = "#d8dee9",
    .ansi = {
      "#3b4252", "#bf616a", "#a3be8c", "#ebcb8b",
      "#81a1c1", "#b48ead", "#88c0d0", "#e5e9f0",
      "#4c566a", "#bf616a", "#a3be8c", "#ebcb8b",
      "#81a1c1", "#b48ead", "#8fbcbb", "#eceff4",
    },
  },
  {
    .id = "gruvbox-dark", .name = "Gruvbox Dark",
    .background = "#282828", .foreground = "#ebdbb2", .cursor = "#ebdbb2",
    .ansi = {
      "#282828", "#cc241d", "#98971a", "#d79921",
      "#458588", "#b16286", "#689d6a", "#a89984",
      "#928374", "#fb4934", "#b8bb26", "#fabd2f",
      "#83a598", "#d3869b", "#8ec07c", "#ebdbb2",
    },
  },
  {
    .id = "solarized-dark", .name = "Solarized Dark",
    .background = "#002b36", .foreground = "#839496", .cursor = "#839496",
    .ansi = {
      "#073642", "#dc322f", "#859900", "#b58900",
      "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#002b36", "#cb4b16", "#586e75", "#657b83",
      "#839496", "#6c71c4", "#93a1a1", "#fdf6e3",
    },
  },
  {
    .id = "solarized-light", .name = "Solarized Light",
    .background = "#fdf6e3", .foreground = "#657b83", .cursor = "#657b83",
    .ansi = {
      "#073642", "#dc322f", "#859900", "#b58900",
      "#268bd2", "#d33682", "#2aa198", "#eee8d5",
      "#002b36", "#cb4b16", "#586e75", "#657b83",
      "#839496", "#6c71c4", "#93a1a1", "#fdf6e3",
    },
  },
  {
    .id = "tango-dark", .name = "Tango Dark",
    .background = "#2e3436", .foreground = "#d3d7cf", .cursor = "#d3d7cf",
    .ansi = {
      "#2e3436", "#cc0000", "#4e9a06", "#c4a000",
      "#3465a4", "#75507b", "#06989a", "#d3d7cf",
      "#555753", "#ef2929", "#8ae234", "#fce94f",
      "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec",
    },
  },
  /* ---- Classic/system sets, dark variants ---- */
  {
    .id = "gnome", .name = "GNOME",
    .background = "#1c1c1f", .foreground = "#ffffff", .cursor = "#ffffff",
    .ansi = {
      "#241f31", "#c01c28", "#2ec27e", "#f5c211",
      "#1e78e4", "#9841bb", "#0ab9dc", "#c0bfbc",
      "#5e5c64", "#ed333b", "#57e389", "#f8e45c",
      "#51a1ff", "#c061cb", "#4fd2fd", "#f6f5f4",
    },
  },
  {
    .id = "ubuntu", .name = "Ubuntu",
    .background = "#300a24", .foreground = "#ffffff", .cursor = "#ffffff",
    .ansi = {
      "#2e3436", "#cc0000", "#4e9a06", "#c4a000",
      "#3465a4", "#75507b", "#06989a", "#d3d7cf",
      "#555753", "#ef2929", "#8ae234", "#fce94f",
      "#729fcf", "#ad7fa8", "#34e2e2", "#eeeeec",
    },
  },
  {
    .id = "campbell", .name = "Campbell",
    .background = "#0c0c0c", .foreground = "#777777", .cursor = "#777777",
    .ansi = {
      "#0c0c0c", "#c50f1f", "#13a10e", "#c19c00",
      "#0037da", "#881798", "#3a96dd", "#cccccc",
      "#767676", "#e74856", "#16c60c", "#f9f1a5",
      "#3b78ff", "#b4009e", "#61d6d6", "#f2f2f2",
    },
  },
  {
    .id = "horizon", .name = "Horizon",
    .background = "#1c1e26", .foreground = "#fdf0ed", .cursor = "#fdf0ed",
    .ansi = {
      "#16161c", "#e95678", "#29d398", "#fab795",
      "#26bbd9", "#ee64ae", "#59e3e3", "#fadad1",
      "#232530", "#ec6a88", "#3fdaa4", "#fbc3a7",
      "#3fc6de", "#f075b7", "#6be6e6", "#fdf0ed",
    },
  },
  {
    .id = "linux", .name = "Linux",
    .background = "#000000", .foreground = "#aaaaaa", .cursor = "#aaaaaa",
    .ansi = {
      "#000000", "#aa0000", "#00aa00", "#aa5500",
      "#0000aa", "#aa00aa", "#00aaaa", "#aaaaaa",
      "#555555", "#ff5555", "#55ff55", "#ffff55",
      "#5555ff", "#ff55ff", "#55ffff", "#ffffff",
    },
  },
  {
    .id = "vscode", .name = "VS Code",
    .background = "#1e1e1e", .foreground = "#cccccc", .cursor = "#cccccc",
    .ansi = {
      "#6a787a", "#e9653b", "#39e9a8", "#e5b684",
      "#44aae6", "#e17599", "#3dd5e7", "#c3dde1",
      "#598489", "#e65029", "#00ff9a", "#e89440",
      "#009afb", "#ff578f", "#5fffff", "#d9fbff",
    },
  },
  {
    .id = "xterm", .name = "XTerm",
    .background = "#000000", .foreground = "#ffffff", .cursor = "#ffffff",
    .ansi = {
      "#000000", "#cd0000", "#00cd00", "#cdcd00",
      "#0000ee", "#cd00cd", "#00cdcd", "#e5e5e5",
      "#7f7f7f", "#ff0000", "#00ff00", "#ffff00",
      "#5c5cff", "#ff00ff", "#00ffff", "#ffffff",
    },
  },
  {
    .id = "high-contrast", .name = "High Contrast",
    .background = "#000000", .foreground = "#cfcfcf", .cursor = "#cfcfcf",
    .ansi = {
      "#1e1e1e", "#c01c28", "#26a269", "#a2734c",
      "#12488b", "#a347ba", "#2aa1b3", "#cfcfcf",
      "#5d5d5d", "#f66151", "#33d17a", "#e9ad0c",
      "#2a7bde", "#c061cb", "#33c7de", "#ffffff",
    },
  },
};

const EpimonePalette *
epimone_palettes (gsize *n_out)
{
  if (n_out != NULL)
    *n_out = G_N_ELEMENTS (epimone_palette_table);
  return epimone_palette_table;
}

const EpimonePalette *
epimone_palette_by_id (const char *id)
{
  if (id == NULL)
    return NULL;
  for (gsize i = 0; i < G_N_ELEMENTS (epimone_palette_table); i++)
    if (g_strcmp0 (epimone_palette_table[i].id, id) == 0)
      return &epimone_palette_table[i];
  return NULL;
}

gboolean
epimone_palette_get_rgba (const EpimonePalette *p,
                          GdkRGBA             *bg,
                          GdkRGBA             *fg,
                          GdkRGBA             *cursor,
                          GdkRGBA              ansi_out[16])
{
  GdkRGBA tmp_bg, tmp_fg, tmp_cursor, tmp_ansi[16];

  if (p == NULL)
    return FALSE;

  if (!gdk_rgba_parse (&tmp_bg, p->background) ||
      !gdk_rgba_parse (&tmp_fg, p->foreground) ||
      !gdk_rgba_parse (&tmp_cursor, p->cursor))
    return FALSE;

  for (gsize i = 0; i < 16; i++)
    if (!gdk_rgba_parse (&tmp_ansi[i], p->ansi[i]))
      return FALSE;

  if (bg != NULL)     *bg = tmp_bg;
  if (fg != NULL)     *fg = tmp_fg;
  if (cursor != NULL) *cursor = tmp_cursor;
  if (ansi_out != NULL)
    for (gsize i = 0; i < 16; i++)
      ansi_out[i] = tmp_ansi[i];

  return TRUE;
}
