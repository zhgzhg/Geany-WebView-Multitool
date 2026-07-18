/*
 * plugin.c — Geany WebView: a reusable WebView host pane for Geany 2.x.
 *
 * M0 skeleton: registers with the modern GeanyPlugin API, adds a "WebView"
 * page to the sidebar notebook hosting a WvHost, and a Tools-menu item to
 * reveal it. The host backend (WebView2 on Windows) renders into the pane's
 * drawing area; until the WebView2 SDK is wired in it uses a stub backend.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <string.h>

#include <geanyplugin.h>

#include "wvhost.h"

/* M0.6 bridge round-trip probe: on load the page pings native; native replies
 * with a pong; the page then posts sys.roundtrip, proving both directions. */
static const char *BRIDGE_TEST_HTML =
	"<!doctype html><html><head><meta charset='utf-8'>"
	"<meta http-equiv='Content-Security-Policy' content=\"default-src 'self'; "
	"style-src 'unsafe-inline'; script-src 'unsafe-inline'\">"
	"<style>body{font-family:system-ui,sans-serif;background:#1e1e22;color:#dcdce2;"
	"margin:0;padding:1rem}h1{font-size:1.05rem}#s{color:#7c7}</style></head>"
	"<body><h1>Geany WebView</h1><p id='s'>bridge: init\xe2\x80\xa6</p>"
	"<script>"
	"window.bridge={_h:{},post:function(c,p){if(window.chrome&&chrome.webview)"
	"chrome.webview.postMessage(JSON.stringify({ch:c,p:p}));},"
	"on:function(c,f){this._h[c]=f;}};"
	"if(window.chrome&&chrome.webview){chrome.webview.addEventListener('message',"
	"function(e){try{var m=JSON.parse(e.data);if(bridge._h[m.ch])bridge._h[m.ch](m.p);}"
	"catch(x){}});}"
	"bridge.on('sys.pong',function(p){document.getElementById('s').textContent="
	"'bridge: round-trip OK';bridge.post('sys.roundtrip',{ok:true});});"
	"window.addEventListener('load',function(){bridge.post('sys.ping',{t:1});});"
	"</script></body></html>";

/* Per-instance plugin state, stored via geany_plugin_set_data(). */
typedef struct {
	GeanyPlugin *plugin;
	GtkWidget   *panel;      /* sidebar notebook page (a GtkBox)            */
	GtkWidget   *webarea;    /* GtkDrawingArea the browser is parented onto */
	GtkWidget   *menu_item;  /* Tools-menu entry                            */
	WvHost      *host;
} GwvState;

/* ------------------------------------------------------------------ host cbs */

static void on_host_ready(WvHost *host, gpointer user)
{
	GwvState *st = user;
	(void) host;
	g_debug("GWV: on_host_ready");
	ui_set_statusbar(FALSE, _("Geany WebView: engine ready."));
	(void) st;
}

static void on_host_message(WvHost *host, const char *json, gpointer user)
{
	GwvState *st = user;
	(void) host;
	g_debug("GWV: on_message: %s", json ? json : "(null)");
	/* Bridge round-trip probe: reply to the page's ping. */
	if (json != NULL && strstr(json, "sys.ping") != NULL)
		wv_host_post_message(st->host, "{\"ch\":\"sys.pong\",\"p\":{\"ok\":true}}");
	ui_set_statusbar(FALSE, _("Geany WebView message: %s"), json ? json : "(null)");
}

static void on_host_failed(WvHost *host, const char *error, gpointer user)
{
	(void) host; (void) user;
	g_debug("GWV: on_host_failed: %s", error ? error : "(null)");
	ui_set_statusbar(TRUE, _("Geany WebView: %s"), error ? error : _("engine unavailable"));
}

/* --------------------------------------------------------------------- ui */

/* Reveal and focus the WebView sidebar page. */
static void reveal_pane(GwvState *st)
{
	GtkNotebook *sb = GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook);
	gint num = gtk_notebook_page_num(sb, st->panel);
	if (num >= 0)
		gtk_notebook_set_current_page(sb, num);
	if (st->host)
		wv_host_focus(st->host);
}

static void on_menu_activate(GtkMenuItem *item, gpointer user)
{
	(void) item;
	reveal_pane(user);
}

/* ------------------------------------------------------------- plugin funcs */

static gboolean gwv_init(GeanyPlugin *plugin, gpointer pdata)
{
	(void) pdata;
	GeanyData *geany_data = plugin->geany_data;
	GwvState  *st = g_new0(GwvState, 1);
	st->plugin = plugin;

	/* Tools menu entry. */
	st->menu_item = gtk_menu_item_new_with_mnemonic(_("_Geany WebView"));
	gtk_widget_show(st->menu_item);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_item);
	g_signal_connect(st->menu_item, "activate", G_CALLBACK(on_menu_activate), st);

	/* Sidebar page: a vertical box with a drawing area that fills it. The
	 * drawing area is what the native browser HWND is parented onto. */
	st->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	st->webarea = gtk_drawing_area_new();
	gtk_widget_set_hexpand(st->webarea, TRUE);
	gtk_widget_set_vexpand(st->webarea, TRUE);
	gtk_box_pack_start(GTK_BOX(st->panel), st->webarea, TRUE, TRUE, 0);
	gtk_widget_show_all(st->panel);

	GtkNotebook *sb = GTK_NOTEBOOK(geany_data->main_widgets->sidebar_notebook);
	gtk_notebook_append_page(sb, st->panel, gtk_label_new(_("WebView")));

	/* Bind the host to the drawing area and load the bridge probe page
	 * (queued until the engine is ready). */
	WvHostCallbacks cb = { on_host_ready, on_host_message, on_host_failed };
	st->host = wv_host_new(st->webarea, &cb, st);
	wv_host_set_html(st->host, BRIDGE_TEST_HTML);

	geany_plugin_set_data(plugin, st, NULL);
	return TRUE;
}

static void gwv_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	if (st == NULL)
		return;

	/* Destroy the engine before the widget it lives in. */
	if (st->host != NULL) {
		wv_host_destroy(st->host);
		st->host = NULL;
	}
	if (st->panel != NULL)
		gtk_widget_destroy(st->panel);      /* also removes it from the notebook */
	if (st->menu_item != NULL)
		gtk_widget_destroy(st->menu_item);

	g_free(st);
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Geany WebView");
	plugin->info->description =
		_("Reusable WebView host pane (terminal, Markdown/HTML preview, …).");
	plugin->info->version = "0.0.1";
	plugin->info->author = "Geany WebView contributors";

	plugin->funcs->init      = gwv_init;
	plugin->funcs->cleanup   = gwv_cleanup;
	plugin->funcs->configure = NULL;
	plugin->funcs->help      = NULL;
	plugin->funcs->callbacks = NULL;

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
