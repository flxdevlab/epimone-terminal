#include <math.h>

#include "epimone-chrome.h"

/* Whole-window palette theming.
 *
 * The titlebar derivation and darkness test are adapted from Ptyxis
 * (ptyxis-window-dressing.c, ptyxis-palette.c; GNOME Ptyxis contributors,
 * GPL-3.0-or-later, the same license as this project).
 *
 * The terminal/window body is the palette background, and the header bar /
 * tab bar / sidebar sit at a "titlebar" shade derived from it, so the chrome
 * steps off the content without a drawn edge:
 *
 *   dark = is_dark (background)                  <- HSP, see epimone_is_dark
 *   titlebar_background = dark ? shade (background, 1.25) : background
 *   titlebar_foreground = shade (foreground, dark ? 1.25 : 0.95)
 *
 * Consequences, accepted by design:
 *   - shade() is PROPORTIONAL (it scales HSL lightness), so the header step is
 *     subtle on very dark palettes: #1e1e1e -> #262626 is only +8 per channel.
 *   - a pure black background (high-contrast) has lightness 0, and 0 * 1.25 is
 *     still 0, so header and body are identical.
 *   - LIGHT palettes take the background unchanged, so header and body are
 *     identical there too (Solarized Light: both #fdf6e3).
 * Do not substitute a fixed offset to force a visible step in those cases
 * (tried and reverted): a flat header is the most seamless possible blend
 * into the body.
 *
 * Applied via a display-wide provider so it reaches every window and dialog.
 * background-color / color ONLY (never border or outline), and never on the
 * toplevel `window` node: Yaru draws a 1px outline + shadow ring on `window.csd`
 * that a solid window-node fill turns into a doubled edge when focused. The
 * interior `toolbarview` is painted for the body instead, so GTK/libadwaita
 * keeps drawing its own single clean window frame in both focus states.
 *
 * Light vs dark is pushed to AdwStyleManager for correct derived shades. Body
 * text is the palette foreground; all chrome text is the derived titlebar
 * foreground. Accent is untouched.
 */

/* HSL helpers reproducing GTK's private _gdk_rgba_shade() bit-for-bit
 * (gdkhsla.c; Benjamin Otte, LGPL-2.1-or-later, usable from
 * GPL-3.0-or-later). Transcribed rather than vendored, since it is three
 * short functions; `float` is kept (not double) so the arithmetic matches
 * the original exactly. */
typedef struct
{
  float hue;          /* degrees, 0..360 */
  float saturation;
  float lightness;
  float alpha;
} EpimoneHSLA;

static void
epimone_hsla_from_rgba (EpimoneHSLA *hsla, const GdkRGBA *rgba)
{
  float red = rgba->red, green = rgba->green, blue = rgba->blue;
  float min, max, delta;

  max = MAX (red, MAX (green, blue));
  min = MIN (red, MIN (green, blue));

  hsla->lightness = (max + min) / 2;
  hsla->saturation = 0;
  hsla->hue = 0;
  hsla->alpha = rgba->alpha;

  if (max != min)
    {
      if (hsla->lightness <= 0.5)
        hsla->saturation = (max - min) / (max + min);
      else
        hsla->saturation = (max - min) / (2 - max - min);

      delta = max - min;
      if (red == max)
        hsla->hue = (green - blue) / delta;
      else if (green == max)
        hsla->hue = 2 + (blue - red) / delta;
      else if (blue == max)
        hsla->hue = 4 + (red - green) / delta;

      hsla->hue *= 60;
      if (hsla->hue < 0.0)
        hsla->hue += 360;
    }
}

/* One channel of the HSL->RGB reconstruction, at hue offset @hue. */
static float
epimone_hsla_channel (float hue, float m1, float m2)
{
  while (hue > 360) hue -= 360;
  while (hue < 0)   hue += 360;

  if (hue < 60)  return m1 + (m2 - m1) * hue / 60;
  if (hue < 180) return m2;
  if (hue < 240) return m1 + (m2 - m1) * (240 - hue) / 60;
  return m1;
}

static void
epimone_rgba_from_hsla (GdkRGBA *rgba, const EpimoneHSLA *hsla)
{
  float lightness = hsla->lightness;
  float saturation = hsla->saturation;
  float m1, m2;

  m2 = (lightness <= 0.5) ? lightness * (1 + saturation)
                          : lightness + saturation - lightness * saturation;
  m1 = 2 * lightness - m2;

  rgba->alpha = hsla->alpha;

  if (saturation == 0)
    {
      rgba->red = rgba->green = rgba->blue = lightness;
    }
  else
    {
      rgba->red   = epimone_hsla_channel (hsla->hue + 120, m1, m2);
      rgba->green = epimone_hsla_channel (hsla->hue,       m1, m2);
      rgba->blue  = epimone_hsla_channel (hsla->hue - 120, m1, m2);
    }
}

