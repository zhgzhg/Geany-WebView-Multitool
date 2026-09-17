# Architecture

The plugin is a **reusable WebView host** plus the tools built on it. One
plugin, one binary; each tool ("view") is a folder of HTML/JS embedded into
the module, talking to native code over a small message bridge.

```
Geany (GTK3)
  └─ plugin.c ............ entry: settings load, pane creation, keybindings,
     │                     Tools menu, gwv_current_doc_dir()
     ├─ settings.c ....... GKeyFile settings + the Preferences page
     ├─ typography.c ..... "Simplify Typography (WVM)" Tools action (→ ASCII)
     ├─ view.c ........... generic view lifecycle (panel/webarea/host/bridge)
     ├─ views/ ........... per-view behaviour + bridge channels
     │   ├─ preview.c .... Markdown/HTML/SVG preview (doc events, debounce, modes;
     │   │                 renders only while its pane is mapped — hidden edits
     │   │                 mark it stale and the pane's map signal catches up)
     │   ├─ terminal.c ... terminal machinery, shared by both placements
     │   ├─ sideterm.c ... the editor-side placement (paned wrapping)
     │   └─ browser.c .... free-browsing pane (nav toolbar, find bar)
     ├─ services/ ........ native services, per-OS (pty_win32.c, pty_unix.c)
     ├─ bridge.{c,h} ..... channel dispatch over the host message pipe
     ├─ assets.{c,h} ..... embedded-asset lookup (+ GWV_ASSET_DIR override)
     ├─ geanywebview.rc.in  Windows VERSIONINFO resource (File/Product version
     │                     from the VERSION file, names from plugin.h branding)
     └─ host/ ............ swappable per-OS WebView backend
         ├─ wvhost.h ..... THE seam: stable, platform-agnostic host API
         ├─ win32.cc ..... WebView2 backend (Windows)
         ├─ gtk.c ........ webkit2gtk-4.1 backend (Linux, macOS/MacPorts-X11)
         └─ cocoa.m ...... WKWebView (future: native-quartz GTK builds only)
```

The tree separates the three growth axes: adding a **tool** is a `views/`
module plus an `assets/<name>/` folder; adding an **OS** is one file in
`host/`; adding a **native capability** is a `services/` module. Nothing
outside `host/` includes a platform header.

## The host seam (`host/wvhost.h`)

One `WvHost` owns one embedded browser bound to a GTK widget. The API is
deliberately small: create/navigate/set-html/post-message/focus/visibility/
destroy, plus:

- `wv_host_new_area()` — the widget the browser fills differs per backend
  (win32: a `GtkDrawingArea` providing a native HWND; gtk: a box the
  `WebKitWebView` is packed into).
- `wv_host_format_url()` — views are served at `https://<host>/…` on Windows
  and `geanyview://<host>/…` on WebKitGTK; callers never hardcode a scheme.
