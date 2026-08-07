<div align="center">

<img src="data/icons/hicolor/scalable/apps/org.felix.Epimone.svg" width="120" alt="Epimone Terminal icon"/>

# Epimone Terminal

**Close the window. Your work keeps running.**

Epimone Terminal is a GTK4 terminal for GNOME with one job the others get wrong: it doesn't lose
your sessions. Shut the window, crash the app, log out and come back tomorrow, and your shells are
still alive with everything still running.

![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)
![Language: C](https://img.shields.io/badge/Language-C-555.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-orange.svg)

<!-- SCREENSHOT: hero shot. The main window with a couple of tabs, ideally one split into panes,
     in the default Epimone palette. This is the first thing anyone sees. Make it look good. -->
<!-- ![Epimone Terminal](docs/screenshots/hero.png) -->

</div>

---

## The problem every terminal has

You kick off a long job. A build, a scan, a copy, six hours of hashcat. Then you close the wrong
tab, or the GUI crashes, or your session drops. The job dies with the window. Everyone who lives in
a terminal has felt that exact sink in their stomach.

tmux solves it, but you have to live inside tmux: keyboard prefixes, no real GUI, a learning wall.
Epimone Terminal gives you the same safety net as a normal graphical terminal you already know how
to use.

The trick: a small background daemon owns the real terminal sessions and their scrollback. The
window is just a view. Kill the view and the sessions don't even notice.

## Persistence that actually works

This is the whole point, so here's the demo. Split a pane, start `ping` in it, close the entire
tab, then reopen it from the overview. The ping is still counting. It never stopped, because it was
never yours to stop, it was the daemon's.

<!-- GIF: the money shot. Split a tab, run `ping 8.8.8.8`, close the tab, open the overview,
     click the card, tab restores with ping still climbing (show the icmp_seq number jumping).
     This single GIF sells the whole project. -->
<!-- ![Session survives closing](docs/screenshots/persistence.gif) -->

Closing anything only detaches it. Nothing dies unless the shell exits on its own or you explicitly
kill it. Accidentally hitting close on a six-hour job is no longer a disaster, it's a shrug.

The one thing persistence can't beat is a reboot. Power off the machine and the running processes
go with the RAM. Everything short of that survives.

## Session overview

Every session you have, attached or detached, in one grid. Each card is a live thumbnail showing the
real split layout of that tab, not a text blob. Click a card and the whole arrangement comes back
exactly how you left it, same splits, same ratios, same running processes.

<!-- SCREENSHOT: the overview grid full of cards, a few of them split into multiple panes so the
     composite thumbnails are visible. This is the most visually impressive view in the app. -->
<!-- ![Session overview](docs/screenshots/overview.png) -->

Detached sessions that would otherwise be invisible are right there, ready to restore or clear out.

## Real tiling splits

A proper split tree, not a bolted-on afterthought. Split horizontally or vertically, nest as deep as
you want, drag to resize, and zoom any pane to fullscreen and back. All from the keyboard.

<!-- SCREENSHOT: one tab tiled into 3-4 panes running different things (htop, a log tail, a shell),
     showing the clean hairline dividers. -->
<!-- ![Tiling splits](docs/screenshots/splits.png) -->

## Theming that covers the whole window

Fifteen bundled palettes, and each one recolors the entire app, terminal and chrome together, not
just the text area. Light and dark both look right, and the window follows your system accent color.

<!-- SCREENSHOT: two or three windows side by side in different palettes (e.g. Dracula, Nord,
     Solarized Light) so the whole-window theming is obvious. -->
<!-- ![Palettes](docs/screenshots/themes.png) -->

## Everything else

- Shell integration with no dotfile edits. Epimone Terminal ships its own bash and zsh integration
  (OSC 7 and OSC 133) and injects it at launch, so new tabs and splits open in the current directory
  without touching your `~/.bashrc`.
- Fourteen rebindable keyboard shortcuts, with conflict detection that names the action already
  using a key before you reassign it.
- Built for the people who need it most: Linux developers, and pentesters on Kali and Parrot where a
  dropped session can cost hours.

## Status

In active development toward 1.0. The core is done and usable every day: tabs, splits, the
persistence daemon, live session restore, the overview, settings, and theming all work now. Named
workspaces, broadcast input, and packaging are still to come.

## Building from source

Epimone Terminal uses the [Meson](https://mesonbuild.com/) build system.

You'll need a C compiler (GCC or Clang), Meson, Ninja, GTK 4, libadwaita 1.5 or newer, VTE (the
GTK 4 build), and GLib.

On Debian, Ubuntu, Kali, or Parrot:

```sh
sudo apt install build-essential meson ninja-build \
    libgtk-4-dev libadwaita-1-dev libvte-2.91-gtk4-dev libglib2.0-dev
```

Build and run:

```sh
git clone https://github.com/flxdevlab/epimone.git
cd epimone
meson setup build
meson compile -C build
./build/epimone
```

Install:

```sh
sudo meson install -C build
```

The daemon starts on its own when you launch the app. You don't run it yourself.

## How it works

Three parts talking over a small protocol on a UNIX socket.

The daemon is plain C with nothing beyond libc. It owns the PTYs, keeps a scrollback ring buffer per
session, and stays alive when the GUI goes away. It never links GTK. It just holds terminals.

The GUI is GTK4, libadwaita, and VTE. It attaches to daemon sessions over a local-PTY bridge, so VTE
gets a real terminal while the daemon stays the true owner. That bridge is what lets a session
outlive its window.

`epimone-ctl` is a command-line client for listing, attaching to, and managing sessions directly.

## Contributing

Epimone Terminal is free and open-source software. Issues and merge requests are welcome. When
filing a bug, include your distro, desktop, and GTK and VTE versions.

## Credits

Epimone Terminal owes two projects in particular.

[Ptyxis](https://gitlab.gnome.org/chergert/ptyxis) by Christian Hergert was the reference for
building a clean, modern GTK4 and libadwaita terminal. Epimone Terminal's GUI and its whole-window
palette theming were shaped by studying how Ptyxis does it.

[tmux](https://github.com/tmux/tmux) is where the persistence model comes from. The idea that a
terminal session should outlive whatever is showing it is tmux's, rebuilt here as a native GUI
instead of a multiplexer you drive by keyboard prefix.

Built with [GTK](https://gtk.org), [libadwaita](https://gitlab.gnome.org/GNOME/libadwaita), and
[VTE](https://gitlab.gnome.org/GNOME/vte).

## License

Epimone Terminal is licensed under the GNU General Public License v3.0 or later (GPL-3.0-or-later).
See [`COPYING`](COPYING) for the full text.
