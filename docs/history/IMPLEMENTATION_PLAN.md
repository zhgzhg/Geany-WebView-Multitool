# Implementation Plan: Geany WebView Host Plugin

*Companion to `FEASIBILITY.md` (research + sources). Windows first; Linux in M4;
macOS deferred. Plan date: 2026-07-18.*

## Ground rules (decisions locked by research)

1. **No webview/webview library.** Direct WebView2 COM on Windows, direct
   `webkit2gtk-4.1` on Linux, behind our own `WvHost` C interface.
2. **In-pane from day one.** GtkDrawingArea in Geany's notebooks → native HWND →
   WebView2 controller. Floating window only as debug fallback (one `#ifdef`).
3. **One host instance per pane, one shared WebView2 environment.** Terminal lives
   in the message-window notebook (bottom, where VTE sits on Linux), preview in the
   sidebar notebook. Separate controllers mean the terminal session survives while
   the preview navigates. Do **not** multiplex views over a single webview.
4. **Toolchain: MSYS2 mingw64** (matches installed `mingw-w64-x86_64-geany` 2.1 and
   the self-built installer). WebView2 SDK files are **vendored from NuGet** (the
   MSYS2 `webview2-loader` package is ucrt64/clang64-only). Compile everything with
   `-D_WIN32_WINNT=0x0A00` (unlocks ConPTY declarations in mingw-w64 headers).
5. **Language:** plugin core in C (matches Geany plugin idiom and the existing
   sample plugins); the WebView2 backend in one C++14 translation unit
   (`host_win32.cc`, hand-rolled `IUnknown` handlers — no WRL) exposing `extern "C"`.
   Link with `g++`.
6. **Never call `CoInitializeEx(COINIT_MULTITHREADED)`.** The GTK/Geany main thread
   is already STA; WebView2 callbacks arrive via GTK's existing message pump.
7. **All web assets vendored** (xterm.js, markdown-it, our JS) — no CDN, works
   offline, stable CSP.
8. **Build system: Meson + Ninja** (already installed in mingw64; Geany itself
   builds with Meson). Chosen over plain Make and CMake because it removes real
   work from this specific project:
   - `shared_module()` is the purpose-built target for plugins — emits `.dll` /
     `.so` / macOS `-bundle` correctly per OS with zero conditionals (the sample
     Makefiles hand-roll all of this).
   - `dependency('geany')` / `dependency('webkit2gtk-4.1')` via pkg-config, with
     clean `host_machine.system()` branching for backend/service source selection.
   - Mixed C / C++14 in one target for free; **Objective-C is first-class** —
     M5's `host_cocoa.m` needs no build-system work at all.
   - The WebView2 NuGet package is a zip: a **Meson wrap** downloads, hash-verifies
     and extracts it at `meson setup` time — the fetch script disappears.
   - `compile_commands.json` for clangd, and the same two commands
     (`meson setup build && ninja -C build`) on every OS and in CI.
   Caveat noted: geany-plugins upstream is autotools; if this plugin is ever
   upstreamed there, `build/*.m4` glue must be written then regardless of what we
   use locally. Standalone distribution loses nothing.
9. **Product decisions (confirmed 2026-07-18):**
   - **Name:** "Geany WebView", binary `geanywebview.dll`/`.so`.
   - **License: GPL-2+** (Geany-ecosystem aligned; keeps geany-plugins upstreaming
     open; bundled MIT/BSD-3 assets are compatible — keep their notices in
     `assets/` and credit in README).
   - **Windows floor: Win10 1809+ hard.** On older Windows the plugin activates
     only far enough to show a clear "requires Windows 10 1809+" message
     (`RtlGetVersion`/build ≥ 17763 check at init) — no webview, no terminal, no
     soft-degrade investment.
   - **Dev/test target: MSYS2 pacman Geany** (`mingw-w64-x86_64-geany`), matching
     the pkg-config headers exactly; the `C:\Program Files\Geany` build is
     verified in M4.4, not before.

## Repo layout

