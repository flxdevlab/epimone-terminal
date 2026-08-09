#!/bin/sh
# build-deb-nohelper.sh — build epimone_<ver>_<arch>.deb WITHOUT debhelper,
# using only Meson + dpkg-dev (dpkg-shlibdeps, dpkg-deb). This is a fallback
# for hosts where `debhelper` is not installed; the canonical, supported build
# is `dpkg-buildpackage -b -us -uc` (see debian/README.build), which drives the
# same Meson install through debian/rules.
#
# The resulting .deb is byte-for-byte equivalent in layout and (on the same
# host) in dependencies to the debhelper build. Run it from the repo root:
#
#     sh debian/build-deb-nohelper.sh
#
# NOTE: shlibs:Depends versions are computed from THIS host's libraries. For a
# Kali target, run this (or dpkg-buildpackage) on the Kali VM so the version
# floors match Kali's shared libraries.
set -eu

# Base version from debian/changelog, plus a unique, monotonic build stamp so
# every artifact is distinguishable AND always sorts NEWER than any earlier
# build. Without this, successive rebuilds share one version string (e.g.
# 0.1.0-2) and `apt install ./epimone_0.1.0-2_amd64.deb` NO-OPS when that
# version is already installed on the target — the corrected binary never
# lands. `+bYYYYmm...` sorts after the plain changelog version (dpkg treats
# '+' > end-of-string), and later timestamps sort after earlier ones.
# Override with EPIMONE_VERSION=... for a reproducible/canonical version.
BASE_VERSION=$(dpkg-parsechangelog -SVersion)
VERSION="${EPIMONE_VERSION:-${BASE_VERSION}+b$(date -u +%Y%m%d%H%M%S)}"
ARCH=$(dpkg --print-architecture)
ROOT=$(pwd)
BUILD=$(mktemp -d)
STAGE=$(mktemp -d)
trap 'rm -rf "$BUILD" "$STAGE"' EXIT

echo ">> meson configure + build (prefix=/usr)"
meson setup "$BUILD" --prefix=/usr --buildtype=plain >/dev/null
ninja -C "$BUILD" >/dev/null

echo ">> staged install into DESTDIR"
DESTDIR="$STAGE" meson install -C "$BUILD" >/dev/null

echo ">> compute shared-library dependencies (dpkg-shlibdeps)"
DEPS=$(dpkg-shlibdeps -O \
  "$STAGE/usr/bin/epimone" \
  "$STAGE/usr/bin/epimone-daemon" \
  "$STAGE/usr/bin/epimone-ctl" 2>/dev/null | sed 's/^shlibs:Depends=//')
# Enforce the libadwaita >= 1.5 floor even if shlibdeps computes a lower one.
case "$DEPS" in
  *libadwaita-1-0*) : ;;
  *) DEPS="$DEPS, libadwaita-1-0 (>= 1.5)" ;;
esac

ISIZE=$(du -k -s "$STAGE/usr" | cut -f1)

echo ">> assemble DEBIAN control"
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: epimone
Version: $VERSION
Architecture: $ARCH
Maintainer: $(dpkg-parsechangelog -SMaintainer)
Installed-Size: $ISIZE
Depends: $DEPS
Section: x11
Priority: optional
Homepage: https://github.com/felix/epimone
Description: Persistent tabbed terminal emulator for GNOME
 Epimone is a GTK4/libadwaita tabbed terminal emulator with a session
 persistence daemon: terminal sessions survive the GUI closing and are
 restored on the next launch. It provides split panes, a tab overview,
 a command palette, shell integration, and a preferences UI.
 .
 This package ships the GUI (epimone), the persistence daemon
 (epimone-daemon) and its control tool (epimone-ctl).
EOF

# maintainer scripts (strip the debhelper token used by the dh build)
sed '/#DEBHELPER#/d' "$ROOT/debian/postinst" > "$STAGE/DEBIAN/postinst"
sed '/#DEBHELPER#/d' "$ROOT/debian/postrm"  > "$STAGE/DEBIAN/postrm"
chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

OUT="$ROOT/epimone_${VERSION}_${ARCH}.deb"
echo ">> pack"
if command -v fakeroot >/dev/null 2>&1; then
  fakeroot dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
else
  dpkg-deb --root-owner-group --build "$STAGE" "$OUT"
fi

echo ">> built: $OUT"

# Provenance: version + a fingerprint of the packaged GUI binary, so a given
# artifact can be matched against the tested tree and against what is installed
# on the target (compare with `sha256sum $(which epimone)` on the box).
echo ">> version:     $VERSION"
echo ">> epimone bin: sha256 $(sha256sum "$STAGE/usr/bin/epimone" | cut -d' ' -f1)"
