/*
 * host/gtk.c — Linux WebView host backend on WebKitGTK (webkit2gtk-4.1),
 * implementing the C ABI declared in host/wvhost.h.
 *
 * The virtual-host mapping is implemented as a custom URI scheme
 * (geanyview://<host>/<path>) registered on a per-host WebKitWebContext and
 * served from the mapped local folders; the scheme is marked secure +
 * CORS-enabled so pages behave as they do on the https:// virtual host of the
 * WebView2 backend. The bridge shim is injected as a document-start user
 * script; page->native flows through a script message handler, native->page
 * through evaluate_javascript() calling window.__bridgeRecv().
 *
 * The first load is deferred until the container is first mapped (actually
 * shown) or wv_host_warmup() is called, which preserves the lazy-view
 * semantics: a terminal shell only spawns when its pane is first shown.
 * (Realize is too early here — GTK3 realizes hidden notebook children at
 * startup, unlike map.)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "host/wvhost.h"

#include <webkit2/webkit2.h>
#include <string.h>

#define GWV_SCHEME "geanyview"

struct WvHost {
	GtkWidget            *container;   /* box from wv_host_new_area()          */
	WebKitWebView        *webview;
	WebKitWebContext     *ctx;
	WebKitUserContentManager *ucm;
	WvHostCallbacks       cb;
	gpointer              user;
	GHashTable           *mounts;      /* virtual host name -> local folder    */
	gchar                *virtual_host;
	gchar                *pending_url;  /* queued until first map / warmup     */
	gchar                *pending_html;
	gboolean              flushed;      /* first load has been issued          */
	gulong                map_id;
	guint                 ready_idle;
};

/* --------------------------- scheme serving ----------------------------- */

/* Serve geanyview://<host>/<path> from the folder mapped for <host>. */
static void on_scheme_request(WebKitURISchemeRequest *req, gpointer user)
{
	WvHost *h = user;
	const gchar *uri = webkit_uri_scheme_request_get_uri(req);

	GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
	if (u == NULL) {
		GError *err = g_error_new(G_FILE_ERROR, G_FILE_ERROR_INVAL, "bad URI");
		webkit_uri_scheme_request_finish_error(req, err);
		g_error_free(err);
		return;
	}
	const gchar *host = g_uri_get_host(u);
	const gchar *path = g_uri_get_path(u);          /* decoded, "/preview/…" */
	const gchar *folder = (host != NULL) ? g_hash_table_lookup(h->mounts, host) : NULL;

	gchar *full = NULL, *canon = NULL, *canon_root = NULL, *data = NULL;
	gsize  len = 0;
	GError *err = NULL;

	if (folder == NULL || path == NULL || *path == '\0') {
		err = g_error_new(G_FILE_ERROR, G_FILE_ERROR_NOENT,
		                  "no folder mapped for host '%s'", host ? host : "");
		goto fail;
	}

	/* Resolve within the mapped folder only (no ../ escapes). */
	full = g_build_filename(folder, path + 1, NULL);
	canon = g_canonicalize_filename(full, NULL);
	canon_root = g_canonicalize_filename(folder, NULL);
	if (!g_str_has_prefix(canon, canon_root)) {
		err = g_error_new(G_FILE_ERROR, G_FILE_ERROR_ACCES, "path escapes mapping");
		goto fail;
	}
	if (!g_file_get_contents(canon, &data, &len, &err))
		goto fail;

	{
		GInputStream *stream = g_memory_input_stream_new_from_data(data, (gssize) len, g_free);
		gchar *ctype = g_content_type_guess(canon, (const guchar *) data,
		                                    MIN(len, (gsize) 4096), NULL);
		gchar *mime = g_content_type_get_mime_type(ctype);
		webkit_uri_scheme_request_finish(req, stream, (gint64) len,
		                                 mime != NULL ? mime : "application/octet-stream");
		g_object_unref(stream);
		g_free(ctype);
		g_free(mime);
	}
	goto out;

fail:
	webkit_uri_scheme_request_finish_error(req, err);
	g_error_free(err);
out:
	g_free(full);
	g_free(canon);
	g_free(canon_root);
	g_uri_unref(u);
}

