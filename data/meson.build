# GSettings schema for Epimone.
#
# Installed to <datadir>/glib-2.0/schemas (compiled there by the top-level
# gnome.post_install(glib_compile_schemas: true)). gnome.compile_schemas()
# also builds a gschemas.compiled here in the build tree, so an uninstalled
# dev run can load the schema with:
#
#     GSETTINGS_SCHEMA_DIR=<builddir>/data ./<builddir>/epimone
#
# or, once for the source tree:
#
#     glib-compile-schemas data/
#
install_data('org.felix.Epimone.gschema.xml',
  install_dir: get_option('datadir') / 'glib-2.0' / 'schemas')

gnome.compile_schemas(depend_files: files('org.felix.Epimone.gschema.xml'))

# ------------------------------------------------------------------ #
# Desktop entry. Icon=org.felix.Epimone matches both the icon basenames
# installed below and the GApplication id, which is what lets the shell tie a
# running window back to this launcher.
# ------------------------------------------------------------------ #
install_data('org.felix.Epimone.desktop',
  install_dir: get_option('datadir') / 'applications')

# ------------------------------------------------------------------ #
# Application icons: the hicolor tree, copied into data/icons/ with the theme
# layout preserved. Each PNG installs to
# <datadir>/icons/hicolor/<size>/apps/org.felix.Epimone.png. PNG only.
#
# gtk-update-icon-cache runs from the top-level gnome.post_install().
# ------------------------------------------------------------------ #
# The PNGs are the pre-rendered ladder from the final asset drop
# (epimone-icon-assets/, flat body), whose master also installs as the
# scalable/ entry. The 16px PNG is the one exception: the master's 1.25px
# chevron stroke falls apart at that size (the drop's pre-rendered 16px
# included), so it comes from the hand-tuned icons/epimone_16_tuned.svg.
#
# 512x512@2 is the hi-DPI slot: hicolor's index.theme declares it Size=512
# Scale=2, i.e. 1024 physical pixels, so the 1024px art lands there and is what
# scale-2 displays and large previews actually load.
#
# There is deliberately no 1024x1024 entry: that directory is not in hicolor's
# index.theme (the theme stops at 512 for apps), so nothing ever reads it.
icon_sizes = ['16x16', '24x24', '32x32', '48x48', '64x64', '128x128',
              '256x256', '512x512', '512x512@2']

foreach size : icon_sizes
  install_data(
    'icons' / 'hicolor' / size / 'apps' / 'org.felix.Epimone.png',
    install_dir: get_option('datadir') / 'icons' / 'hicolor' / size / 'apps',
  )
endforeach

# Scalable master: what large previews and non-integer sizes rasterize from.
install_data('icons/hicolor/scalable/apps/org.felix.Epimone.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'scalable' / 'apps')

# Symbolic variant: the >_ mark alone, single-fill per GNOME conventions,
# recolored by GTK where a monochrome icon is wanted (e.g. status rows).
install_data('icons/hicolor/symbolic/apps/org.felix.Epimone-symbolic.svg',
  install_dir: get_option('datadir') / 'icons' / 'hicolor' / 'symbolic' / 'apps')