- `wv_host_warmup()` — engines initialize lazily when the pane is first
  *mapped* (so a hidden terminal doesn't spawn a shell); eager views and
  reveal-by-keybinding call warmup explicitly. GTK note: *realize* fires for
  hidden notebook children at startup, *map* does not — the GTK backend keys
  on map. The Windows backend keys on the HWND being available, so hidden
  tabs there do initialize at startup (known, harmless).
- `wv_host_map_dir()` — maps an extra virtual host to a local folder (used
  for the document's directory, so relative images work in the preview).
- `wv_host_put_virtual()` — publishes an in-memory document under the asset
  host (the rendered HTML preview never touches disk).
- `WvHostConfig.allow_browsing` — pinned views (preview/terminal) cancel any
  navigation off their own asset host; the cancelled URL is first offered to
  the view via the `on_navigate_external` callback (the preview uses this to
  open doc-host links — local files next to the document — in the editor,
  and, by setting, http(s) links in the Browser pane), and otherwise opens
  in the OS browser. `target=_blank` / middle-click takes the same route.
  Browsing views (Browser pane) navigate freely and `target=_blank` stays
  in-view.
- History + find: `go_back/go_forward/reload`, `find/find_next/find_stop`,
  and the capability query `wv_host_needs_find_ui()` — WebView2 ships the
  Edge find bar (Ctrl+F is a browser accelerator there), WebKitGTK has only
  the API, so `browser.c` builds a GTK find bar just for that backend.

**Rule:** every addition to `WvHostConfig`/`WvHostCallbacks` or new
`wv_host_*` function must land in *both* backends — including the no-SDK stub
section at the bottom of `win32.cc` — and in the initializers in
`src/tools/clip_repro.c`.

## The bridge

Views load in the webview and talk only to `window.bridge`, a JS shim
(`assets/bridge.js`) injected at document start over the platform message
channel (`chrome.webview` on Windows, `webkit.messageHandlers` on WebKitGTK).
Envelopes are `{"t": "<token>", "ch": "<channel>", "p": <payload>}`; native
side registers per-channel handlers via `bridge_on()` (`bridge.c`, using its
own strict JSON reader — json-glib is off limits, see Platform quirks). The
Browser pane gets **no** bridge injection — arbitrary web sites must not see
the host message channel; its toolbar is native GTK.

The token is a per-view random secret substituted into the shim at view
creation (`view.c`): `bridge_handle()` drops envelopes that don't echo it.
This authenticates the *sender*, because the shim (and thus the token) only
exists in top-level documents of the plugin's own pages — the shim bails in
subframes (WebView2 injects user scripts into child frames; WebKitGTK is
top-frame-only but exposes `webkit.messageHandlers.bridge` to **every**
frame, which is exactly the hole the token closes: the sandboxed
HTML-preview iframe runs untrusted content that could otherwise post
`ui.copy` etc. directly).

Channel naming: `pty.*` (terminal I/O, payloads carry an instance `id`),
`preview.*`, `term.*` (page config), `ui.*` (clipboard/focus helpers).

## Asset pipeline

`assets/**` is compiled into the module as a **GResource** (prefix
`/geany/webview/`): `scripts/gen-gresource.sh` emits the manifest at
configure time, `glib-compile-resources` compiles it, and the bundle is
registered at module load. Consequences:

- After **adding/removing asset files**, re-run
  `meson setup --reconfigure <builddir>` (the file list is captured at
  configure time). Content-only edits rebuild normally.
- For a rebuild-free edit/refresh loop set `GWV_ASSET_DIR=<repo>/assets` —
  files are served from disk with per-file fallback to the embedded copy.
- Serving: the WebKitGTK backend registers the `geanyview://` scheme
  (secure + CORS-enabled, lookup order: published virtuals → embedded assets
  → mapped folders). The Windows backend intercepts `WebResourceRequested`
  on the asset host and answers from memory (`SHCreateMemStream`); mapped
  folders still use `SetVirtualHostNameToFolderMapping`.

**Vendored web libraries** (xterm.js, markdown-it, …) are fetched by
`scripts/fetch-assets.sh` at pinned versions and patched by version-pinned
sed scripts in `patches/` (loud failure if the target pattern drifts). Never
hand-edit files under `assets/*/vendor/`.

### Third-party licenses

The project itself is **GPL-2.0-only** (root `LICENSE`). Bundled components
keep their own GPL-compatible licenses — the pinned list lives in
`assets/VENDOR_VERSIONS.txt`: Microsoft WebView2 SDK (BSD-3-Clause), xterm.js
+ fit addon (MIT), markdown-it and github-markdown-css (MIT),
markdown-it-task-lists (ISC), markdown-it-anchor (Unlicense),
markdown-it-toc-done-right (MIT), highlight.js
(BSD-3-Clause), mermaid (MIT), and DOMPurify (dual Apache-2.0/MPL-2.0,
**used under MPL-2.0** — the Apache-2.0 option is GPLv2-incompatible).
Notices are retained alongside the vendored files.

## Terminals

`views/terminal.c` is placement-agnostic machinery: each view multiplexes up
to 8 xterm.js instances over one webview (an in-page tab row, HTML/CSS in
`assets/terminal/`), every `pty.*` payload carries the instance id, and the
native side keeps an `id → PTY` slot table per view. `views/sideterm.c` adds
the second placement by wrapping whatever occupies Geany's editor-area slot
(`hpaned1`, child 2) in a fresh `GtkPaned` — that anchor choice is what makes
it compose with the Split Window plugin in either activation order (both
plugins re-resolve parents at un/split time; splitwindow.c vacates a paned
slot before re-adding).

The PTY services (`services/pty_*.c`) marshal reads onto the GTK main thread
via a ref-counted link (safe teardown: kill wakes the reader; queued idles
are neutralized on free).

### Key handling

Geany's keybinding handler sits on the main window's `key-press-event` and
runs before any widget handler, so bound chords (Ctrl+K, Ctrl+W, Ctrl+F, …)
would never reach a pane. Two **GTK key snoopers** (deprecated but stable for
GTK3's lifetime — the only hook that precedes another party's window handler)
fix this:

- the terminal snooper forwards *all* keys to whichever terminal view has
  focus (same semantics as Geany's built-in VTE "override Geany keybindings");
- the browser snooper handles *only* Ctrl+F (open find bar) and Escape (close
  it) while focus is inside the browser panel — everything else still reaches
  Geany.

On Windows neither snooper ever fires: WebView2 keys go to its native HWND
and bypass GTK entirely. There the equivalent policy is
`AreBrowserAcceleratorKeysEnabled(FALSE)` for pinned views (keys belong to
the shell) and TRUE for browsing views (Ctrl+F/F5/F12 are the point).

### Clipboard / selection

Terminal selection is offered to PRIMARY (X11 semantics; GTK emulates it
process-locally on Windows), and middle-click pastes it without touching the
regular clipboard. Two routes exist by necessity: a native GTK button-2
handler that consumes the event *before* WebKit's built-in global-selection
paste (double-paste prevention, WebKitGTK platforms), and a JS
`mousedown`/`auxclick` path that is the only route on Windows (native-HWND
input) and provably inert elsewhere. Paste flows native-side
(`gtk_clipboard_wait_for_text` → `term.paste` → xterm bracketed paste) — the
browser clipboard-permission path is never used.

## Windows implementation notes

- The WebView2 controller is parented to the native HWND of a realized
  `GtkDrawingArea` (`gdk_win32_window_get_handle`); bounds/visibility follow
  `size-allocate`/`map`/`unmap`.
- No custom COM apartment or message pump: Geany's GTK Win32 backend already
  runs an STA thread with a pump, which services WebView2's async callbacks.
- `WebView2.h` is compiled directly with MinGW-w64 g++ (no WRL); interface
  IIDs are associated via `__CRT_UUID_DECL` (GUIDs verbatim from the SDK
  header). The SDK is a hash-pinned Meson wrap (`subprojects/webview2.wrap`).
- `WebView2Loader.dll` ships next to the plugin and is genuinely required:
  it is the app-carried bootstrap that *finds* the Evergreen Runtime — the
  Runtime auto-updates the browser but never installs the loader anywhere.
  win32.cc loads it at runtime by full module-dir path and resolves two
  exports (`CreateCoreWebView2EnvironmentWithOptions`,
  `GetAvailableCoreWebView2BrowserVersionString`); when the file is missing
  the plugin still loads and each pane reports "runtime not found" instead.
  The single-file alternatives lose: `WebView2LoaderStatic.lib` is MSVC-only
  object code (a MinGW link dies on MSVC CRT/ABI internals), and linking the
  import lib would move the DLL into the import table, turning a missing
  file into a hard plugin-load failure (see the dependency-resolution quirk
  below). The plugin-manager scan logging "has no plugin_version_check()"
  for the loader is harmless noise.
- The plugin calls `plugin_module_make_resident()`: the process-wide WebView2
  environment (and other static host state) must survive disable/enable.

## Platform quirks worth knowing

- **Windows resolves a plugin's DLL dependencies from geany.exe's `bin\`,
  already-loaded modules and system paths — never from the plugin's own
  directory.** A link-time dependency the Geany bundle doesn't ship fails
  the whole plugin with "The specified module could not be found" — the
  message names the plugin, not the missing DLL. Runtime helpers must be
  loaded explicitly by full path (as win32.cc does for `WebView2Loader.dll`);
  best is to need nothing beyond GTK/GLib/libgeany in the import table
  (`objdump -p geanywebview.dll | grep "DLL Name"` audits it).
- **json-glib is off limits** — the official Windows Geany bundle ships no
  json-glib DLL (importing it trips the quirk above), and geany-plugins'
  `lsp.dll` statically embeds its own copy, so a second json-glib in the
  process — shared *or* static — would collide on duplicate GType
  registration and crash on first use. `bridge.c` carries a minimal strict
  JSON reader instead; the same reasoning applies to any other
  GObject-type-registering library.
- **GTK 3.24.31 win32 clipboard crash** — a use-after-free in
  `queue_open_clipboard` (exposed by GLib ≥ 2.76's GSlice removal, fixed
  upstream in 3.24.38) crashes Geany on copy/paste with any WebView2 in the
  process. Require/ship GTK ≥ 3.24.38 on Windows (MSYS2 3.24.51 is fine).
- **MacPorts WebKitGTK JSC number corruption** — that JSC build evaluates
  *every* number above 2³¹ as INT32_MIN (including `Date.now()`). Our assets
  carry workarounds: `patches/xterm-6.0.0.sed` (MAX_BUFFER_SIZE and the
  scrollback option sanitizer — both hold the literal 4294967295, which turns
  negative there and makes `new Terminal()` throw),
  a gated `Date.now` polyfill in `term.js`, and an explicit terminal
  `selectionBackground` whose red channel ≤ 0x7f (packed-RGBA math). The
  find-in-page controller is native C API and unaffected. Arbitrary web
  sites in the Browser pane may still misbehave there — engine bug, not ours.
- **NVIDIA proprietary driver on Linux** — WebKit's DMA-BUF renderer often
  fails to allocate GBM buffers, leaving panes blank; the gtk backend sets
  `WEBKIT_DISABLE_DMABUF_RENDERER=1` automatically when `/sys/module/nvidia`
  exists (a user-exported value wins).
- **Ephemeral web contexts (WebKitGTK)** — one private context per host;
  ephemeral because the views persist nothing and multiple contexts sharing
  the default on-disk storage race over its sqlite databases (libsoup
  "database is locked").
- **Geany plugin loading** — Geany scans only top-level `plugins/*.so`
  (`G_MODULE_SUFFIX` is "so" everywhere including macOS — meson's
  `name_suffix` forces it), and `check_plugin_path` compares paths literally:
  on macOS use `/private/tmp`, never `/tmp`, in hand-written
  `active_plugins` — and only sanctioned dirs load at all (the system plugin
  dir, `<configdir>/plugins`, or the extra-path pref; anything else is
  dropped silently). In hand-written configs on Windows keep forward
  slashes: GKeyFile unescapes backslashes on read, so `C:\tmp\...` comes
  back with a literal TAB in it. `session.conf` overrides `geany.conf` once
  it exists.

## Settings model

GKeyFile at `<configdir>/plugins/geanywebview/geanywebview.conf`, created
with defaults on first run, loaded otherwise; the Preferences dialog applies
live through `settings_apply()`. Terminal panes have no enable flags — the
per-pane instance count is the switch (0 = pane disabled). Legacy keys
(`enable_terminal`, `terminal_side_panel`, `terminal_font_size` — superseded
by the Pango-style `terminal_font` — and the flat config file location) are
migrated on load and disappear on the next save.

## Development workflow

```sh
meson setup <builddir>          # build/ (Windows) · build-linux · build-macos
ninja -C <builddir>             # zero-warning policy
ninja -C <builddir> devinstall  # per-user plugin dir, single module
```

Diagnostics (all permanent, env-gated):

| Knob | Effect |
|---|---|
| `geany -v` | surfaces the plugin's `GWV:` trace (add `G_MESSAGES_DEBUG=geany-webview` on Linux/macOS) |
| `GWV_ASSET_DIR=<repo>/assets` | serve assets from disk (rebuild-free web dev loop) |
| `GWV_WEBKIT_CONSOLE=1` | mirror page console/JS errors to stdout (WebKitGTK) |
| `GWV_WARM_TERMINAL=1` | initialize the terminal without its pane being shown (headless testing) |

Headless smoke pattern (all platforms): isolated config dir with
`active_plugins` pointing at the built module, open a test document, grep the
`-v` output for `Loaded:`, `settings loaded:`, `preview.rendered`,
`pty.start … -> ok`. `preview.rendered` only fires while the preview tab is
visible (hidden panes skip rendering and log
`preview hidden -> render skipped` instead) — put `sidebar_page=2` under
`[geany]` in the isolated `geany.conf` to select the preview tab at startup
(Geany reapplies it after plugins load; 2 = after Symbols and Documents).
Platform traps: Windows Geany can take >15 s to start
(too-short timeouts kill it before plugin load, and attaching a debugger *at
startup* produces false crashes — attach after); macOS has no GNU `timeout`
(background + `sleep` + `kill`) and needs `/private/tmp` paths; delete
`session.conf` between scripted runs.

See [`TESTING.md`](TESTING.md) for the manual regression checklist and
[`history/`](history/) for the original feasibility research and milestone
plan.