/* ----------------------------- bridge glue ------------------------------ */

/* page -> native: bridge.js posts envelope strings on the "bridge" handler. */
static void on_script_message(WebKitUserContentManager *ucm,
                              WebKitJavascriptResult *jsr, gpointer user)
{
	(void) ucm;
	WvHost *h = user;
	JSCValue *v = webkit_javascript_result_get_js_value(jsr);
	if (v != NULL && jsc_value_is_string(v)) {
		gchar *s = jsc_value_to_string(v);
		if (s != NULL && h->cb.on_message != NULL)
			h->cb.on_message(h, s, h->user);
		g_free(s);
	}
}

/* Encode text as a JS double-quoted string literal (UTF-8 passes through). */
static gchar *js_string_literal(const char *s)
{
	GString *out = g_string_new("\"");
	for (const char *p = s; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(out, "\\\""); break;
		case '\\': g_string_append(out, "\\\\"); break;
		case '\n': g_string_append(out, "\\n");  break;
		case '\r': g_string_append(out, "\\r");  break;
		case '\t': g_string_append(out, "\\t");  break;
		default:
			if ((guchar) *p < 0x20)
				g_string_append_printf(out, "\\u%04x", (guint) (guchar) *p);
			else
				g_string_append_c(out, *p);
		}
	}
	g_string_append_c(out, '"');
	return g_string_free(out, FALSE);
}

/* ------------------------------ navigation ------------------------------ */

/* Keep the pane on the virtual host; open external links in the OS browser. */
static gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                                 WebKitPolicyDecisionType type, gpointer user)
{
	(void) view; (void) user;
	if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
	    type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION)
		return FALSE;

	WebKitNavigationAction *act = webkit_navigation_policy_decision_get_navigation_action(
		WEBKIT_NAVIGATION_POLICY_DECISION(decision));
	const gchar *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(act));
	gboolean internal = (uri != NULL) &&
		(g_str_has_prefix(uri, GWV_SCHEME "://") ||
		 g_str_has_prefix(uri, "about:") ||
		 g_str_has_prefix(uri, "data:")  ||
		 g_str_has_prefix(uri, "blob:"));

	if (internal && type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION)
		return FALSE;                      /* default: allow */

	webkit_policy_decision_ignore(decision);
	if (!internal && uri != NULL) {
		g_debug("GWV: external link -> OS browser: %s", uri);
		gtk_show_uri_on_window(NULL, uri, GDK_CURRENT_TIME, NULL);
	}
	return TRUE;
}

/* ------------------------------- lifecycle ------------------------------ */

static void host_flush_pending(WvHost *h)
{
	if (h->webview == NULL)
		return;
	h->flushed = TRUE;
	if (h->pending_url != NULL) {
		g_debug("GWV: navigate %s", h->pending_url);
		webkit_web_view_load_uri(h->webview, h->pending_url);
		g_clear_pointer(&h->pending_url, g_free);
	} else if (h->pending_html != NULL) {
		gchar *base = wv_host_format_url(h->virtual_host != NULL ? h->virtual_host : "local", "");
		webkit_web_view_load_html(h->webview, h->pending_html, base);
		g_free(base);
		g_clear_pointer(&h->pending_html, g_free);
	}
}

static void on_container_map(GtkWidget *widget, gpointer user)
{
	(void) widget;
	WvHost *h = user;
	if (!h->flushed)
		host_flush_pending(h);
}

static gboolean fire_ready(gpointer user)
{
	WvHost *h = user;
	h->ready_idle = 0;
	if (h->cb.on_ready != NULL)
		h->cb.on_ready(h, h->user);
	return G_SOURCE_REMOVE;
}

/* ------------------------------- C ABI ---------------------------------- */

