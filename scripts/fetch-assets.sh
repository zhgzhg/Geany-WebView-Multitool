#!/bin/sh
# Fetch and vendor the plugin's web assets at pinned versions.
#
# Re-run any time to refresh the files under assets/**/vendor/. To upgrade a
# library, bump its version below and re-run. Exact versions are recorded in
# assets/VENDOR_VERSIONS.txt (committed) so the vendored tree is reproducible.
#
# SPDX-License-Identifier: GPL-2.0-only
set -e
cd "$(dirname "$0")/.."   # repo root

# ----------------------------- pinned versions -----------------------------
XTERM=6.0.0
XTERM_FIT=0.11.0
XTERM_SEARCH=0.16.0
MARKDOWN_IT=14.3.0
MD_TASK_LISTS=2.1.1
MD_ANCHOR=9.2.1
MD_TOC=4.2.0
DOMPURIFY=3.4.14
GH_MD_CSS=5.9.0
HLJS=11.12.0
MERMAID=11.17.2

CDN=https://cdn.jsdelivr.net/npm

fetch()       { printf '  %s\n' "$2"; curl -fsSL -o "$2" "$1"; }
strip_srcmap() { for f in "$@"; do sed -i '/sourceMappingURL/d' "$f" 2>/dev/null || true; done; }

echo "terminal view:"
mkdir -p assets/terminal/vendor
fetch "$CDN/@xterm/xterm@$XTERM/lib/xterm.js"               assets/terminal/vendor/xterm.js
fetch "$CDN/@xterm/xterm@$XTERM/css/xterm.css"              assets/terminal/vendor/xterm.css

# Apply version-pinned patches to freshly fetched vendor files. Each patch is a
# sed script named patches/<lib>-<version>.sed documenting why it exists; the
# vendored files are single-line minified blobs, so sed scripts stay readable
# where unified diffs would not. A patch that no longer matches (version bump
# changed the code) fails the fetch loudly instead of drifting silently.
apply_patch() {
	_sedfile="$1"; _target="$2"
	[ -f "$_sedfile" ] || return 0
	_before=$(cksum < "$_target")
	sed -i.bak -f "$_sedfile" "$_target" && rm -f "$_target.bak"
	_after=$(cksum < "$_target")
	if [ "$_before" = "$_after" ]; then
		echo "ERROR: $_sedfile did not change $_target — pattern drift after version bump?" >&2
		exit 1
	fi
	echo "patched: $_target ($_sedfile)"
}

apply_patch "patches/xterm-$XTERM.sed" assets/terminal/vendor/xterm.js
fetch "$CDN/@xterm/addon-fit@$XTERM_FIT/lib/addon-fit.js"   assets/terminal/vendor/addon-fit.js
fetch "$CDN/@xterm/addon-search@$XTERM_SEARCH/lib/addon-search.js" assets/terminal/vendor/addon-search.js
fetch "$CDN/@xterm/xterm@$XTERM/LICENSE"                    assets/terminal/vendor/LICENSE
strip_srcmap assets/terminal/vendor/xterm.js assets/terminal/vendor/addon-fit.js \
             assets/terminal/vendor/addon-search.js

echo "preview view:"
mkdir -p assets/preview/vendor
fetch "$CDN/markdown-it@$MARKDOWN_IT/dist/markdown-it.min.js"                         assets/preview/vendor/markdown-it.min.js
fetch "$CDN/markdown-it-task-lists@$MD_TASK_LISTS/dist/markdown-it-task-lists.min.js" assets/preview/vendor/markdown-it-task-lists.min.js
fetch "$CDN/markdown-it-anchor@$MD_ANCHOR/dist/markdownItAnchor.umd.js"               assets/preview/vendor/markdown-it-anchor.js
fetch "$CDN/markdown-it-toc-done-right@$MD_TOC/dist/markdownItTocDoneRight.umd.js"    assets/preview/vendor/markdown-it-toc.js
fetch "$CDN/dompurify@$DOMPURIFY/dist/purify.min.js"                                  assets/preview/vendor/purify.min.js
fetch "$CDN/github-markdown-css@$GH_MD_CSS/github-markdown-dark.css"                  assets/preview/vendor/github-markdown-dark.css
fetch "$CDN/github-markdown-css@$GH_MD_CSS/github-markdown-light.css"                 assets/preview/vendor/github-markdown-light.css
fetch "$CDN/@highlightjs/cdn-assets@$HLJS/highlight.min.js"                           assets/preview/vendor/highlight.min.js
fetch "$CDN/@highlightjs/cdn-assets@$HLJS/styles/github-dark.min.css"                 assets/preview/vendor/highlight-github-dark.css
fetch "$CDN/@highlightjs/cdn-assets@$HLJS/styles/github.min.css"                      assets/preview/vendor/highlight-github-light.css
fetch "$CDN/mermaid@$MERMAID/dist/mermaid.min.js"                                     assets/preview/vendor/mermaid.min.js
strip_srcmap assets/preview/vendor/markdown-it.min.js assets/preview/vendor/markdown-it-task-lists.min.js \
             assets/preview/vendor/purify.min.js assets/preview/vendor/highlight.min.js \
             assets/preview/vendor/mermaid.min.js assets/preview/vendor/markdown-it-toc.js

{
	echo "# Vendored web-asset versions and licenses."
	echo "# Regenerate with scripts/fetch-assets.sh."
	echo ""
	printf '%-24s %-9s %s\n' "library"                "version"        "license"
	printf '%-24s %-9s %s\n' "@xterm/xterm"           "$XTERM"         "MIT"
	printf '%-24s %-9s %s\n' "@xterm/addon-fit"       "$XTERM_FIT"     "MIT"
	printf '%-24s %-9s %s\n' "@xterm/addon-search"    "$XTERM_SEARCH"  "MIT"
	printf '%-24s %-9s %s\n' "markdown-it"            "$MARKDOWN_IT"   "MIT"
	printf '%-24s %-9s %s\n' "markdown-it-task-lists" "$MD_TASK_LISTS" "ISC"
	printf '%-24s %-9s %s\n' "markdown-it-anchor"     "$MD_ANCHOR"     "Unlicense"
	printf '%-26s %-7s %s\n' "markdown-it-toc-done-right" "$MD_TOC"    "MIT"
	printf '%-24s %-9s %s\n' "dompurify"              "$DOMPURIFY"     "Apache-2.0 OR MPL-2.0"
	printf '%-24s %-9s %s\n' "github-markdown-css"    "$GH_MD_CSS"     "MIT"
	printf '%-24s %-9s %s\n' "highlight.js"           "$HLJS"          "BSD-3-Clause"
	printf '%-24s %-9s %s\n' "mermaid"                "$MERMAID"       "MIT"
} > assets/VENDOR_VERSIONS.txt

echo "wrote assets/VENDOR_VERSIONS.txt"
echo "done."
