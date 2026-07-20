/*
 * view.c — generic WebView view lifecycle: creates a notebook page backed by a
 * WvHost + Bridge, and the shared host callbacks / message widgets. Per-view
 * behaviour lives in views/<name>.c.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "view.h"
#include "gwvutil.h"

/* ------------------------------------------------------------ messages ui */

static GtkWidget *make_message_widget(const char *text, gboolean install_link)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	g_object_set(box, "margin", 16, NULL);

	GtkWidget *lbl = gtk_label_new(text);
	gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

	if (install_link) {
		GtkWidget *link = gtk_link_button_new_with_label(
			GWV_WEBVIEW2_URL, _("Install the WebView2 Runtime"));
		gtk_widget_set_halign(link, GTK_ALIGN_START);
		gtk_box_pack_start(GTK_BOX(box), link, FALSE, FALSE, 0);
	}
	return box;
}

void gwv_show_view_message(GwvView *v, const char *text, gboolean install_link)
{
	GtkWidget *msg = make_message_widget(text, install_link);
	gtk_box_pack_start(GTK_BOX(v->panel), msg, FALSE, FALSE, 0);
	gtk_widget_show_all(v->panel);
}

/* -------------------------------------------------------------- host cbs */

static void on_host_ready(WvHost *host, gpointer user)
{
	(void) host; (void) user;
	g_debug("GWV: on_host_ready");
}

static void on_host_message(WvHost *host, const char *json, gpointer user)
{
	(void) host;
	GwvView *v = user;
	if (v->bridge != NULL)
		bridge_handle(v->bridge, json);
}

static void on_host_failed(WvHost *host, const char *error, gpointer user)
{
	(void) host;
	GwvView *v = user;
	g_warning("GWV: on_host_failed: %s", error ? error : "(null)");
	if (v->webarea != NULL)
		gtk_widget_hide(v->webarea);
	gchar *msg = g_strdup_printf(_("WebView failed to start: %s"),
	                             error ? error : _("unknown error"));
	GtkWidget *w = make_message_widget(msg, TRUE);
	gtk_box_pack_start(GTK_BOX(v->panel), w, FALSE, FALSE, 0);
	gtk_widget_show_all(w);
	g_free(msg);
}

static void on_ch_ready(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) user;
	g_debug("GWV: view sys.ready %s", payload);
}

/* Dispatch document-URL changes to the view's optional hook (browser view). */
static void on_host_url_changed(WvHost *host, const char *url, gpointer user)
{
	(void) host;
	GwvView *v = user;
	if (v->on_url_changed != NULL)
		v->on_url_changed(v, url);
}

/* Dispatch find-in-page match counts likewise. */
static void on_host_find_matches(WvHost *host, guint count, gpointer user)
{
	(void) host;
	GwvView *v = user;
	if (v->on_find_matches != NULL)
		v->on_find_matches(v, count);
}

/* Shared: copy text from a view (code-block button, terminal selection) to
 * Geany's own GTK clipboard, so it lands on the same clipboard the editor uses. */
void gwv_on_ch_copy(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) user;
	gchar *text = bridge_payload_string(payload);
	if (text != NULL) {
		GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
		gtk_clipboard_set_text(cb, text, -1);
	}
	g_free(text);
}

/* ------------------------------------------------------------- lifecycle */

GwvView *gwv_view_new_full(GwvState *st, GtkNotebook *notebook, const char *label,
                           const WvHostConfig *cfg, const char *url,
                           gboolean eager, gboolean with_bridge)
{
	GwvView *v = g_new0(GwvView, 1);
	v->plugin = st->plugin;
	v->st = st;

	v->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	if (notebook != NULL)     /* NULL: caller packs the panel elsewhere */
		gtk_notebook_append_page(notebook, v->panel, gtk_label_new(label));
	gtk_widget_show_all(v->panel);

	if (!gwv_os_supported()) {
		gwv_show_view_message(v,
			_("Geany WebView requires Windows 10 version 1809 (build 17763) or newer."),
			FALSE);
		return v;
	}
	char *ver = NULL;
	gboolean avail = wv_host_runtime_available(&ver);
	if (!avail) {
		gchar *msg = g_strdup_printf(
			_("The Microsoft Edge WebView2 Runtime is required but was not found.\n(%s)"),
			ver ? ver : _("not detected"));
		gwv_show_view_message(v, msg, TRUE);
		g_free(msg);
		g_free(ver);
		return v;
	}
	g_free(ver);

	v->webarea = wv_host_new_area();
	gtk_widget_set_hexpand(v->webarea, TRUE);
	gtk_widget_set_vexpand(v->webarea, TRUE);
	gtk_box_pack_start(GTK_BOX(v->panel), v->webarea, TRUE, TRUE, 0);
	gtk_widget_show_all(v->panel);

	WvHostCallbacks cb = { on_host_ready, on_host_message, on_host_failed,
	                       on_host_url_changed, on_host_find_matches };
	v->host = wv_host_new(v->webarea, cfg, &cb, v);

	if (with_bridge) {
		v->bridge = bridge_new(v->host);
		bridge_on(v->bridge, "sys.ready", on_ch_ready, v);
	}

	wv_host_navigate(v->host, url);

	/* Eager views pre-initialize now (instant open); lazy views wait until the
	 * pane is first shown (so a terminal shell isn't spawned until opened). */
	if (eager)
		wv_host_warmup(v->host);

	return v;
}

GwvView *gwv_view_new(GwvState *st, GtkNotebook *notebook,
                      const char *label, const char *view_path, gboolean eager)
{
	WvHostConfig cfg = { GWV_VIRTUAL_HOST, st->bridge_js, FALSE };
	gchar *url = wv_host_format_url(GWV_VIRTUAL_HOST, view_path);
	GwvView *v = gwv_view_new_full(st, notebook, label, &cfg, url, eager, TRUE);
	g_free(url);
	return v;
}

void gwv_view_reveal(GwvView *v, GtkNotebook *notebook)
{
	if (v == NULL)
		return;
	gint num = gtk_notebook_page_num(notebook, v->panel);
	if (num >= 0)
		gtk_notebook_set_current_page(notebook, num);
	/* Bring it up if it was created lazily. */
	if (v->host != NULL) {
		wv_host_warmup(v->host);
		wv_host_focus(v->host);
	}
}

void gwv_view_free(GwvView *v)
{
	if (v == NULL)
		return;
	if (v->ptys != NULL) {           /* slot destroy func frees each PTY */
		g_hash_table_unref(v->ptys);
		v->ptys = NULL;
	}
	if (v->bridge != NULL) {
		bridge_free(v->bridge);
		v->bridge = NULL;
	}
	if (v->host != NULL) {
		wv_host_destroy(v->host);
		v->host = NULL;
	}
	if (v->panel != NULL)
		gtk_widget_destroy(v->panel);
	g_free(v);
}