/* GTK's _gdk_rgba_shade(): scale HSL lightness AND saturation by @factor. */
static GdkRGBA
epimone_shade (const GdkRGBA *color, float factor)
{
  EpimoneHSLA hsla;
  GdkRGBA out;

  epimone_hsla_from_rgba (&hsla, color);

  hsla.lightness = CLAMP (hsla.lightness * factor, 0.0, 1.0);
  hsla.saturation = CLAMP (hsla.saturation * factor, 0.0, 1.0);

  epimone_rgba_from_hsla (&out, &hsla);
  out.alpha = 1.0;
  return out;
}

/* HSP perceived brightness, http://alienryderflex.com/hsp.html. Not the same
 * as relative luminance; this is the test that selects the dark branch of
 * both titlebar formulas above, so the same test must be used everywhere a
 * palette is classified. */
static gboolean
epimone_is_dark (const GdkRGBA *color)
{
  double r = color->red * 255.0;
  double g = color->green * 255.0;
  double b = color->blue * 255.0;
  double hsp = sqrt (0.299 * (r * r) +
                     0.587 * (g * g) +
                     0.114 * (b * b));

  return hsp <= 127.5;
}

static char *
epimone_hex (const GdkRGBA *c)
{
  return g_strdup_printf ("#%02x%02x%02x",
                          (int) CLAMP (c->red   * 255.0 + 0.5, 0.0, 255.0),
                          (int) CLAMP (c->green * 255.0 + 0.5, 0.0, 255.0),
                          (int) CLAMP (c->blue  * 255.0 + 0.5, 0.0, 255.0));
}

