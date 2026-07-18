#!/bin/sh
# Copy the freshly built plugin DLL + web assets into the per-user Geany
# plugin directory. Invoked by `ninja -C build devinstall`.
#
# Meson passes the built module path as $1, then any extra runtime files (e.g.
# WebView2Loader.dll) as $2..$N, and sets MESON_SOURCE_ROOT / MESON_BUILD_ROOT
# in the environment.
set -e

DLL="$1"
if [ -z "$DLL" ]; then
	DLL="$MESON_BUILD_ROOT/geanywebview.dll"
fi
shift 2>/dev/null || true

if [ -z "$APPDATA" ]; then
	echo "devinstall: APPDATA is not set; are you in an MSYS2/Windows shell?" >&2
	exit 1
fi

DEST="$(cygpath -u "$APPDATA")/geany/plugins"
ASSETS="$DEST/geanywebview"

mkdir -p "$ASSETS"
cp -f "$DLL" "$DEST/"

# Remaining args are runtime files that must sit next to the plugin DLL
# (WebView2Loader.dll is located relative to the plugin's own module dir).
for f in "$@"; do
	[ -n "$f" ] && cp -f "$f" "$DEST/"
done

if [ -d "$MESON_SOURCE_ROOT/assets" ]; then
	cp -rf "$MESON_SOURCE_ROOT/assets/." "$ASSETS/"
fi

echo "devinstall: installed $(basename "$DLL") + assets (+$# runtime file(s)) into $DEST"
