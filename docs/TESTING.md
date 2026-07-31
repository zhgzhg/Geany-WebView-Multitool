# Manual test checklist

Automated GUI testing of a Geany plugin is limited, so this is run by hand on
a real desktop — once per platform for a release. Launch Geany with **`-v`**
to watch the plugin's `GWV:` trace while testing (on Linux/macOS also
`G_MESSAGES_DEBUG=geany-webview`).

```sh
ninja -C <builddir> devinstall
geany -v
```

## Build & load

- [ ] `meson setup <builddir> && ninja` from a clean tree — no errors, **no
      warnings** (fetches the WebView2 SDK wrap on Windows).
- [ ] `devinstall` places a single module (plus `WebView2Loader.dll` on
      Windows) in the per-user plugin dir — no asset folder.
- [ ] Plugin Manager lists **Geany WebView Multitool (WVM)** with the version
      from `VERSION`; enabling adds the panes; disable → re-enable works.
- [ ] Trace shows `settings loaded: …` with the expected values.

## File Preview (WVM)

- [ ] Markdown renders (GFM: tables, task lists, fenced code); edits
      re-render after the debounce.
- [ ] Relative local images resolve against the document's folder.
- [ ] Section links scroll; external links open in the **OS browser**.
- [ ] A relative link to a **local file** opens it in the editor (the preview
      follows); a link to a missing file reports in the status bar; the
      preview page itself never navigates away or goes stale.
- [ ] Code-block copy button copies to the system clipboard.
- [ ] Dark/light toggle works, persists, and both themes stay readable.
- [ ] HTML mode renders (auto by file type + forced via toolbar/Preferences);
      mode changes persist.

## Terminal (WVM) — both placements

- [ ] Shell spawns in the current document's directory; typing, colors,
      resize (drag the splitter) all work.
- [ ] `exit` + Enter restarts the shell.
- [ ] Bound Geany chords reach the shell while focused (Ctrl+K, Ctrl+W …
      Linux/macOS: via the key snooper); Ctrl+Shift+C/V copy/paste,
      Ctrl+Shift+E focuses the editor.
- [ ] Selection shows a visible highlight; select in the editor →
      middle-click in the terminal pastes (and vice versa); no double-paste.
- [ ] Instances > 1: tab row appears, shells spawn lazily per tab,
      background tab tints amber on output and clears when viewed.
- [ ] Count lowered: highest tabs close, shells end. Count 0: pane disappears.
- [ ] Side pane + **Split Window** plugin: split before/after enabling the
      side terminal — the terminal stays rightmost; unsplit/disable in any
      order restores the layout.

## Web Browser (WVM)

- [ ] Address bar: bare domain → https, `localhost:PORT` → http; links
      navigate in-pane; `target=_blank` stays in-pane.
- [ ] Back/forward/reload/home; Home honors the configured home page and its
      tooltip shows it.
- [ ] **Ctrl+F** (pane focused) opens find-in-page — Edge bar on Windows, the
      plugin's bar on WebKitGTK (highlights, match count, Enter/Shift+Enter,
      Escape closes). **Ctrl+F in the editor still opens Geany's Find.**
- [ ] Preview/terminal panes are still pinned (external links → OS browser).

## Settings & Tools

- [ ] Every Preferences change applies live (panes created/destroyed, counts
      resized, shell used on next start).
- [ ] Config file created with defaults on first run; legacy keys migrate.
- [ ] **Copy File Path (WVM)**: copies the absolute path, greys out on
      unsaved documents, toggling the setting adds/removes it immediately.

## Platform-specific

- [ ] **Windows**: editor copy → paste churn with a preview open (the GTK
      clipboard crash regression, needs GTK3 ≥ 3.24.38).
- [ ] **Linux/NVIDIA**: panes render (auto `WEBKIT_DISABLE_DMABUF_RENDERER`).
- [ ] **macOS**: first pane load may take ~10 s under XQuartz (once per run);
      terminal selection highlight visible (JSC packed-RGBA workaround).
