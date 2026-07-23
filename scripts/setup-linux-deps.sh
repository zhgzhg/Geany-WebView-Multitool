#!/bin/sh
# Install (or print) the packages needed to build Geany WebView on Linux.
#
# Usage:
#   scripts/setup-linux-deps.sh            # print the install command for this distro
#   scripts/setup-linux-deps.sh --install  # run it (via sudo)
#
# Needed everywhere: a C/C++ toolchain, meson + ninja, and devel files for
# geany and webkit2gtk-4.1 (the GTK3 API of WebKitGTK).
set -e

if command -v dnf >/dev/null 2>&1; then
	PM="dnf"
	CMD="dnf install -y gcc gcc-c++ meson ninja-build geany geany-devel webkit2gtk4.1-devel"
elif command -v apt-get >/dev/null 2>&1; then
	PM="apt"
	CMD="apt-get install -y build-essential meson ninja-build geany libgeany-dev libwebkit2gtk-4.1-dev"
elif command -v pacman >/dev/null 2>&1; then
	PM="pacman"
	# Arch ships headers in the main packages; no -devel splits.
	CMD="pacman -S --needed --noconfirm base-devel meson ninja geany webkit2gtk-4.1"
else
	echo "Unsupported distro: install manually — a C/C++ toolchain, meson, ninja," >&2
	echo "and devel packages for: geany, webkit2gtk-4.1." >&2
	exit 1
fi

if [ "$1" = "--install" ]; then
	echo "[$PM] sudo $CMD"
	exec sudo $CMD
else
	echo "Run the following (or re-run this script with --install):"
	echo "  sudo $CMD"
fi