void
epimone_chrome_apply_palette (const GdkRGBA *bg, const GdkRGBA *fg)
{
  static GtkCssProvider *provider = NULL;
  gboolean dark;
  GdkRGBA titlebar_bg;
  GdkRGBA titlebar_fg;
  g_autofree char *bg_hex = NULL;
  g_autofree char *fg_hex = NULL;
  g_autofree char *tbg_hex = NULL;
  g_autofree char *tfg_hex = NULL;
  g_autofree char *css = NULL;
  const char *separator_css;
  const char *destructive_hex;

  if (bg == NULL || fg == NULL)
    return;

  /* Titlebar derivation; see the file header. */
  dark = epimone_is_dark (bg);
  titlebar_bg = dark ? epimone_shade (bg, 1.25f) : *bg;
  titlebar_fg = epimone_shade (fg, dark ? 1.25f : 0.95f);

  adw_style_manager_set_color_scheme (
    adw_style_manager_get_default (),
    dark ? ADW_COLOR_SCHEME_FORCE_DARK : ADW_COLOR_SCHEME_FORCE_LIGHT);

  bg_hex = epimone_hex (bg);
  fg_hex = epimone_hex (fg);
  tbg_hex = epimone_hex (&titlebar_bg);
  tfg_hex = epimone_hex (&titlebar_fg);

  /* Pane divider line, adapted to the palette by the SAME luminance test that
   * picks everything else here (epimone_is_dark, above). main.c's static
   * rgba(255,255,255,0.35) line composites cleanly on dark palettes but is an
   * invisible pale streak on Solarized Light (measured (254,250,241) on
   * (253,246,227) at the old 0.5 alpha: 1.04:1), so light palettes flip it to
   * the mirror image, black at the same 0.35 alpha.
   *
   * Light palettes also kill libadwaita's own separator shadow:
   *   paned.horizontal > separator:dir(ltr) {
   *     box-shadow: inset 1px 0 color-mix(in srgb, currentColor 15%, ...); }
   * (and inset 0 1px on paned.vertical): a 1px currentColor smudge on one
   * edge of the handle, confirmed by pixel match (15% fg over bg, exact) and
   * by the smudge vanishing under an injected box-shadow:none. On light
   * palettes it darkens toward a LIGHT line and reads as a lopsided gradient.
   *
   * DARK PALETTES EMIT NOTHING, deliberately, so their rendering stays
   * byte-identical to main.c's static rule, including that same shadow,
   * which on dark sits between the background and the light line and reads
   * as part of the line. Verified byte-identical by snapshot comparison. */
  separator_css = dark
    ? ""
    : "paned > separator {"
      "  background-color: rgba(0,0,0,0.35);"
      "  box-shadow: none;"
      "}";

  /* Destructive menu items (.epi-destructive, the overview's Kill all
   * sessions): libadwaita's own error tones, picked by the SAME luminance
   * test as everything else here; a single hardcoded red would fail on half
   * the palettes. Measured against the popover background these rules sit on
   * (titlebar_bg): #ff938c on Dracula's derived #303346 is 5.82:1, #c30000
   * on Solarized Light's #fdf6e3 is 5.86:1; both clear WCAG AA. */
  destructive_hex = dark ? "#ff938c" : "#c30000";

  if (provider == NULL)
    {
      provider = gtk_css_provider_new ();
      gtk_style_context_add_provider_for_display (
        gdk_display_get_default (), GTK_STYLE_PROVIDER (provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    }

  /* %1$s body bg, %2$s body fg, %3$s titlebar bg, %4$s titlebar fg.
   * Body text is the palette foreground; every chrome surface (top-bar
   * revealer, windowhandle, popovers) uses the derived titlebar foreground.
   * No border/outline; no background on the toplevel `window` node. */
  css = g_strdup_printf (
    /* Named colors, for the GTK core rules that still reference @names. */
    "@define-color window_bg_color %1$s;"
    "@define-color window_fg_color %2$s;"
    "@define-color view_bg_color %1$s;"
    "@define-color view_fg_color %2$s;"
    "@define-color headerbar_bg_color %3$s;"
    "@define-color headerbar_fg_color %4$s;"
    "@define-color sidebar_bg_color %3$s;"
    "@define-color sidebar_fg_color %4$s;"
    "@define-color card_bg_color %3$s;"
    "@define-color card_fg_color %4$s;"
    "@define-color popover_bg_color %3$s;"
    "@define-color popover_fg_color %4$s;"
    /* The same palette again as libadwaita's CSS custom properties. THIS is
     * the block that actually retints stock widgets.
     *
     * libadwaita styles its surfaces off var(--window-bg-color) and friends,
     * and those vars are NOT the @define-colors above: in GTK4 named colors
     * resolve per provider, so libadwaita's
     * `:root { --window-bg-color: @window_bg_color }` reads libadwaita's OWN
     * definition and never sees these overrides. Custom properties, unlike
     * named colors, cascade per element, so redefining them here on :root,
     * from a PRIORITY_APPLICATION provider, does reach every widget.
     *
     * Without this, any surface not named explicitly keeps libadwaita's stock
     * grey (e.g. the GtkFontDialog's `window.background` node at #2c2c2c with
     * #414140 entries) inside an otherwise palette-tinted app. One block
     * covers every such surface at once instead of chasing individual nodes.
     *
     * Deliberately NOT set: --headerbar-shade-color (main.c leans on
     * libadwaita's own value for the header lift) and every *-border-color, so
     * this stays background-and-text only. */
    ":root {"
    "  --window-bg-color: %1$s; --window-fg-color: %2$s;"
    "  --view-bg-color: %1$s; --view-fg-color: %2$s;"
    "  --thumbnail-bg-color: %1$s; --thumbnail-fg-color: %2$s;"
    "  --headerbar-bg-color: %3$s; --headerbar-fg-color: %4$s;"
    "  --sidebar-bg-color: %3$s; --sidebar-fg-color: %4$s;"
    "  --secondary-sidebar-bg-color: %3$s; --secondary-sidebar-fg-color: %4$s;"
    "  --card-bg-color: %3$s; --card-fg-color: %4$s;"
    "  --dialog-bg-color: %3$s; --dialog-fg-color: %4$s;"
    "  --popover-bg-color: %3$s; --popover-fg-color: %4$s;"
    /* Backdrop (unfocused) variants follow libadwaita's own convention of
     * settling toward the window background. */
    "  --headerbar-backdrop-color: %1$s;"
    "  --sidebar-backdrop-color: %1$s;"
    "  --secondary-sidebar-backdrop-color: %1$s;"
    "}"
    /* Body: interior content backing (never the window node) + default text. */
    "window { color: %2$s; }"
    "toolbarview { background-color: %1$s; }"
    /* Pane dividers: the paned background shows through the separator's 1px
     * transparent grab padding on each side of the 1px line (main.c). It must
     * be the terminal background; anything else reads as a seam beside the
     * line (a hardcoded #000 measured as a black band either side of the grey
     * line on Solarized Light). Same priority as main.c's provider, but this
     * one is added later, so this rule wins the cascade. Opaque on purpose
     * (epimone_hex drops alpha), like main.c's rule: a dimmed semi-opaque
     * terminal should reveal the palette tone, not the widgets underneath. */
    "paned { background-color: %1$s; }"
    /* The top-bar revealer itself, not just its children. AdwToolbarView paints
     * this node `var(--headerbar-bg-color)`, and that var resolves against
     * libadwaita's OWN @define-color, not the one above: in GTK4 named colors are
     * per-provider, so the @define-color block above cannot reach it. Left
     * alone it fills any part of the top bar its children don't cover (the few
     * px below the header bar / around the tab bar) with libadwaita's stock
     * grey: a neutral full-width stripe between the palette-tinted header and
     * the palette body, which reads as a hard edge line. Unclassed `.top-bar`
     * so flat top bars (the preferences window) are covered too. */
    "toolbarview > .top-bar { background-color: %3$s; color: %4$s; }"
    /* Chrome: header / tab bar / sidebar / cards / popovers. `color` on tabbar
     * and on the tab nodes so tab labels take the titlebar foreground too;
     * libadwaita dims the inactive ones from currentColor, so they follow. */
    "headerbar { background-color: %3$s; color: %4$s; }"
    "tabbar { background-color: %3$s; color: %4$s; }"
    "tabbar tabbox > tab { color: %4$s; }"
    ".navigation-sidebar { background-color: %3$s; color: %4$s; }"
    ".boxed-list, .card { background-color: %3$s; }"
    "popover > contents { background-color: %3$s; color: %4$s; }"
    "popover > arrow { background-color: %3$s; }"
    /* Session overview (epimone-overview.c): the content is painted the SAME
     * titlebar tone as the header, so overview mode reads as one continuous
     * surface rather than a panel under a titlebar. This is the one
     * deliberate exception to the header-steps-off-the-body relationship
     * above. Opaque on purpose: the overview relies on it to hide its
     * offscreen render host. */
    ".epi-overview { background-color: %3$s; color: %4$s; }"
    /* The card thumbnail's backing, behind CONTAIN letterboxing: terminal
     * background, so the bars read as terminal margin (must sort after the
     * .card rule above to override its titlebar tone). */
    ".epi-thumb-backing { background-color: %1$s; }"
    /* The overview card's kill control: the BUTTON is invisible (it is only
     * the hit target), and the visible part is a small 24px translucent
     * circle on its image node, inset 6px into the thumbnail corner. The
     * circle is the palette foreground at 15/25/55% for rest/hover/active.
     * Always visible, not hover-revealed. */
    ".epi-thumb-close, .epi-thumb-close:hover, .epi-thumb-close:active {"
    "  padding: 0; border-radius: 99px; background: none; box-shadow: none; }"
    ".epi-thumb-close > image {"
    "  margin: 6px; min-width: 24px; min-height: 24px; border-radius: 9999px;"
    "  background-color: alpha(%2$s, 0.15); color: %2$s; }"
    ".epi-thumb-close:hover > image { background-color: alpha(%2$s, 0.25); }"
    ".epi-thumb-close:active > image { background-color: alpha(%2$s, 0.55); }"
    /* Detached badge: amber pill on the thumbnail. Fixed colours rather than
     * palette-derived, so it stays legible over any terminal background. */
    ".epi-thumb-badge {"
    "  background-color: alpha(#f5c211, 0.92); color: #241f00;"
    "  border-radius: 6px; padding: 0px 6px; font-size: 0.7em;"
    "  font-weight: bold; }"
    /* Selected-tab mark in the overview: a faint 1px line hugging the
     * thumbnail's edge, nothing around the caption. This is libadwaita's own
     * tabthumbnail treatment (`outline: 1px solid white/30%; outline-offset:
     * -1px` on the picture) with the fixed white swapped for the titlebar
     * foreground, so it reads on light palettes too. Outline, not border:
     * GTK draws it after the children, so the thumbnail cannot paint over
     * it, and it takes no layout space. The radius only shapes the outline
     * (the stack paints no background), matching the thumbnail's 12px
     * corners. */
    ".epi-card-current .epi-thumb {"
    "  outline-style: solid; outline-width: 1px;"
    "  outline-color: alpha(%4$s, 0.35);"
    "  outline-offset: -1px; border-radius: 12px; }"
    /* Destructive menu items: palette-adaptive text, and a faint tint of the
     * same red on hover (0.12 keeps the text at >= 4.5:1 over the blend). */
    "modelbutton.epi-destructive { color: %6$s; }"
    "modelbutton.epi-destructive:hover {"
    "  background-color: alpha(%6$s, 0.12); }"
    /* Empty on dark palettes; see separator_css above. */
    "%5$s",
    bg_hex, fg_hex, tbg_hex, tfg_hex, separator_css, destructive_hex);
  gtk_css_provider_load_from_string (provider, css);
}