```
geany-webview/
├─ meson.build                  # project(), deps, shared_module(), install rules
├─ meson_options.txt            # plugindir / local-install options
├─ README.md
├─ FEASIBILITY.md               # research (done)
├─ IMPLEMENTATION_PLAN.md       # this file
├─ subprojects/
│  ├─ webview2.wrap             # NuGet Microsoft.Web.WebView2 zip + sha256
│  └─ packagefiles/
│     └─ webview2/meson.build   # overlay: declare_dependency(include_directories),
│                               #   exposes x64/WebView2Loader.dll for install
├─ src/
│  ├─ plugin.c                  # geany_load_module, menus, keybindings, panes
│  ├─ wvhost.h                  # WvHost C interface (the stable core)
│  ├─ host_win32.cc             # WebView2 backend (C++14, extern "C")
│  ├─ host_gtk.c                # WebKitGTK backend            [M4]
│  ├─ bridge.c / bridge.h       # message routing view ⇄ native services
│  ├─ util.c / util.h           # asset-dir discovery, base64, json helpers
│  └─ services/
│     ├─ pty.h                  # PTY service interface
│     ├─ pty_win32.c            # ConPTY                        [M2]
│     └─ pty_posix.c            # forkpty                       [M4]
└─ assets/                      # installed next to the DLL
   ├─ bridge.js                 # transport shim (chrome.webview ⇄ webkit)
   ├─ hello/index.html          # M0 spike + bridge self-test page
   ├─ terminal/                 # index.html, term.js, xterm.js, addon-fit [M2]
   └─ markdown/                 # index.html, preview.js, markdown-it     [M3]
```

Install layout: `%APPDATA%/geany/plugins/geanywebview.dll` plus
`%APPDATA%/geany/plugins/geanywebview/` (assets + `WebView2Loader.dll`).
Dev loop: `meson setup build && ninja -C build install` with
`-Dprefix`/`plugindir` pointed at the local plugin dir (a `devinstall` run target
wraps this). Asset dir discovered at runtime via
`g_win32_get_package_installation_directory_of_module()` on Windows /
`dladdr()` on Unix — never hardcoded.

Root `meson.build` sketch (illustrates all the moving parts):

```meson
project('geany-webview', 'c', 'cpp',
  default_options: ['c_std=c11', 'cpp_std=c++14'])

geany_dep = dependency('geany')          # pulls GTK3 transitively
deps      = [geany_dep]
sources   = ['src/plugin.c', 'src/bridge.c', 'src/util.c']

if host_machine.system() == 'windows'
  webview2_dep = subproject('webview2').get_variable('webview2_dep')
  deps += [webview2_dep]
  sources += ['src/host_win32.cc', 'src/services/pty_win32.c']
  add_project_arguments('-D_WIN32_WINNT=0x0A00', language: ['c', 'cpp'])
  deps += declare_dependency(link_args: ['-lole32', '-lshlwapi', '-lversion', '-ladvapi32'])
else
  deps += [dependency('webkit2gtk-4.1')]
  sources += ['src/host_gtk.c', 'src/services/pty_posix.c']
endif

shared_module('geanywebview', sources,
  dependencies: deps,
  name_prefix: '',                       # geanywebview.dll, not libgeanywebview.dll
  install: true, install_dir: plugindir)

install_subdir('assets', install_dir: plugindir / 'geanywebview', strip_directory: true)
```

## The stable core: `wvhost.h`

```c
typedef struct WvHost WvHost;

typedef struct {
    void (*on_ready)  (WvHost *h, gpointer user);          /* async init done   */
    void (*on_message)(WvHost *h, const char *json, gpointer user);
    void (*on_failed) (WvHost *h, const char *error, gpointer user);
} WvHostCallbacks;

WvHost  *wv_host_new         (GtkWidget *container,        /* realized parent  */
                              const WvHostCallbacks *cb, gpointer user);
void     wv_host_navigate    (WvHost *h, const char *url); /* queued if !ready */
void     wv_host_set_html    (WvHost *h, const char *html);
void     wv_host_post_message(WvHost *h, const char *json);/* → window.bridge  */
void     wv_host_focus       (WvHost *h);
void     wv_host_set_visible (WvHost *h, gboolean visible);
void     wv_host_destroy     (WvHost *h);

gboolean wv_host_runtime_available(char **version_out);    /* WebView2 detect  */
```

Backend contract (implemented per platform, invisible to callers):
- **Creation is async** (WebView2). `WvHost` queues `navigate`/`post_message`
  calls until `on_ready`; views must also wait for `bridge.ready`.
- Windows backend wiring: `container` `realize` → `GDK_WINDOW_HWND` →
  `CreateCoreWebView2Controller`; `size-allocate` → `put_Bounds`;
  notebook `switch-page`/`map`/`unmap` → `put_IsVisible`; toplevel
  `configure-event` → `NotifyParentWindowPositionChanged`; `focus-in-event` →
  `MoveFocus(PROGRAMMATIC)`; destroy → `Close()` then release.
