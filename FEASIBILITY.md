# Feasibility Research: Cross-Platform WebView Host Plugin for Geany 2.x

*Research date: 2026-07-18. Claims marked **[verified]** were confirmed in primary
sources (docs, source code, package repos); claims marked **[plausible]** come from
forums/issues or are inferences from verified facts.*

## Verdict

**Feasible on all three platforms.** Windows and Linux with high confidence;
macOS is doable but the hardest and rightly lower priority.

- **Linux is already proven** — Geany's own maintained `markdown` and `webhelper`
  plugins embed a `WebKitWebView` directly into the sidebar / message-window
  notebooks today, against `webkit2gtk-4.1` (GTK3). Our plugin does the same. **[verified]**
- **Windows works, and in-pane embedding is less risky than assumed** — GTK 3.24's
  `gdk_win32_window_get_handle()` auto-promotes a widget to a native HWND, WebView2's
  controller parents to any HWND, GTK's win32 backend already pumps all Win32
  messages, and the GTK/Geany main thread is already STA COM — exactly what WebView2
  requires. The wxWidgets `wxWebViewEdge` source is a complete reference
  implementation of "WebView2 as a child of a foreign toolkit HWND". **[verified]**
- **macOS is feasible-but-unproven** — WebKitGTK does not exist on macOS, so the
  path is WKWebView as a native NSView subview of the GTK-quartz widget's NSView
  (`gdk_quartz_window_get_nsview()`, ObjC translation unit required). Mechanically
  established pattern (GStreamer video overlay does handle-based embedding this way),
  but no complete WKWebView-in-GTK3-quartz prior art exists. **[verified availability
  of the APIs; the combination is unproven]**
