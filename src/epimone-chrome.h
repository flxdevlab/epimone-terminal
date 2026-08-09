#pragma once
#include <adwaita.h>

G_BEGIN_DECLS

/* Whole-app chrome theming: retint the entire GUI (window body, header bar,
 * tab bar, sidebar, cards, popovers) to match a terminal palette, so the
 * whole app reads as one coherent theme alongside the terminal.
 *
 * @bg / @fg are the palette background / foreground. The chrome background
 * becomes the palette background (with a slightly shaded header/sidebar for
 * separation); all chrome text becomes the palette foreground. Light vs dark is
 * chosen automatically from the background luminance so light palettes give a
 * light window with dark text. The system accent is left untouched.
 *
 * Applied globally via libadwaita named-color overrides, so it is border-safe
 * (no hard window frame) and updates live + at startup. */
void epimone_chrome_apply_palette (const GdkRGBA *bg, const GdkRGBA *fg);

G_END_DECLS
