# Geany WebView

A **reusable WebView host pane for [Geany](https://www.geany.org/) 2.x**, plus the
views built on it. The host embeds a native browser control into Geany's sidebar
or message-window notebook; individual *views* are folders of HTML/JS that talk to
a small native bridge. Planned first views: a **ConPTY terminal** (xterm.js) and a
**Markdown/HTML preview**.

Cross-platform by design — WebView2 on Windows, WebKitGTK (webkit2gtk-4.1) on
Linux, WKWebView on macOS — behind one platform-agnostic host interface
(`src/wvhost.h`). **Windows is implemented first.**

> Status: **Phase 0 (spike) complete.** On Windows a WebView2 control renders in
> the Geany sidebar and a bidirectional JS⇄native bridge works. See
> [`IMPLEMENTATION_PLAN.md`](IMPLEMENTATION_PLAN.md) for the roadmap and
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
ninja -C build devinstall      # copies the DLL + WebView2Loader.dll + assets
                               # into %APPDATA%/geany/plugins
```

Then start Geany, open **Tools ▸ Plugin Manager**, and enable **Geany WebView**.
A "WebView" tab appears in the sidebar. Tools ▸ *Geany WebView* reveals it.

To see internal trace logging, launch Geany with `-v` (Geany's verbose mode
surfaces the plugin's `GWV:` debug messages; `G_MESSAGES_DEBUG` alone won't,
because Geany installs its own log handler).

## Architecture

```
Geany (GTK3)
  └─ plugin.c ............ registers the plugin, adds the sidebar pane + menu,
     │                     routes bridge messages to native services
     └─ wvhost.h .......... stable, platform-agnostic host interface
          ├─ host_win32.cc . WebView2 backend (this platform)
          ├─ host_gtk.c .... webkit2gtk-4.1 backend        (planned, M4)
          └─ host_cocoa.m .. WKWebView backend             (planned, M5)

Views (assets/<view>/) load in the webview and talk only to `window.bridge`,
a thin JS shim over the platform message channel (chrome.webview on Windows,
webkit.messageHandlers on Linux).
```

The host is intentionally small and view-agnostic: create/navigate/set-html/
post-message/focus/visibility/destroy. Adding a new view is meant to be "drop a
folder of assets", not "re-do the embedding".

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
meson.build, meson_options.txt   build
subprojects/webview2.wrap        WebView2 SDK (fetched/verified by Meson)
subprojects/packagefiles/…       overlay meson.build for the SDK
src/plugin.c                     Geany plugin + pane + bridge routing
src/wvhost.h                     host interface (the reusable core)
src/host_win32.cc                WebView2 backend
assets/                          web views (installed next to the DLL)
scripts/devinstall.sh            dev install to %APPDATA%/geany/plugins
FEASIBILITY.md                   research + sources
IMPLEMENTATION_PLAN.md           milestones M0–M5
```

## License

**GPL-2.0-or-later** (see [`COPYING`](COPYING)) — aligned with the Geany
ecosystem. Bundled third-party components keep their own permissive licenses:
the Microsoft WebView2 SDK (BSD-3-Clause) and, once added, xterm.js and
markdown-it (MIT). Their notices are retained alongside the vendored assets.