- **The niche is unclaimed and wanted**: no Geany plugin anywhere combines
  webview/xterm.js/ConPTY. Geany has an **open feature request for a Windows
  terminal since 2015** ([geany#675](https://github.com/geany/geany/issues/675));
  VTE is explicitly disabled on Windows in Geany's build. **[verified]**

## Key decision changes vs. the original plan

### 1. Do NOT use the webview/webview library — use direct per-platform APIs

The original plan picked [webview/webview](https://github.com/webview/webview) as
the embedding layer. Research says drop it:

- Its official embedding target is a **toplevel window** (`GtkWindow*` / `NSWindow*`
  contentView / fill-parent HWND), not a child pane. `webview_set_size()` resizes
  *your host toplevel*; the destructor assumes the widget lives in the window you
  passed; on macOS it **replaces the NSWindow's contentView** (would blow away
  Geany's UI); on Windows `webview_create()` blocks in a nested message pump during
  WebView2 init. **[verified from source]**
- On Linux it is a thin wrapper (~200 LOC equivalent) over exactly what Geany's
  markdown plugin already does directly with WebKitGTK.
- Low velocity: last release 0.12.0 (Sept 2024), no release in ~22 months; open
  issue asking for host-loop integration with no maintainer commitment
  ([#1121](https://github.com/webview/webview/issues/1121)). **[verified]**
- Critical trap it *would* have introduced: current wrappers increasingly target
  GTK4 (`saucer` is GTK4-only). **Loading GTK4 into a GTK3 process is a hard
  runtime abort** (`Gtk-ERROR: GTK 2/3 symbols detected`). webview/webview *can*
  be forced to GTK3/webkit2gtk-4.1 (its default), but direct APIs remove the
  footgun entirely. **[verified]**

What remains valuable from webview/webview: its MinGW-compatible WebView2 loader
logic (registry-based runtime discovery) as **reference code**, and its JS-shim
pattern (one `window.bridge` API over `chrome.webview` vs `webkit.messageHandlers`).

**The plugin defines its own small "WebViewHost" interface** with three backends:

| Platform | Backend | Status |
|----------|---------|--------|
| Windows | WebView2 COM API (`WebView2.h`, C or C++ w/o WRL) | verified viable under MinGW-w64 |
| Linux | `webkit2gtk-4.1` (`WebKitWebView` widget) | proven by Geany's own plugins |
| macOS | WKWebView via ObjC + `gdk_quartz_window_get_nsview()` | feasible, unproven, defer |

### 2. In-pane embedding on Windows can be attempted in the first spike

The original plan deferred sidebar embedding to Phase 4 as "riskiest". Research
downgrades that risk substantially:

- `gdk_win32_window_get_handle()` calls `gdk_window_ensure_native()` internally
  (GTK 3.24 source) — any realized widget's GdkWindow can become a real HWND.
  The GStreamer VideoOverlay docs document exactly this embedding pattern. **[verified]**
- GDK's win32 event source dispatches **all** thread messages (not just GDK
  windows'), so WebView2's `Chrome_WidgetWin_*` children and STA COM callbacks get
  serviced by Geany's existing main loop. No second loop, no `webview_run()`. **[verified
  pump mechanics; "WebView2 under GLib loop" specifically has no published prior art]**
- GTK initializes OLE as STA (`gdk_win32_ensure_ole()`), Geany itself calls
  `CoInitialize(NULL)` (STA). WebView2 requires STA + message pump — already
  satisfied. **Do not call `CoInitializeEx(COINIT_MULTITHREADED)` anywhere.** **[verified]**
- The host must do manually: `size-allocate` → `put_Bounds`; `focus-in-event` →
  `MoveFocus`; toplevel `configure-event` → `NotifyParentWindowPositionChanged`;
  `unrealize` → `Close()`. This is the wxWebViewEdge recipe. **[verified]**

Keep a floating `GtkWindow` fallback in the spike in case pane embedding surprises us —
but it's a fallback, not the plan.

### 3. The real #1 Windows risk: keyboard focus / accelerators

When the WebView2 child HWND has focus, **Geany's GTK accelerators never see
keystrokes** (messages go to Chromium's HWND). This is an open bug even in
wxWidgets ([wx#24786](https://github.com/wxWidgets/wxWidgets/issues/24786)). **[verified]**

Mitigations, in order:
1. **`AllowHostInputProcessing`** (`ICoreWebView2ControllerOptions4`) — designed fix,
   stable since SDK 1.0.3351.48 / **Runtime ≥ 138** (July 2025). Must be set before
   controller creation. Caveats: Tab/Shift-Tab move focus out to host; Alt activates
   host menubar; throws `E_NOINTERFACE` on older runtimes → feature-detect. **[verified]**
2. `add_AcceleratorKeyPressed` handler forwarding chosen combos to Geany's
   `keybindings_*` API (works on all runtimes; wx rejected it for re-entrancy in
   their generic case, but forwarding a *fixed small set* of combos is safe). **[plausible]**
3. For the terminal view specifically, most keys *should* go to the terminal;
   define one reserved "focus editor" chord handled in-page via `onData`/`attachCustomKeyEventHandler`
   and bridged out. **[plausible]**

Note IME edge cases exist in WebView2 composition hosting; we use windowed hosting,
where IME is generally fine. **[verified]**

## Per-platform findings

### Windows (build & runtime)

- **MinGW-w64 builds WebView2 clients fine.** `WebView2.h` (from the NuGet package)
  compiles under g++ ≥ C++14 with current mingw-w64 headers (`__CRT_UUID_DECL`
  emulation for `__uuidof`, `eventtoken.h` present). Avoid MSVC-only WRL
  `Callback<>` — hand-roll tiny `IUnknown` handler classes (wx pattern) or use the
  flat-C `CINTERFACE` API ([jchv/webview2-in-mingw](https://github.com/jchv/webview2-in-mingw)). **[verified]**
- **MSYS2 packages the SDK**: `mingw-w64-webview2-loader` 1.0.3912.50 = official
  NuGet repack (`WebView2.h` + `WebView2Loader.dll`), BSD-3-Clause. ⚠ Built for
  **ucrt64/clang64/clangarm64 only — not mingw64**. Our currently installed Geany is
  `mingw-w64-x86_64-geany` (mingw64). Options: (a) develop in **ucrt64** (Geany is
  packaged there too) and keep Geany+plugin in one env; (b) stay mingw64 and
  **vendor** the NuGet headers + `WebView2Loader.dll` in the repo (they're just
  files; redistribution is sanctioned). Rule: **build the plugin in the same MSYS2
  env as the Geany binary it loads into.** **[verified packaging; env-matching is
  standard ABI hygiene]**
- **Loader strategy**: LoadLibrary `WebView2Loader.dll` at runtime and resolve just
  `CreateCoreWebView2EnvironmentWithOptions` + `GetAvailableCoreWebView2BrowserVersionString`
  (exact wxWidgets pattern; static lib is MSVC-only). **[verified]**
- **Runtime availability is a non-issue**: preinstalled on Win11, pushed via Windows
  Update to virtually all Win10 since 2022 (not on Windows Server). Detect via
  `GetAvailableCoreWebView2BrowserVersionString` → on miss, prompt with the 2 MB
  bootstrapper (`MicrosoftEdgeWebview2Setup.exe /silent /install`); shipping the
  bootstrapper is an explicitly documented workflow. **[verified]**
- **User data folder**: WebView2's default UDF is beside the host exe — fails under
  `C:\Program Files\Geany`. Pass an explicit UDF (e.g. under `g_get_user_cache_dir()`)
  to `CreateCoreWebView2EnvironmentWithOptions`. **[standard WebView2 practice]**
- Serve bundled view assets via `SetVirtualHostNameToFolderMapping` (or `file://`
  during the spike). **[standard practice]**

### Linux

- Target **`webkit2gtk-4.1`** (GTK3 + libsoup3). It survived the March 2026 removal
  of the old 4.0/libsoup2 API and has **no announced EOL**; shipped by current
  WebKitGTK 2.52.x and packaged on Debian 13, Ubuntu 24.04/26.04, Fedora 42+.
  Do not offer a 4.0 fallback (dead; and libsoup2+3 in one process crashes). **[verified]**
- Embedding recipe = copy the maintained `markdown`/`webhelper` plugins:
  `WebKitWebView` in a `GtkScrolledWindow`, `gtk_notebook_append_page()` into
  `geany->main_widgets->sidebar_notebook` or `->message_window_notebook`. **[verified]**
- GTK3 is safe to target for years: Geany 2.2-dev still requires GTK 3.24; the GTK4
  port is discussion-stage only (Scintilla GTK4 ports incomplete; maintainers say
  it won't happen quickly). **[verified]**
- Terminal view is redundant on Linux (native VTE exists) but works via
  `forkpty()` for parity/testing of the bridge.

### macOS (defer; keep the seam)

- Geany ships an official GTK3-quartz app bundle (geany-osx, JHBuild). WebKitGTK is
  **not** available (Homebrew bottles Linux-only; geany-osx doesn't build it). **[verified]**
- Path: `gdk_quartz_window_get_nsview()` from `gdkquartz-cocoa-access.h` (ObjC-only
  header → plugin file must be `.m`), add WKWebView as subview of the widget's
  backing `GdkQuartzView`, manual frame sync on `size-allocate` (flipped coords).
  GDK doesn't track foreign subviews → expect z-order/focus quirks. Prototype early
  *when we get to it*; treat as the highest-risk backend. **[verified APIs; combination unproven]**
- PTY: `forkpty()` in `<util.h>` — trivial. **[verified]**

## Terminal stack (Windows)

- **ConPTY from plain C/MinGW is solid.** `CreatePseudoConsole` /
  `ResizePseudoConsole` / `ClosePseudoConsole`, Win10 1809+, in mingw-w64 headers
  since v7.0.0 (2019). **Gotcha: declarations are hidden unless
  `-D_WIN32_WINNT=0x0A00`.** Microsoft's EchoCon sample shows full plumbing
  (pipes → pseudoconsole → `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` →
  `CreateProcessW` with `EXTENDED_STARTUPINFO_PRESENT`). **[verified]**
- Sharp edges (all documented): service input/output pipes on **separate threads**
  (marshal to GTK main thread via `g_idle_add`); close child-side pipe handles
  after `CreateProcess`; keep draining output through `ClosePseudoConsole` or it
  deadlocks pre-Win11-24H2; closing the pseudoconsole kills the process tree;
  debounce resize (~100–200 ms) — ConPTY re-renders and can desync on aggressive
  resizes. **[verified]**
- **ConPTY speaks UTF-8 + VT both ways; xterm.js is UTF-8-only — perfect match, no
  transcoding.** xterm.js 6.0 (MIT, Dec 2025, `@xterm/xterm` + `@xterm/addon-fit`)
  has `windowsPty: { backend: 'conpty' }` heuristics. Vendor the assets (no CDN —
  offline use, CSP). **[verified]**
- PowerShell 7 / cmd / normal CLI apps under ConPTY is the mainstream path —
  Windows Terminal itself runs everything through it. **[verified]**
- Precedents of exactly our stack: `tfx-for-windows` (xterm.js in WebView2 +
  ConPTY, C#), `ptyqt` (C++/Qt + xterm.js over WebSocket). **[verified]**

## Bridge (one JS API, two native transports)

| Direction | WebView2 (Windows) | WebKitGTK (Linux) |
|-----------|--------------------|--------------------|
| JS → native | `window.chrome.webview.postMessage()` → `add_WebMessageReceived` | `window.webkit.messageHandlers.<name>.postMessage()` → `script-message-received::<name>` |
| native → JS | `PostWebMessageAsJson/AsString` | `webkit_web_view_evaluate_javascript()` (`run_javascript` is deprecated since 2.40) |
| inject shim | `AddScriptToExecuteOnDocumentCreated` | `webkit_user_content_manager_add_script()` |

- Ship a tiny `bridge.js` exposing `bridge.post(msg)` / `bridge.onMessage(cb)` over
  whichever transport exists — the webview/webview pattern. Views code against the
  shim only.
- WebView2 web messages are **strings/JSON only (no binary)**. For terminal data:
  JSON strings with flow control — pause ConPTY reads when the frontend lags
  (node-pty's XON/XOFF precedent). Escape hatch if throughput ever bites:
  `PostSharedBufferToScript` (SDK ≥ 1.0.1661.34) or a localhost WebSocket. Terminal
  rendering speed, not transport, is the practical ceiling. **[verified APIs;
  throughput judgment is plausible]**

## Revised phased plan

**Phase 0 — Embed + bridge spike (Windows).**
GtkDrawingArea page in the sidebar notebook → realize → `GDK_WINDOW_HWND` →
WebView2 environment (explicit UDF) → controller → static HTML → round-trip
postMessage. Floating-window fallback only if pane embedding misbehaves.
*Exit: webview lives in a Geany pane; bridge echoes both ways; resize tracks.*

**Phase 1 — Host v1 + Linux port.**
`WebViewHost` interface (create/attach/navigate/setHtml/postMessage/destroy);
view registry (folder of static assets + manifest); asset serving (virtual host
mapping on Windows, `load_html`/`file://` base URI on Linux); WebView2-missing
detection + bootstrap prompt. Bring up the **WebKitGTK backend now** — it's cheap
(markdown-plugin precedent) and keeps the host API honest before views are built
on it. *Exit: same dummy view runs in a pane on Windows and Linux from one asset folder.*

**Phase 2 — Terminal view.**
xterm.js + fit addon vendored; ConPTY backend (`-D_WIN32_WINNT=0x0A00`, reader/writer
threads, `g_idle_add` marshaling, resize debounce, flow control); `forkpty` backend
on Linux (parity/testing); shell configurable (PowerShell 7 → `powershell.exe` →
`cmd`); "Open Terminal" menu + keybinding via `plugin_set_key_group`. Solve the
focus-escape chord here (mitigation ladder above). *Exit: real PowerShell in a
Geany pane on Windows; same view runs on Linux. This closes geany#675 territory.*

**Phase 3 — Markdown/HTML preview view.**
Render Markdown **in-page with a JS library** (marked/markdown-it, vendored) —
zero native deps, identical on all platforms (native cmark stays an option later).
`document-activate`/`document-save`/`editor-notify` signals → debounced buffer
push over the bridge. HTML files: navigate to `file://`. *Exit: live preview for
.md/.html on Windows + Linux.*

**Phase 4 — Polish + extensibility.**
Keyboard/focus hardening (AllowHostInputProcessing where Runtime ≥ 138, accelerator
forwarding elsewhere); `NotifyParentWindowPositionChanged` wiring; view-pack
manifest documented; third example view. *Exit: adding a view = dropping a folder.*

**Phase 5 — macOS backend (optional).**
WKWebView subview via `gdk_quartz_window_get_nsview()` (ObjC), `forkpty` terminal,
geany-osx packaging. Highest per-platform risk; nothing in Phases 0–4 blocks on it
if the host interface stays clean.

## Updated risk table

| Risk | Original assessment | Post-research assessment |
|------|---------------------|--------------------------|
| GTK↔WebView2 HWND embedding | "riskiest, defer to Phase 4" | **Downgraded**: verified primitives + wx/GStreamer prior art; attempt in Phase 0 with floating fallback |
| Keyboard/accelerators when webview focused | not identified | **Promoted to #1 Windows risk**; mitigation ladder exists (AllowHostInputProcessing / accelerator forwarding / in-page chord) |
| webview/webview library misfit | chosen as foundation | **Dropped** — toplevel-oriented, GTK4 footgun, low velocity; direct APIs instead |
| ConPTY quirks | flagged | Confirmed manageable; specific recipes documented (threads, drain-before-close, resize debounce) |
| WebView2 runtime missing | flagged | Confirmed near-non-issue (Win11 preinstalled, Win10 ~universal); detect + bootstrapper |
| Toolchain (mingw64 vs ucrt64) | not identified | New: webview2-loader pkg is ucrt64/clang64-only; either move to ucrt64 or vendor NuGet files; match Geany's build env |
| Linux WebKitGTK availability | out of scope | Confirmed healthy: webkit2gtk-4.1 shipped everywhere, no EOL; use 4.1 only |
| macOS embedding | "later" | Confirmed hardest part; APIs exist, combination unproven; keep deferred |
| GTK4 future breaking GTK3 plugin | not identified | Non-risk for years: Geany GTK4 port is discussion-only |

## Environment facts (this machine)

- MSYS2 with `mingw-w64-x86_64-geany 2.1-2` installed: `pkg-config geany` → 2.1,
  **API 250, ABI 73**, GTK 3.24.31, full plugin headers in `/mingw64/include/geany`.
- `gdkwin32.h` present with `GDK_WINDOW_HWND()` / `gdk_win32_window_get_handle()`.
- Working sample plugin builds in `~/Geany-Base64-Converter` etc. — the Makefile
  pattern (`pkg-config geany`, `MINGW_PREFIX` branch, install to
  `%APPDATA%/geany/plugins`) is the template for this plugin's build.
- Full Geany source tree at `~/geany-msys-success` (built successfully, installer
  scripts working) — reference for internals; confirms VTE is compiled out on
  Windows (`meson.build:128`).

## Key sources

- WebView2 threading: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/threading-model
- WebView2 distribution: https://learn.microsoft.com/en-us/microsoft-edge/webview2/concepts/distribution
- wxWebViewEdge (reference impl): https://github.com/wxWidgets/wxWidgets/blob/master/src/msw/webview_edge.cpp
- Accelerator bug + fix: https://github.com/wxWidgets/wxWidgets/issues/24786 , `ICoreWebView2ControllerOptions4::AllowHostInputProcessing`
- MinGW WebView2: https://github.com/jchv/webview2-in-mingw , https://packages.msys2.org/base/mingw-w64-webview2-loader
- webview/webview internals: `core/include/webview/detail/backends/{gtk_webkitgtk.hh,win32_edge.hh,cocoa_webkit.hh}`
- WebKitGTK API versions: https://blogs.gnome.org/mcatanzaro/2025/04/28/webkitgtk-api-versions/ , https://webkitgtk.org/2025/10/07/webkitgtk-soup2-deprecation.html
- Geany markdown plugin embedding: https://github.com/geany/geany-plugins/blob/master/markdown/src/plugin.c
- ConPTY: https://learn.microsoft.com/en-us/windows/console/creating-a-pseudoconsole-session , EchoCon sample: https://github.com/microsoft/terminal/tree/main/samples/ConPTY/EchoCon
- xterm.js 6.0: https://github.com/xtermjs/xterm.js/releases/tag/6.0.0
- Windows terminal feature request: https://github.com/geany/geany/issues/675
- GDK quartz NSView access: `gdk/quartz/gdkquartz-cocoa-access.h` (gtk-3-24)
- Bridge APIs: https://learn.microsoft.com/en-us/microsoft-edge/webview2/how-to/communicate-btwn-web-native , https://webkitgtk.org/reference/webkit2gtk/stable/class.UserContentManager.html