- Loader: `LoadLibraryW` on our vendored `WebView2Loader.dll` (absolute path
  next to assets), resolve only `CreateCoreWebView2EnvironmentWithOptions` +
  `GetAvailableCoreWebView2BrowserVersionString` (wxWidgets pattern).
- Environment (created once, shared): explicit user-data folder
  `g_get_user_cache_dir()/geany-webview/`; assets mapped via
  `SetVirtualHostNameToFolderMapping("geanyview.local", <assets>, DENY_CORS)` →
  views load as `https://geanyview.local/<view>/index.html`.

## Bridge protocol (frozen in M1)

`bridge.js` (injected via `AddScriptToExecuteOnDocumentCreated` /
`webkit_user_content_manager_add_script`) exposes to every view:

```js
bridge.post(channel, payload)       // → native
bridge.on(channel, cb)              // ← native
bridge.ready                        // Promise
```

Envelope, both directions: `{"ch": "<channel>", "p": <payload>}`.

| Channel | Dir | Payload |
|---------|-----|---------|
| `sys.ready` | js→c | `{view}` — page booted |
| `sys.error` | both | `{msg}` |
| `doc.active` | c→js | `{path, filetype}` on document-activate |
| `doc.text` | c→js | `{text, path}` (preview pushes) |
| `pty.start` | js→c | `{cols, rows}` (shell/cwd resolved natively) |
| `pty.data` | both | base64 string (UTF-8 bytes; b64 avoids split-codepoint + JSON-escaping issues) |
| `pty.resize` | js→c | `{cols, rows}` |
| `pty.exit` | c→js | `{code}` |
| `ui.focusEditor` | js→c | `{}` — escape chord; native calls `keybindings_send_command(...FOCUS_EDITOR)` |

Native side: `bridge.c` keeps a `channel → handler` table per host instance;
services (PTY, preview) register handlers. All native→JS sends must happen on the
GTK main thread (`g_idle_add` from worker threads).

---

## Milestone 0 — Spike: pane embedding + bridge (est. 2–4 days)

Goal: retire the two Windows unknowns (pane embedding, bridge) with throwaway-grade
code in the real plugin skeleton.

- [ ] **M0.1** Repo skeleton + root `meson.build` (sketch above) + `devinstall`
      run target installing DLL+assets into `%APPDATA%/geany/plugins`; `git init`,
      GPL-2+ `COPYING` + SPDX headers.
      Build inside the MSYS2 **MINGW64** shell: `meson setup build && ninja -C build`.
- [ ] **M0.2** `subprojects/webview2.wrap`: `[wrap-file]` pointing at
      `https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2/1.0.4191.47`
      (a `.nupkg` is a zip) with sha256, plus the `packagefiles/webview2/meson.build`
      overlay declaring `webview2_dep` (include dir `build/native/include`) and a
      `webview2_loader_dll` variable (`build/native/x64/WebView2Loader.dll`) that
      the root install rules copy into the assets dir. Verify `WebView2.h`
      compiles in a smoke TU.
- [ ] **M0.3** Plugin registers (`geany_load_module` / `GEANY_PLUGIN_REGISTER`),
      adds a "WebView" page (GtkDrawingArea in GtkBox) to
      `geany->main_widgets->sidebar_notebook`, Tools-menu item, clean unload
      (page removed, no warnings on `pacman`-Geany restart-with-plugin-manager cycle).
- [ ] **M0.4** `host_win32.cc` minimal: runtime detect → env (explicit UDF) →
      controller parented to the widget HWND → `NavigateToString` hello page.
      Confirm rendering inside the sidebar.
- [ ] **M0.5** Resize/visibility wiring (`size-allocate` → `put_Bounds`;
      hidden-tab → `put_IsVisible(FALSE)`). Check: drag sidebar splitter, switch
      tabs, minimize/restore, move window between monitors (DPI).
- [ ] **M0.6** Bridge round-trip on the hello page: button posts to native →
      statusbar message; native menu item posts to JS → DOM update. Include a
      stress button (1000 messages, 1 MB message) — numbers inform M2 flow control.
- [ ] **M0.7** Teardown truth-table: unload plugin, quit Geany with pane open,
      quit while page loading. No crash, no zombie `msedgewebview2.exe`.