/* On the NVIDIA proprietary driver, WebKit's DMA-BUF renderer often fails to
 * allocate GBM buffers ("Failed to create GBM buffer …: Invalid argument"),
 * leaving the views blank. Fall back to shared-memory rendering there. Must
 * run before the first web process spawns; a user-exported value wins. */
static void host_apply_gpu_workarounds(void)
{
	static gboolean done = FALSE;
	if (done)
		return;
	done = TRUE;
	if (g_file_test("/sys/module/nvidia", G_FILE_TEST_IS_DIR)) {
		g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", FALSE);
		g_debug("GWV: NVIDIA driver detected -> WEBKIT_DISABLE_DMABUF_RENDERER=%s",
		        g_getenv("WEBKIT_DISABLE_DMABUF_RENDERER"));
	}
}

/* The WebKit view is a regular GtkWidget, so the area is just a container to
 * pack it into. */
GtkWidget *wv_host_new_area(void)
{
	return gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
}

/* Mapped folders are served by the custom scheme at geanyview://<host>/. */
char *wv_host_format_url(const char *host_name, const char *path)
{
	return g_strdup_printf(GWV_SCHEME "://%s/%s", host_name, path != NULL ? path : "");
}

WvHost *wv_host_new(GtkWidget *container, const WvHostConfig *config,
                    const WvHostCallbacks *cb, gpointer user)
{
	host_apply_gpu_workarounds();

	WvHost *h = g_new0(WvHost, 1);
	h->container = container;
	if (cb != NULL)
		h->cb = *cb;
	h->user = user;
	h->mounts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

	/* A private context per host: the scheme handler carries this host's
	 * mounts, and a fresh context sidesteps re-registration on plugin reload.
	 * Ephemeral: the views persist nothing (settings live natively), and
	 * multiple contexts sharing the default on-disk storage race over its
	 * sqlite databases (libsoup "database is locked" warnings). */
	h->ctx = webkit_web_context_new_ephemeral();
	webkit_web_context_register_uri_scheme(h->ctx, GWV_SCHEME,
	                                       on_scheme_request, h, NULL);
	WebKitSecurityManager *sm = webkit_web_context_get_security_manager(h->ctx);
	webkit_security_manager_register_uri_scheme_as_secure(sm, GWV_SCHEME);
	webkit_security_manager_register_uri_scheme_as_cors_enabled(sm, GWV_SCHEME);

	if (config != NULL && config->virtual_host != NULL) {
		h->virtual_host = g_strdup(config->virtual_host);
		if (config->asset_root != NULL)
			g_hash_table_replace(h->mounts, g_strdup(config->virtual_host),
			                     g_strdup(config->asset_root));
	}

	h->ucm = webkit_user_content_manager_new();
	webkit_user_content_manager_register_script_message_handler(h->ucm, "bridge");
	g_signal_connect(h->ucm, "script-message-received::bridge",
	                 G_CALLBACK(on_script_message), h);
	if (config != NULL && config->inject_js != NULL) {
		WebKitUserScript *script = webkit_user_script_new(config->inject_js,
			WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
			WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
		webkit_user_content_manager_add_script(h->ucm, script);
		webkit_user_script_unref(script);
	}

	WebKitSettings *settings = webkit_settings_new();
	webkit_settings_set_enable_developer_extras(settings, TRUE);
	/* Diagnostic: GWV_WEBKIT_CONSOLE=1 mirrors page console output (incl. JS
	 * errors) to stdout — invaluable when a view misbehaves headlessly. */
	if (g_getenv("GWV_WEBKIT_CONSOLE") != NULL)
		webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);

	h->webview = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
		"web-context", h->ctx,
		"user-content-manager", h->ucm,
		"settings", settings,
		NULL));
	g_object_unref(settings);

	g_signal_connect(h->webview, "decide-policy", G_CALLBACK(on_decide_policy), h);

	gtk_box_pack_start(GTK_BOX(container), GTK_WIDGET(h->webview), TRUE, TRUE, 0);
	gtk_widget_show(GTK_WIDGET(h->webview));

	/* Defer the first load until the container is first shown (lazy views);
	 * eager views call wv_host_warmup() right after creation. */
	h->map_id = g_signal_connect(container, "map",
	                             G_CALLBACK(on_container_map), h);

	h->ready_idle = g_idle_add(fire_ready, h);
	return h;
}

