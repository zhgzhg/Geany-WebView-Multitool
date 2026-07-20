#!/bin/sh
# Generate the GResource manifest for assets/** (invoked by meson at configure
# time). Every file under assets/ is embedded into the plugin module under the
# /geany/webview/ resource prefix, zlib-compressed.
#
# NOTE: meson captures the asset FILE LIST at configure time — after adding or
# removing asset files, re-run `meson setup --reconfigure <builddir>`.
# (Content edits of existing files are tracked automatically.)
#
# Usage: gen-gresource.sh <assets-dir> <output-xml>
set -e

ASSETS="$1"
OUT="$2"

{
	printf '<?xml version="1.0" encoding="UTF-8"?>\n'
	printf '<gresources>\n'
	printf '  <gresource prefix="/geany/webview">\n'
	(cd "$ASSETS" && find . -type f ! -name '.DS_Store' ! -name '*.bak' | sed 's|^\./||' | LC_ALL=C sort) |
	while IFS= read -r f; do
		printf '    <file compressed="true">%s</file>\n' "$f"
	done
	printf '  </gresource>\n'
	printf '</gresources>\n'
} > "$OUT"

echo "gen-gresource: $(grep -c '<file' "$OUT") files -> $OUT"
