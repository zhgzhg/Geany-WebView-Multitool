/*
 * wvhost.h — the stable, platform-agnostic WebView host interface.
 *
 * One WvHost owns one embedded browser bound to a host GtkWidget. Backends:
 *   - Windows: WebView2  (src/host/win32.cc)
 *   - Linux + macOS (MacPorts GTK3/X11): webkit2gtk-4.1 (src/host/gtk.c)
 *   - macOS native-quartz GTK builds:    WKWebView (src/host/cocoa.m, future)
 *
 * The plugin core talks only to this header; it never sees a platform API.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_WVHOST_H
#define GEANYWEBVIEW_WVHOST_H

#include <gtk/gtk.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WvHost WvHost;

/* Lifecycle / event callbacks. Any field may be NULL. Always invoked on the
 * GTK main thread. */
typedef struct {
	/* The engine finished async initialization; queued navigate/set_html
	 * calls have been flushed and the injected bridge script is active. */
	void (*on_ready)   (WvHost *host, gpointer user);
	/* A message arrived from page JS via the bridge (raw envelope JSON). */
	void (*on_message) (WvHost *host, const char *json, gpointer user);
	/* The engine could not be created (runtime missing, etc.). `error` is a
	 * human-readable reason. The WvHost remains valid but inert. */
	void (*on_failed)  (WvHost *host, const char *error, gpointer user);
	/* The view's document URL changed (navigation, redirects, history moves).
	 * Useful for browsing views with an address bar; NULL to ignore. */
	void (*on_url_changed)(WvHost *host, const char *url, gpointer user);
	/* Result of wv_host_find(): how many matches the page has. */
	void (*on_find_matches)(WvHost *host, guint count, gpointer user);
} WvHostCallbacks;

/* Static configuration applied once when the engine comes up. All fields are
 * copied by wv_host_new(); any may be NULL. */
typedef struct {
	/* Host name the embedded assets (see assets.h) are served under, so views
	 * load as <scheme>://<virtual_host>/<view>/index.html. Documents published
	 * with wv_host_put_virtual() appear under the same host; additional hosts
	 * mapped with wv_host_map_dir() serve local folders. */
	const char *virtual_host;   /* e.g. "geanyview.local" */
	/* JavaScript injected into every document *before* its own scripts run
	 * (the bridge shim). NULL for none. */
	const char *inject_js;
	/* FALSE (the default): the view is pinned to the virtual host and external
	 * links open in the OS browser. TRUE: free web browsing — any http(s)
	 * navigation stays in the view, and new-window requests (target=_blank)
	 * navigate the same view. */
	gboolean allow_browsing;
} WvHostConfig;

/*
 * Create the widget the browser will fill, of whatever GtkWidget type this
 * backend needs (win32: a GtkDrawingArea providing a native HWND to parent
 * WebView2 onto; gtk: a container the WebKitWebView is packed into). Pack it,
 * then pass it to wv_host_new() as `container`.
 */
GtkWidget *wv_host_new_area   (void);

/*
 * Create a host bound to `container` (the widget whose area the browser fills,
 * from wv_host_new_area). Creation is asynchronous: navigate/set_html issued
 * before on_ready are queued and flushed once the engine is up. `config` may be
 * NULL. Returns NULL only on hard allocation failure.
 */
WvHost  *wv_host_new          (GtkWidget *container, const WvHostConfig *config,
                               const WvHostCallbacks *cb, gpointer user);

/*
 * Platform URL for `path` under a mapped virtual host: the win32 backend serves
 * mapped folders at https://<host>/, the gtk backend at geanyview://<host>/.
 * Callers build view URLs with this instead of hardcoding a scheme. Caller
 * g_free()s.
 */
char    *wv_host_format_url   (const char *host_name, const char *path);

void     wv_host_navigate     (WvHost *host, const char *url);
void     wv_host_set_html     (WvHost *host, const char *html);

/*
 * Bring the engine up now instead of waiting for the pane to be shown (eager
 * views, and reveal). Lazy views skip this; their engine initializes when the
 * container is first shown.
 */
void     wv_host_warmup       (WvHost *host);

/*
 * Map (or re-map) an additional virtual host name to a local folder, so the page
 * can load that folder's files (e.g. images next to the current document) as
 * https://<host_name>/... Pass folder=NULL/"" to remove the mapping. No-op until
 * the engine is ready.
 */
void     wv_host_map_dir      (WvHost *host, const char *host_name, const char *folder);

/*
 * Publish an in-memory document at <virtual_host>/<path> (query strings are
 * ignored when serving). `data` is copied (`len` = -1 for NUL-terminated);
 * NULL removes the entry. Replaces any previous content at that path.
 */
void     wv_host_put_virtual  (WvHost *host, const char *path,
                               const char *data, gssize len, const char *mime);

/* Send an envelope JSON string to page JS (delivered to the bridge shim). */
void     wv_host_post_message (WvHost *host, const char *json);

void     wv_host_focus        (WvHost *host);
void     wv_host_set_visible  (WvHost *host, gboolean visible);

/* History navigation (browsing views). No-ops when there is nowhere to go. */
void     wv_host_go_back      (WvHost *host);
void     wv_host_go_forward   (WvHost *host);
void     wv_host_reload       (WvHost *host);

/*
 * Find-in-page (browsing views). The WebView2 backend ships the browser's own
 * find bar (Ctrl+F is a browser accelerator there), so it needs no UI from the
 * plugin: wv_host_needs_find_ui() returns FALSE and the calls are no-ops.
 * WebKitGTK exposes only the search API — the caller provides the bar.
 */
gboolean wv_host_needs_find_ui(void);
/* Start/restart an incremental case-insensitive wrap-around search; reports
 * the match count via on_find_matches. NULL/"" stops the search. */
void     wv_host_find         (WvHost *host, const char *text);
void     wv_host_find_next    (WvHost *host, gboolean forward);
void     wv_host_find_stop    (WvHost *host);

/* Tear down the engine and free the host. Safe to pass NULL. */
void     wv_host_destroy      (WvHost *host);

/*
 * TRUE if the platform webview engine is usable right now. On return, if
 * `version_out` is non-NULL, *version_out is a newly allocated string: the
 * engine version on success, or a human-readable reason on failure. Caller
 * g_free()s it.
 */
gboolean wv_host_runtime_available (char **version_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GEANYWEBVIEW_WVHOST_H */
