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
} WvHostCallbacks;

/* Static configuration applied once when the engine comes up. All fields are
 * copied by wv_host_new(); any may be NULL. */
typedef struct {
	/* Serve `asset_root` at https://<virtual_host>/ so views load as
	 * https://<virtual_host>/<view>/index.html. Both NULL disables mapping. */
	const char *virtual_host;   /* e.g. "geanyview.local" */
	const char *asset_root;     /* local folder to serve */
	/* JavaScript injected into every document *before* its own scripts run
	 * (the bridge shim). NULL for none. */
	const char *inject_js;
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

/* Send an envelope JSON string to page JS (delivered to the bridge shim). */
void     wv_host_post_message (WvHost *host, const char *json);

void     wv_host_focus        (WvHost *host);
void     wv_host_set_visible  (WvHost *host, gboolean visible);

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
