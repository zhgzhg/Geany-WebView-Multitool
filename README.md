# Geany WebView

A **reusable WebView host pane for [Geany](https://www.geany.org/) 2.x**, plus the
views built on it. The host embeds a native browser control into Geany's sidebar
or message-window notebook; individual *views* are folders of HTML/JS that talk to
a small native bridge. Planned first views: a **ConPTY terminal** (xterm.js) and a
**Markdown/HTML preview**.

Cross-platform by design — WebView2 on Windows, WebKitGTK (webkit2gtk-4.1) on
Linux **and on macOS** (MacPorts GTK3/X11) — behind one platform-agnostic host
interface (`src/host/wvhost.h`).

> Status: **Windows, Linux and macOS are working.** All platforms render the
> **Markdown/HTML preview** (GitHub-flavored markdown, local images, dark/light
> toggle, code-copy) in the sidebar and run a real shell in the message-window
> **terminal** (ConPTY + PowerShell/cmd on Windows, forkpty + `$SHELL` on
> Linux/macOS), with per-plugin settings under Plugin Manager ▸ Preferences.
> See [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) for the roadmap and
> [`FEASIBILITY.md`](FEASIBILITY.md) for the research behind the design.

## Requirements (Windows)

- **Windows 10 1809 (build 17763) or newer** — required by both WebView2 and
  ConPTY. Older Windows is not supported.
- **Microsoft Edge WebView2 Runtime** — preinstalled on Windows 11 and virtually
  all Windows 10; otherwise install the
  [Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/).
- **Geany 2.x** (API ≥ 235). Tested against `mingw-w64-x86_64-geany` 2.1.
- Build toolchain: **MSYS2 mingw64** — `gcc`/`g++` (C++14), `meson`, `ninja`,
  `pkg-config`, and the Geany dev headers:
  ```
  pacman -S mingw-w64-x86_64-{gcc,meson,ninja,pkgconf,geany}
  ```

The Microsoft WebView2 SDK is fetched automatically by Meson (a wrap pinned to a
known hash); nothing to install by hand.

## Build & install (from an MSYS2 MINGW64 shell)

```sh
meson setup build
ninja -C build
ninja -C build devinstall      # copies the DLL + WebView2Loader.dll
                               # into %APPDATA%/geany/plugins
```

Then start Geany, open **Tools ▸ Plugin Manager**, and enable **Geany WebView**.
A "Preview" tab appears in the sidebar and a "Terminal" tab in the message
window; both can be toggled under the plugin's **Preferences**.

## Requirements & build (Linux)

- **Geany 2.x** (API ≥ 235), **webkit2gtk-4.1** (the GTK3 API of WebKitGTK),
  **json-glib**, and a toolchain (gcc, meson, ninja).
- `scripts/setup-linux-deps.sh` prints the exact install command for your
  distro (`--install` runs it via sudo):

  | Distro | Packages |
  |---|---|
  | Fedora | `gcc gcc-c++ meson ninja-build geany geany-devel webkit2gtk4.1-devel json-glib-devel` |
  | Debian/Ubuntu | `build-essential meson ninja-build geany libgeany-dev libwebkit2gtk-4.1-dev libjson-glib-dev` |
  | Arch | `base-devel meson ninja geany webkit2gtk-4.1 json-glib` |

```sh
meson setup build-linux
ninja -C build-linux
ninja -C build-linux devinstall    # per-user dev install: geanywebview.so
                                   # into ~/.config/geany/plugins
sudo meson install -C build-linux  # OR system-wide: into <libdir>/geany
                                   # (the Geany plugin dir), for packaging
```

The terminal runs your `$SHELL` over a pty; web views are served over an
internal `geanyview://` URI scheme (the WebKit equivalent of the Windows
virtual-host mapping).

## Requirements & build (macOS)

Targets the **MacPorts** Geany stack, which runs GTK3 under X11/XQuartz — the
same WebKitGTK backend as Linux is used (a native-quartz WKWebView backend
would only apply to a quartz GTK build, which MacPorts does not ship):

```sh
sudo port install geany webkit2-gtk meson ninja pkgconfig
meson setup build-macos
ninja -C build-macos
ninja -C build-macos devinstall    # per-user: ~/.config/geany/plugins
```

Note for hand-written `active_plugins` entries on macOS: Geany only loads
plugins from its known plugin directories and compares paths literally, and
macOS `/tmp` is a symlink to `/private/tmp` — use the resolved path.

## Settings

**Plugin Manager ▸ Geany WebView ▸ Preferences** (stored in
`<geany config>/plugins/geanywebview/geanywebview.conf`, created with defaults on first
run):

- enable/disable the **preview** and **terminal** panes (applied live),
- **primary-selection** copy/paste in the terminal (select copies,
  middle-click pastes; on by default),
- **default preview mode** — auto (by file type), Markdown, or HTML; the
  preview toolbar changes it too and both persist,
- **terminal shell command** — full command line, empty for the platform
  default (`$SHELL`, or pwsh/powershell/cmd on Windows); applies when the
  shell next starts.

The preview's dark/light background toggle is also persisted.

## Trace logging

Launch Geany with `-v` to surface the plugin's `GWV:` debug messages (on
Windows, Geany's own log handler shows them with `-v` alone; on Linux also set
`G_MESSAGES_DEBUG=geany-webview`, or `=all`).