void wv_host_warmup(WvHost *h)
{
	if (h != NULL && !h->flushed)
		host_flush_pending(h);
}

void wv_host_navigate(WvHost *h, const char *url)
{
	if (h == NULL || url == NULL)
		return;
	if (h->flushed || gtk_widget_get_mapped(h->container)) {
		h->flushed = TRUE;
		g_debug("GWV: navigate %s", url);
		webkit_web_view_load_uri(h->webview, url);
	} else {
		g_free(h->pending_url);
		h->pending_url = g_strdup(url);
	}
}

void wv_host_set_html(WvHost *h, const char *html)
{
	if (h == NULL || html == NULL)
		return;
	if (h->flushed || gtk_widget_get_mapped(h->container)) {
		h->flushed = TRUE;
		gchar *base = wv_host_format_url(h->virtual_host != NULL ? h->virtual_host : "local", "");
		webkit_web_view_load_html(h->webview, html, base);
		g_free(base);
	} else {
		g_free(h->pending_html);
		h->pending_html = g_strdup(html);
	}
}

void wv_host_post_message(WvHost *h, const char *json)
{
	if (h == NULL || h->webview == NULL || json == NULL)
		return;
	gchar *lit = js_string_literal(json);
	gchar *script = g_strconcat("window.__bridgeRecv(", lit, ");", NULL);
	webkit_web_view_evaluate_javascript(h->webview, script, -1,
	                                    NULL, NULL, NULL, NULL, NULL);
	g_free(script);
	g_free(lit);
}

void wv_host_map_dir(WvHost *h, const char *host_name, const char *folder)
{
	if (h == NULL || host_name == NULL)
		return;
	if (folder != NULL && *folder != '\0')
		g_hash_table_replace(h->mounts, g_strdup(host_name), g_strdup(folder));
	else
		g_hash_table_remove(h->mounts, host_name);
}

void wv_host_focus(WvHost *h)
{
	if (h != NULL && h->webview != NULL)
		gtk_widget_grab_focus(GTK_WIDGET(h->webview));
}

void wv_host_set_visible(WvHost *h, gboolean visible)
{
	if (h != NULL && h->webview != NULL)
		gtk_widget_set_visible(GTK_WIDGET(h->webview), visible);
}

void wv_host_destroy(WvHost *h)
{
	if (h == NULL)
		return;
	if (h->ready_idle != 0)
		g_source_remove(h->ready_idle);
	if (h->map_id != 0 && h->container != NULL)
		g_signal_handler_disconnect(h->container, h->map_id);
	if (h->ucm != NULL) {
		g_signal_handlers_disconnect_by_data(h->ucm, h);
		webkit_user_content_manager_unregister_script_message_handler(h->ucm, "bridge");
	}
	if (h->webview != NULL) {
		g_signal_handlers_disconnect_by_data(h->webview, h);
		gtk_widget_destroy(GTK_WIDGET(h->webview));
	}
	g_clear_object(&h->ucm);
	g_clear_object(&h->ctx);
	g_clear_pointer(&h->mounts, g_hash_table_unref);
	g_free(h->virtual_host);
	g_free(h->pending_url);
	g_free(h->pending_html);
	g_free(h);
}

gboolean wv_host_runtime_available(char **version_out)
{
	if (version_out != NULL)
		*version_out = g_strdup_printf("WebKitGTK %u.%u.%u",
		                               webkit_get_major_version(),
		                               webkit_get_minor_version(),
		                               webkit_get_micro_version());
	return TRUE;
}