**Exit:** WebView2 renders and echoes messages inside a Geany sidebar pane, resize
tracks, load/unload clean. **Fallback trigger:** if pane embedding fails in some
unfixable way (focus loops, paint corruption), switch `wv_host_new` to a floating
`GtkWindow` parented to Geany's toplevel and continue — the rest of the plan is
unchanged (that's the point of `WvHost`).

## Milestone 1 — Host v1: real asset pipeline + lifecycle (est. 3–5 days)

- [ ] **M1.1** Freeze `wvhost.h` (API above) and refactor the spike behind it.
      Queued-until-ready semantics; `on_failed` path renders a native GTK error
      box with the WebView2 bootstrapper link when the runtime is missing, and
      the Win10-1809+ floor message on older Windows (ground rule 9).
- [ ] **M1.2** Asset serving: install assets beside the DLL; virtual-host mapping;
      `bridge.js` injection; CSP meta in view pages (`default-src 'self'`).
- [ ] **M1.3** `bridge.c` channel routing + `sys.ready` handshake; JSON via GLib
      (`json-glib` if available in mingw64, else a minimal encoder — decision at
      implementation time, record in README).
- [ ] **M1.4** Two placements wired: sidebar page + message-window page, each with
      its own `WvHost`, shared environment. Menu toggles per pane.
- [ ] **M1.5** Settings skeleton (GKeyFile in `geany->app->configdir`, like the
      sample plugins): per-view enable, shell path (used in M2).
- [ ] **M1.6** Manual test checklist doc (`TESTING.md`): the M0.5/M0.7 matrices,
      run on plugin-manager enable/disable, Geany session restart.

**Exit:** `hello` view loads from `https://geanyview.local/hello/` in either pane;
runtime-missing path shows the install prompt; API of `wvhost.h` untouched by
later milestones (this is the "reusable host" success criterion).

## Milestone 2 — Terminal view (est. 1–2 weeks)

Native (`services/pty_win32.c`):
- [ ] **M2.1** ConPTY session: pipes → `CreatePseudoConsole` →
      `PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE` → `CreateProcessW(EXTENDED_STARTUPINFO_PRESENT)`.
      Close child-side handles post-spawn. Shell from settings; default
      `powershell.exe`, honor `pwsh.exe` if found; cwd = current document dir,
      else project base, else home.
- [ ] **M2.2** Reader thread: blocking `ReadFile` loop → base64 → `g_idle_add` →
      `pty.data`. Writer: `WriteFile` from main thread (input is tiny; move to a
      writer thread only if it ever blocks). Exit watcher: `WaitForSingleObject`
      on `hProcess` (own thread) → `pty.exit`.
- [ ] **M2.3** Teardown order (doc'd deadlock): kill process if alive → keep
      draining output → `ClosePseudoConsole` **not** on the reader thread →
      close pipes. Runs on plugin unload and Geany quit.
- [ ] **M2.4** Flow control: if >N chunks queued for JS (unacked), pause reads;
      `pty.data` acks every M chunks from JS. Test: `type` a 100 MB file, Ctrl+C.

Web (`assets/terminal/`):
- [ ] **M2.5** Vendor `@xterm/xterm` 6.0 + `@xterm/addon-fit` (MIT; note versions
      in README). `windowsPty: {backend:'conpty', buildNumber}` (build number over
      the bridge). `term.onData` → `pty.data`; fit-on-container-resize with 150 ms
      debounce → `pty.resize` → `ResizePseudoConsole`.
- [ ] **M2.6** Exit/restart UX: `pty.exit` → banner + "Restart" button.
- [ ] **M2.7** Keybindings: `plugin_set_key_group` — "Toggle terminal pane",
      "Focus terminal" (shows pane + `wv_host_focus` + `term.focus()`).
      Escape chord (default `Ctrl+Shift+E`) via `term.attachCustomKeyEventHandler`
      → `ui.focusEditor` → `keybindings_send_command(GEANY_KEY_GROUP_FOCUS,
      GEANY_KEYS_FOCUS_EDITOR)`.
- [ ] **M2.8** Acceptance: interactive PowerShell (colors, line editing, Ctrl+C);
      `python` REPL; `git log` pager; `ssh` to something running `htop`; resize
      during output; UTF-8 output (`chcp 65001` note if A-API mojibake appears).

**Exit:** daily-drivable real shell in Geany on Windows — the 2015 geany#675 gap,
closed by a plugin.

## Milestone 3 — Markdown/HTML preview view (est. 3–5 days)

- [ ] **M3.1** `assets/markdown/`: vendored markdown-it, render in-page (no native
      markdown dep — identical output on every platform later).
- [ ] **M3.2** Native push: `document-activate` / `document-save` / `editor-notify`
      (SC_MOD_INSERTTEXT|DELETETEXT) → 300 ms debounce (`plugin_timeout_add`) →
      `doc.text` with full buffer (Geany docs are small; optimize only if needed).
      Only when preview pane visible and filetype ∈ {Markdown, HTML}.
- [ ] **M3.3** HTML filetype: navigate to the file's `file://` URL on save instead
      of bridge push (relative assets work). Markdown: in-page render + preserved
      scroll position.
- [ ] **M3.4** Acceptance: edit README.md live; large (1 MB) md file stays smooth;
      images with relative paths render (base href over bridge).

**Exit:** usable live preview; host API still unchanged.

## Milestone 4 — Hardening + Linux port (est. 1–2 weeks)

- [ ] **M4.1** Keyboard ladder: feature-detect `ICoreWebView2ControllerOptions4`
      → `AllowHostInputProcessing(TRUE)` (Runtime ≥ 138); else
      `add_AcceleratorKeyPressed` forwarding a fixed table (the plugin's own
      chords + user-configured "pass-through" combos) via `g_idle_add` →
      `keybindings_send_command` (async = no re-entrancy).
- [ ] **M4.2** `host_gtk.c`: `WebKitWebView` (webkit2gtk-4.1) implementing
      `wvhost.h` — `evaluate_javascript` / script-message-handler / user-script
      injection; ~200 LOC, markdown-plugin precedent. `pty_posix.c` via `forkpty`.
- [ ] **M4.3** Build matrix: the per-OS branching already lives in `meson.build`
      (M0.1), so this is CI only — GitHub Actions: `msys2/setup-msys2` (mingw64,
      `pacman -S mingw-w64-x86_64-{geany,meson,ninja,gcc}`) job + Ubuntu job
      (`libwebkit2gtk-4.1-dev`, `geany` dev headers, `meson`); both run
      `meson setup build && ninja -C build`.
- [ ] **M4.4** Windows dist: zip of `geanywebview.dll` + assets dir (incl.
      `WebView2Loader.dll`); install instructions for `%APPDATA%/geany/plugins`;
      verify against BOTH pacman-Geany and the `C:\Program Files\Geany`
      self-built installer build.
- [ ] **M4.5** View-pack format doc: `manifest.json` (`id`, `title`, `entry`,
      `placement`, `channels[]`) + loader in `plugin.c`; convert hello/terminal/
      markdown into packs; write "adding a view" README section.

**Exit:** same plugin source builds and runs on Windows and Linux; adding a view =
adding an asset folder + optional service registration.

## Milestone 5 — macOS (deferred, unscheduled)

`host_cocoa.m`: WKWebView as subview of `gdk_quartz_window_get_nsview()`,
manual frame sync (flipped coords), `WKScriptMessageHandler` bridge;
`pty_posix.c` already done. Build via geany-osx jhbuild env. Highest-risk backend;
prototype the NSView embedding before committing to polish. Nothing earlier
blocks on it.

---

## Watch-list (carry into every milestone)

| Item | Trigger | Response |
|------|---------|----------|
| Focus/accelerator loss (webview focused) | Geany shortcuts dead while terminal focused | M4.1 ladder; until then document the escape chord |
| ConPTY close deadlock | hang on unload/quit | M2.3 order is mandatory; test on Win10 too (pre-24H2 semantics) |
| Bridge throughput | terminal freezes on huge output | M2.4 flow control; escape hatch: SharedBuffer / localhost WS (don't build preemptively) |
| DPI / multi-monitor | blurry or mis-sized webview | test in M0.5; `put_Bounds` uses pixel rect from allocation |
| CRT/env mismatch | weird crashes in Program Files Geany | build in the env matching the target Geany; M4.4 verifies both |
| Plugin unload leaks | `msedgewebview2.exe` survivors | M0.7 truth-table re-run each milestone |

## Definition of done (v1 = M0–M4)

1. Reusable host: `wvhost.h` unchanged since M1 while three views shipped.
2. Terminal: real PowerShell via ConPTY in a Geany pane on Windows.
3. Preview: live Markdown/HTML on Windows + Linux.
4. Distribution: zip install into `%APPDATA%/geany/plugins` on a clean Win11
   machine with stock Geany works with zero extra installs (WebView2 preinstalled);
   Win10-without-runtime shows the guided install prompt.