## Architecture

```
Geany (GTK3)
  └─ plugin.c ............ entry: settings, menus, keybindings, wiring
     ├─ settings.c ....... settings (GKeyFile) + Preferences page
     ├─ view.c ........... generic view lifecycle (create/navigate/reveal/destroy)
     ├─ views/ ........... per-view behaviour + bridge channels
     │   ├─ preview.c .... Markdown / HTML preview
     │   └─ terminal.c ... ConPTY terminal
     ├─ services/ ........ native services, per-OS   (pty_win32.c …)
     └─ host/ ............ swappable per-OS WebView backend
         ├─ wvhost.h ..... stable, platform-agnostic host interface
         ├─ win32.cc ..... WebView2 backend (this platform)
         ├─ gtk.c ........ webkit2gtk-4.1 backend (Linux, macOS/MacPorts-X11)
         └─ cocoa.m ...... WKWebView backend (future: native-quartz GTK builds)

Views (assets/<view>/) load in the webview and talk only to `window.bridge`,
a thin JS shim over the platform message channel (chrome.webview on Windows,
webkit.messageHandlers on Linux).
```

The host is intentionally small and view-agnostic: create/navigate/set-html/
post-message/focus/visibility/destroy. The tree separates the three growth axes —
per-OS backends (`host/`), native services (`services/`), and views/tools
(`views/` + `assets/<name>/`) — so adding a tool is "a native module plus a folder
of assets", and adding an OS is "one file in `host/`", not "re-do the embedding".

### Windows implementation notes

- The WebView2 controller is parented to the native `HWND` of a realized
  `GtkDrawingArea` (`gdk_win32_window_get_handle`); bounds/visibility follow the
  pane's `size-allocate` / `map` / `unmap` signals.
- No custom COM apartment or message loop: Geany's GTK Win32 backend already runs
  an STA thread with a message pump, which services WebView2's async callbacks.
- `WebView2.h` is compiled directly with MinGW-w64 g++ (no WRL); interface IIDs
  are associated via `__CRT_UUID_DECL`. `WebView2Loader.dll` is loaded dynamically
  (explicit link) from next to the plugin.

## Repository layout

```
meson.build                      build (src/ is the include root)
subprojects/webview2.wrap        WebView2 SDK (fetched/verified by Meson)
subprojects/packagefiles/…       overlay meson.build for the SDK
src/plugin.{c,h}                 entry point + shared types (GwvState/GwvView)
src/settings.{c,h}               settings + Preferences page
src/view.{c,h}                   generic view lifecycle (the reusable core)
src/views/<name>.{c,h}           per-view behaviour (preview, terminal)
src/host/                        per-OS WebView backend (wvhost.h + win32.cc …)
src/services/                    native services (pty …)
src/bridge.{c,h}                 messaging
src/assets.{c,h}                 embedded-asset access (+ GWV_ASSET_DIR override)
src/gwvutil.{c,h}                helpers
src/tools/                       diagnostics (clip_repro.c, pty_test.c)
assets/<view>/                   web views — EMBEDDED into the module at build
                                 time as a GResource (scripts/gen-gresource.sh)
patches/                         version-pinned patches for vendored web libs
scripts/                         devinstall.sh · fetch-assets.sh ·
                                 gen-gresource.sh · setup-linux-deps.sh ·
                                 catch-crash.sh
FEASIBILITY.md                   research + sources
IMPLEMENTATION_PLAN.md           milestones M0–M5
```

## Development notes

- Assets are compiled into the plugin, so a normal edit needs a rebuild. Set
  `GWV_ASSET_DIR=<path-to-repo>/assets` to serve from disk instead (per-file
  fallback to the embedded copy) for a rebuild-free edit/refresh loop.
- The asset file list is captured at configure time — after adding/removing
  asset files, re-run `meson setup --reconfigure <builddir>`.
- Diagnostics: `GWV_WEBKIT_CONSOLE=1` mirrors page-console/JS errors to stdout
  (WebKitGTK backends); `GWV_WARM_TERMINAL=1` starts the terminal without its
  pane being shown. Both pair well with `geany -v` and
  `G_MESSAGES_DEBUG=geany-webview`.

## License

**GPL-2.0-or-later** (see [`COPYING`](COPYING)) — aligned with the Geany
ecosystem. Bundled third-party components keep their own permissive licenses:
the Microsoft WebView2 SDK (BSD-3-Clause) and **xterm.js** + its fit addon
(MIT, `assets/terminal/vendor/`, license retained there). Their notices are
kept alongside the vendored assets.
