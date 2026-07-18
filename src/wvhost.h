/*
 * wvhost.h — the stable, platform-agnostic WebView host interface.
 *
 * One WvHost owns one embedded browser bound to a host GtkWidget. Backends:
 *   - Windows: WebView2  (src/host_win32.cc)
 *   - Linux:   webkit2gtk-4.1  (src/host_gtk.c)   [M4]
 *   - macOS:   WKWebView        (src/host_cocoa.m) [M5]
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
	/* The engine finished async initialization; queued navigate/post calls
	 * have been (or are about to be) flushed. */
	void (*on_ready)   (WvHost *host, gpointer user);
	/* A message arrived from page JS via the bridge (raw JSON string). */
	void (*on_message) (WvHost *host, const char *json, gpointer user);
	/* The engine could not be created (runtime missing, etc.). `error` is a
	 * human-readable reason. The WvHost remains valid but inert. */
	void (*on_failed)  (WvHost *host, const char *error, gpointer user);
} WvHostCallbacks;

/*
 * Create a host bound to `container` (the widget whose area the browser fills).
 * Creation is asynchronous: navigate/set_html/post_message issued before
 * on_ready are queued and flushed once the engine is up. Returns NULL only on
 * hard allocation failure.
 */
WvHost  *wv_host_new          (GtkWidget *container,
                               const WvHostCallbacks *cb, gpointer user);

void     wv_host_navigate     (WvHost *host, const char *url);
void     wv_host_set_html     (WvHost *host, const char *html);

/* Send a JSON string to page JS (delivered to window.bridge.onMessage). */
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
