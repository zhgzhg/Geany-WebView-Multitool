# Manual test checklist

Automated GUI testing of a Geany plugin is limited, so these are run by hand on
a real desktop. Launch Geany with **`-v`** to see the plugin's `GWV:` trace
(Geany installs its own log handler, so `-v` — not `G_MESSAGES_DEBUG` — is what
surfaces the messages).

```sh
ninja -C build devinstall     # install DLL + WebView2Loader.dll + assets
/mingw64/bin/geany -v
```

## Build

- [ ] `meson setup build && ninja -C build` from a clean tree (fetches the
      WebView2 SDK wrap) — no errors, no warnings.
- [ ] `ninja -C build devinstall` places `geanywebview.dll`,
      `WebView2Loader.dll`, and `geanywebview/` (assets) under
      `%APPDATA%/geany/plugins`.

## Load / first view

- [ ] Plugin Manager lists **Geany WebView**; enabling it adds a **WebView** tab
      to the sidebar.
- [ ] The tab shows the hello page (dark), status line **"bridge: round-trip
      OK"** — proving asset serving + injected bridge + JS↔native round-trip.
- [ ] Clicking **Ping native** keeps the status green (native replies pong).
- [ ] Trace shows: `vhost … hr=0x00000000`, `navigate https://geanyview.local/…`,
      `on_host_ready`, `view sys.ready`, `sys.ping -> sys.pong`.

## Layout

- [ ] Drag the sidebar splitter — the web content resizes to match (no gap /
      clipping).
- [ ] Switch to another sidebar tab and back — content hides then reappears.
- [ ] Minimize / restore Geany; move the window between monitors of different
      DPI — content stays correctly sized.

## Runtime / OS gates

- [ ] On a machine without the WebView2 Runtime (or temporarily rename
      `WebView2Loader.dll`), the pane shows the "runtime required" message with a
      working **Install the WebView2 Runtime** link instead of a blank webview.
- [ ] (If testable) On Windows older than 10 1809, the pane shows the OS-floor
      message and no webview is created.

## Teardown (no leaks)

- [ ] **Hot unload:** with the pane open, disable the plugin in Plugin Manager.
      The tab disappears; the trace shows `wv_host_destroy`; Task Manager shows
      **no leftover `msedgewebview2.exe`** for this app after a few seconds.
- [ ] **Quit with pane open:** close Geany normally — clean exit, no crash, no
      leftover `msedgewebview2.exe`.
- [ ] Re-enable after a hot unload — the view comes back (retry-on-BUSY covers
      the brief user-data-folder lock if the old browser process is still exiting).

## Notes

- The user-data folder is under
  `%LOCALAPPDATA%\Microsoft\Windows\INetCache\geany-webview`.
- `msedgewebview2.exe` processes are children of Geany; a normal quit or hot
  unload should reap them. Orphans only linger after a hard kill (`taskkill /F`).
