# Epimone: a tabbed terminal for GNOME.
#
# NOTE: this file was reconstructed on 2026-08-06 after the original was
# accidentally overwritten by a zip archive of the icon assets (a stray
# `zip meson.build ...`; the same archive also exists as files.zip). The
# targets, sources, dependencies, flags, and install layout below were
# recovered from build/meson-info introspection, build/build.ninja, and
# build/compile_commands.json of the last successful configure, so they
# match the original build exactly; the comments are new.

project('epimone', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
  ])

cc = meson.get_compiler('c')
gnome = import('gnome')

# Standing rule: warning_level=2 with -Wno-unused-parameter as the only
# suppression. _GNU_SOURCE for openpty()/login_tty() and friends.
add_project_arguments('-Wno-unused-parameter', '-D_GNU_SOURCE', language: 'c')

gtk_dep = dependency('gtk4')
adw_dep = dependency('libadwaita-1')
vte_dep = dependency('vte-2.91-gtk4')
gio_dep = dependency('gio-2.0')
util_dep = cc.find_library('util')
m_dep = cc.find_library('m')

# ------------------------------------------------------------------ #
# GUI client
# ------------------------------------------------------------------ #
epimone_sources = files(
  'src/main.c',
  'src/epimone-window.c',
  'src/epimone-settings.c',
  'src/epimone-chrome.c',
  'src/epimone-palette.c',
  'src/epimone-page.c',
  'src/epimone-overview.c',
  'src/epimone-shortcuts.c',
  'src/epimone-client.c',
  'src/epimone-layout.c',
)

# Absolute in-tree paths for uninstalled runs, both guarded at runtime on
# the directory existing: ./build/epimone adds data/icons to the icon search
# path (see epimone_setup_icon) and finds the shell-integration scripts in
# the source tree. Installed builds skip both and use the system locations.
epimone_icons_src = meson.current_source_dir() / 'data' / 'icons'
epimone_shell_integration_src = meson.current_source_dir() / 'src' / 'shell-integration'

executable('epimone', epimone_sources,
  c_args: [
    '-DEPIMONE_ICONS_SRC="@0@"'.format(epimone_icons_src),
    '-DEPIMONE_SHELL_INTEGRATION_SRC="@0@"'.format(epimone_shell_integration_src),
  ],
  dependencies: [gtk_dep, adw_dep, vte_dep, gio_dep, util_dep, m_dep],
  install: true)

# ------------------------------------------------------------------ #
# Session daemon and control tool (plain C, no GLib)
# ------------------------------------------------------------------ #
epimone_daemon_sources = files(
  'src/daemon/main.c',
  'src/daemon/server.c',
  'src/daemon/session.c',
  'src/daemon/ring.c',
  'src/daemon/group.c',
  'src/daemon/paths.c',
  'src/daemon/log.c',
)

executable('epimone-daemon', epimone_daemon_sources,
  dependencies: [util_dep],
  install: true)

executable('epimone-ctl',
  files('src/ctl/main.c', 'src/daemon/paths.c'),
  install: true)

# ------------------------------------------------------------------ #
# Shell integration scripts
# ------------------------------------------------------------------ #
shell_integration_dir = get_option('datadir') / 'epimone' / 'shell-integration'

install_data(
  'src/shell-integration/epimone.bash',
  'src/shell-integration/epimone.zsh',
  'src/shell-integration/epimone.fish',
  install_dir: shell_integration_dir)

install_data(
  'src/shell-integration/zsh/.zshenv',
  'src/shell-integration/zsh/.zprofile',
  'src/shell-integration/zsh/.zshrc',
  'src/shell-integration/zsh/.zlogin',
  install_dir: shell_integration_dir / 'zsh')

subdir('data')

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
