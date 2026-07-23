# CLAUDE.md — project brief for AI coding agents

Cross-platform Geany 2.x plugin ("Geany WebView Multitool (WVM)", module
`geanywebview.dll/.so`) providing web-powered tool panes: Markdown/HTML
preview, xterm.js terminals (two placements, multi-instance), a browser pane,
and small Tools actions. C11 + a C++14 backend file; GPL-2.0-only.
Read `docs/ARCHITECTURE.md` before structural changes.

## Build

| Platform | Build dir | Toolchain |
|---|---|---|
| Windows | `build/` | MSYS2 MINGW64 (gcc/g++, meson, ninja) |
| Linux | `build-linux/` | distro toolchain (`scripts/setup-linux-deps.sh`) |
| macOS | `build-macos/` | MacPorts (`/opt/local`), X11/XQuartz stack |

```sh
meson setup <builddir>            # once (or --reconfigure, see assets rule)
ninja -C <builddir>               # ZERO WARNINGS is the standard
ninja -C <builddir> devinstall    # per-user plugin dir; single module
```

## Hard rules

- **Commit only after the user has interactively verified the change**; one
  commit per feature/milestone. Never push unless asked.
- **Zero compiler warnings** on every platform. New `WvHostConfig`/
  `WvHostCallbacks` fields and `wv_host_*` functions must be added to BOTH
  backends (`host/win32.cc`, `host/gtk.c`), to the **no-SDK stub section at
  the bottom of win32.cc**, and to the struct initializers in
  `src/tools/clip_repro.c` — missing any of these breaks the *other*
  platform's build, which you usually cannot compile from here.
- **Platform code only under `src/host/` and `src/services/`**; everything
  else must stay portable. The only cross-platform surface is
  `host/wvhost.h`; add capability queries (like `wv_host_needs_find_ui()`)
  rather than `#ifdef`s in shared code.
- **Never hand-edit `assets/*/vendor/`** — vendored libs are fetched at
  pinned versions by `scripts/fetch-assets.sh` and patched via version-pinned
  sed files in `patches/` (they fail loudly on pattern drift).
- **Branding:** user-facing names carry the "(WVM)" suffix, composed from the
  `GWV_*_LABEL` macros in `plugin.h`. Code identifiers (`gwv_`,
  `geanywebview`, config path) are stable — do not rename them.
- Indentation is tabs; comments are `/* … */`; match the existing style.

## Gotchas that cost real time before (details in docs/ARCHITECTURE.md)

- Adding/removing an **asset file** requires `meson setup --reconfigure`
  (GResource manifest is generated at configure time). Content edits don't.
  `GWV_ASSET_DIR=<repo>/assets` serves from disk for a rebuild-free loop.
- **MacPorts JSC evaluates every number > 2³¹ as INT32_MIN** (incl.
  `Date.now()`). Keep JS numbers small; terminal theme colors need red
  ≤ 0x7f; see `patches/xterm-6.0.0.sed` and the polyfill in `term.js`.
- **Windows GTK3 < 3.24.38 crashes on clipboard use** with a WebView2 in
  process (upstream `queue_open_clipboard` bug) — not our bug, require newer.
- **Plain-Windows Geany resolves plugin DLL dependencies from geany.exe's
  `bin\` and already-loaded modules — never the plugin's dir.** The bundle
  has no json-glib and `lsp.dll` embeds its own copy (a second copy =
  GType-collision crash): `bridge.c` keeps its hand-rolled JSON reader —
  never link json-glib or another GObject-registering DLL. Keep
  `WebView2Loader.dll` a separate runtime file loaded by full path (the
  MSVC-only static lib can't link under MinGW).
- GTK **realize fires for hidden notebook tabs at startup, map does not** —
  the gtk backend defers first load to map (lazy shells). WebView2 keys and
  mouse **bypass GTK entirely** (native HWND): key snoopers never fire there,
  and middle-click needs the JS path (`ui.pastePrimary`), which is inert on
  WebKitGTK because the native button-2 handler consumes the event first.
- Geany's window-level key handler beats widget handlers — anything that must
  see bound chords (terminal keys, browser Ctrl+F) goes through a
  `gtk_key_snooper` scoped by focus.
- Geany loads only top-level `plugins/*.so` (all OSes; meson forces the
  suffix), compares `active_plugins` paths **literally** (macOS: use
  `/private/tmp`, never `/tmp`), and `session.conf` overrides `geany.conf` —
  delete it between scripted runs.

## Headless smoke pattern

Isolated config + grep the `-v` trace (`Loaded:`, `settings loaded:`,
`preview.rendered`, `pty.start … -> ok`; `critical=0`):

```sh
TCFG=$(mktemp -d)/geanycfg; mkdir -p "$TCFG/plugins"
cp <builddir>/geanywebview.so "$TCFG/plugins/"   # + WebView2Loader.dll on Windows
printf '[plugins]\nload_plugins=true\nactive_plugins=%s/plugins/geanywebview.so;\n' \
  "$TCFG" > "$TCFG/geany.conf"
G_MESSAGES_DEBUG=all timeout -k 3 25 geany -v -i -c "$TCFG" /path/to/test.md
```

Traps: `active_plugins` loads only from Geany's own plugin dirs (the
`$TCFG/plugins` above) and needs forward slashes — GKeyFile unescapes
backslashes on read; Windows startup can exceed 15 s (short timeouts kill
Geany before the plugin loads — looks like a plugin failure) and debuggers attached at startup
produce FALSE crashes (attach after startup); macOS has no GNU `timeout`
(background + sleep + kill) and needs `/private/tmp`; the side terminal is
visible at startup, so it spawns a shell with no extra flags, while the
message-window terminal needs `GWV_WARM_TERMINAL=1` (on WebKitGTK).
`GWV_WEBKIT_CONSOLE=1` mirrors page JS errors to stdout.

## Settings conventions

GKeyFile at `<configdir>/plugins/geanywebview/geanywebview.conf`; created
with defaults on first run. Terminal panes are driven by instance counts
(0 = disabled) — no enable flags. When changing keys, migrate old ones on
load (read legacy key if new one absent; the next save drops legacy keys)
and keep `settings_apply()` idempotent — the dialog applies live.
