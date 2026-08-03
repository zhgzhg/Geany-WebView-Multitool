# Geany WebView Multitool (WVM)

A cross-platform plugin for [Geany](https://www.geany.org/) 2.x that adds a
set of web-powered tool panes — every pane you see marked **(WVM)** comes from
this plugin:

- **File Preview (WVM)** *(sidebar)* — live GitHub-flavored **Markdown** and
  **HTML** preview of the current document: local images, section links,
  [Mermaid](https://mermaid.js.org/) diagrams (```` ```mermaid ```` fences,
  and bare `.mmd`/`.mermaid` files) with per-diagram zoom and drag-to-pan
  controls, GitLab-style `[[_TOC_]]` / `[TOC]` tables of contents,
  dark/light background toggle, code-block copy buttons; links to local
  files open them in the editor, external links in your OS browser.
- **Terminal (WVM)** *(message window, and optionally right of the editor)* —
  a real shell terminal (xterm.js): ConPTY + PowerShell/cmd on Windows,
  forkpty + `$SHELL` on Linux/macOS. Up to 8 instances per pane with in-pane
  tabs and an activity indicator on background tabs. The side pane composes
  with the Split Window plugin and always stays right of the split.
- **Web Browser (WVM)** *(sidebar)* — a small web browser with address bar,
  back/forward/reload/home and find-in-page (Ctrl+F) — browse documentation
  or your `localhost` dev server without leaving Geany.
- **Copy File Path (WVM)** *(Tools menu, optional)* — copies the active
  document's absolute path to the clipboard.

Everything is rendered by the platform's native browser engine — **WebView2**
on Windows, **WebKitGTK** on Linux and macOS (MacPorts/X11) — behind one
plugin, one binary, with all web assets embedded. Nothing installs next to
the plugin file except, on Windows, Microsoft's `WebView2Loader.dll`
companion (placed automatically by `devinstall`).

## Install & build

### Windows (MSYS2 MINGW64)

Requires **Windows 10 1809+** (WebView2 and ConPTY need it) and the
[WebView2 Runtime](https://developer.microsoft.com/microsoft-edge/webview2/)
(preinstalled on Windows 11 and virtually all Windows 10).

```sh
pacman -S mingw-w64-x86_64-{gcc,meson,ninja,pkgconf,geany}
meson setup build          # fetches the hash-pinned WebView2 SDK
ninja -C build
ninja -C build devinstall  # per-user: %APPDATA%/geany/plugins
```

`devinstall` places two files: the plugin and `WebView2Loader.dll` — the
app-shipped bootstrap that locates the WebView2 Runtime, required next to
the plugin. To install into a plain (installer-based) Geany, copy those same
two files into `%APPDATA%\geany\plugins` (per-user, no admin rights) or
`C:\Program Files\Geany\lib\geany` (all users); the plugin needs no other
DLLs beyond what Geany itself bundles.

### Linux

Requires Geany 2.x (API ≥ 235) and **webkit2gtk-4.1**.
`scripts/setup-linux-deps.sh` prints the exact command for your distro
(`--install` runs it):

| Distro | Packages |
|---|---|
| Fedora | `gcc gcc-c++ meson ninja-build geany geany-devel webkit2gtk4.1-devel` |
| Debian/Ubuntu | `build-essential meson ninja-build geany libgeany-dev libwebkit2gtk-4.1-dev` |
| Arch | `base-devel meson ninja geany webkit2gtk-4.1` |

```sh
meson setup build-linux
ninja -C build-linux
ninja -C build-linux devinstall    # per-user: ~/.config/geany/plugins
sudo meson install -C build-linux  # OR system-wide (for packaging)
```

### macOS (MacPorts)

Targets the MacPorts Geany stack (GTK3 under X11/XQuartz; the same WebKitGTK
backend as Linux):

```sh
sudo port install geany webkit2-gtk meson ninja pkgconfig
meson setup build-macos
ninja -C build-macos
ninja -C build-macos devinstall    # per-user: ~/.config/geany/plugins
```

### Enable it

Start Geany, open **Tools ▸ Plugin Manager**, tick **Geany WebView Multitool
(WVM)**. The panes appear immediately; configure them via the plugin's
**Preferences** button (or Edit ▸ Plugin Preferences).

## Settings

Stored in `<geany config>/plugins/geanywebview/geanywebview.conf` (created
with defaults on first run; edited from the Preferences dialog — changes apply
live):

| Group | Setting |
|---|---|
| File Preview | show in the sidebar; default mode (auto / Markdown / HTML) — the preview toolbar changes it too, both persist, as does the dark/light toggle |
| Browser | show in the sidebar; home page (empty = `about:blank`), used at pane open and by the Home button |
| Terminal (shared) | shell command (empty = platform default); primary-selection copy/paste (select copies, middle-click pastes — independent of the regular clipboard); font size (6–32, applies live to both panes) |
| Terminal — message window | instance count **0–8**; **0 disables the pane** |
| Terminal — right of the editor | instance count **0–8**; **0 disables the pane** |
| Tools | "Copy File Path (WVM)" menu item |

With more than one terminal instance, a tab row inside the pane switches
between shells; each shell starts when its tab is first opened, and a
background tab lights up on new output until viewed.

Terminal keys: plain keystrokes (including Ctrl+K, Ctrl+W, …) go to the
shell while a terminal is focused. **Ctrl+Shift+C/V** copy/paste via the
regular clipboard, **Ctrl+Shift+E** focuses the editor, `exit` + Enter
restarts the shell.

## Troubleshooting

- **Panes show "WebView2 Runtime … not found" (Windows)** — install the
  [Evergreen Runtime](https://developer.microsoft.com/microsoft-edge/webview2/),
  and make sure `WebView2Loader.dll` sits next to `geanywebview.dll`.
- **Plugin fails to load with "The specified module could not be found"
  (Windows)** — despite naming the plugin, this means one of its DLL
  *dependencies* is missing: Windows resolves them from Geany's `bin\`
  folder, never from the plugin directory. Release builds depend only on
  DLLs Geany bundles; if you rebuilt with extra libraries, that's the cause
  (details in `docs/ARCHITECTURE.md`).
- **Blank panes on Linux with the NVIDIA proprietary driver** — the plugin
  auto-sets `WEBKIT_DISABLE_DMABUF_RENDERER=1` when it detects the driver; if
  you still see blank panes, export it yourself before starting Geany.
- **Trace logging** — launch `geany -v` to see the plugin's `GWV:` messages
  (on Linux/macOS also set `G_MESSAGES_DEBUG=geany-webview`, or `=all`).
- **Browser logins don't persist (Linux/macOS)** — by design: the WebKitGTK
  panes use ephemeral web storage; nothing is written to disk.

## Documentation

- [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) — how it works: the
  platform-agnostic host interface, the bridge, the asset pipeline, per-OS
  implementation notes, development workflow and diagnostics.
- [`docs/TESTING.md`](docs/TESTING.md) — the manual regression checklist.
- [`docs/history/`](docs/history/) — the original feasibility research and
  the milestone plan the plugin grew from.
- [`CLAUDE.md`](CLAUDE.md) — project conventions for AI coding agents.

## Other Useful Plugins

* [Geany JSON Prettifier](https://github.com/zhgzhg/Geany-JSON-Prettifier)
* [Geany Generic SQL Formatter](https://github.com/zhgzhg/Geany-Generic-SQL-Formatter)
* [Geany Unix Timestamp Converter](https://github.com/zhgzhg/Geany-Unix-Timestamp-Converter)
* [Geany Base64 Converter](https://github.com/zhgzhg/Geany-Base64-Converter)
