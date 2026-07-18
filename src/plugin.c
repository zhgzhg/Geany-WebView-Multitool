/*
 * plugin.c — Geany WebView: a reusable WebView host pane for Geany 2.x.
 *
 * Registers with the modern GeanyPlugin API, adds a "WebView" page to the
 * sidebar notebook, and hosts a WvHost that serves the bundled `assets/` folder
 * over a virtual host with an injected JS bridge. Views talk only to
 * window.bridge; native handlers are registered per channel (src/bridge.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <geanyplugin.h>

#include "wvhost.h"
#include "bridge.h"
#include "util.h"

#define GWV_VIRTUAL_HOST "geanyview.local"
#define GWV_ASSET_SUBDIR "geanywebview"
#define GWV_WEBVIEW2_URL "https://developer.microsoft.com/microsoft-edge/webview2/"

/* Per-instance plugin state, stored via geany_plugin_set_data(). */
typedef struct {
	GeanyPlugin *plugin;
	GtkWidget   *panel;      /* sidebar notebook page (a GtkBox)            */
	GtkWidget   *webarea;    /* GtkDrawingArea the browser is parented onto */
	GtkWidget   *menu_item;  /* Tools-menu entry                            */
	WvHost      *host;
	Bridge      *bridge;
} GwvState;

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

static void show_pane_message(GwvState *st, const char *text, gboolean install_link)
{
	GtkWidget *msg = make_message_widget(text, install_link);
	gtk_box_pack_start(GTK_BOX(st->panel), msg, FALSE, FALSE, 0);
	gtk_widget_show_all(st->panel);
}

/* ------------------------------------------------------------- bridge cbs */

static void on_ch_ready(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) payload;
	(void) user;
	g_debug("GWV: view sys.ready %s", payload);
	ui_set_statusbar(FALSE, _("Geany WebView: view ready."));
}

static void on_ch_ping(Bridge *bridge, const char *payload, gpointer user)
{
	(void) payload; (void) user;
	g_debug("GWV: sys.ping -> sys.pong");
	bridge_post(bridge, "sys.pong", "{\"v\":\"native\"}");
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
	GwvState *st = user;
	if (st->bridge != NULL)
		bridge_handle(st->bridge, json);
}

static void on_host_failed(WvHost *host, const char *error, gpointer user)
{
	(void) host;
	GwvState *st = user;
	g_warning("GWV: on_host_failed: %s", error ? error : "(null)");
	/* Don't tear down inside the host callback; just surface the failure. */
	if (st->webarea != NULL)
		gtk_widget_hide(st->webarea);
	gchar *msg = g_strdup_printf(_("WebView failed to start: %s"),
	                             error ? error : _("unknown error"));
	GtkWidget *w = make_message_widget(msg, TRUE);
	gtk_box_pack_start(GTK_BOX(st->panel), w, FALSE, FALSE, 0);
	gtk_widget_show_all(w);
	g_free(msg);
}

/* --------------------------------------------------------------------- ui */

static void reveal_pane(GwvState *st)
{
	GtkNotebook *sb = GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook);
	gint num = gtk_notebook_page_num(sb, st->panel);
	if (num >= 0)
		gtk_notebook_set_current_page(sb, num);
	if (st->host != NULL)
		wv_host_focus(st->host);
}

static void on_menu_activate(GtkMenuItem *item, gpointer user)
{
	(void) item;
	reveal_pane(user);
}

/* ------------------------------------------------------------ webview setup */

static void create_webview(GwvState *st)
{
	st->webarea = gtk_drawing_area_new();
	gtk_widget_set_hexpand(st->webarea, TRUE);
	gtk_widget_set_vexpand(st->webarea, TRUE);
	gtk_box_pack_start(GTK_BOX(st->panel), st->webarea, TRUE, TRUE, 0);
	gtk_widget_show_all(st->panel);

	gchar *dir        = gwv_plugin_dir();
	gchar *asset_root = g_build_filename(dir, GWV_ASSET_SUBDIR, NULL);
	gchar *bridge_path = g_build_filename(asset_root, "bridge.js", NULL);
	gchar *bridge_js  = NULL;
	if (!g_file_get_contents(bridge_path, &bridge_js, NULL, NULL))
		g_warning("GWV: could not read %s", bridge_path);

	WvHostConfig    cfg = { GWV_VIRTUAL_HOST, asset_root, bridge_js };
	WvHostCallbacks cb  = { on_host_ready, on_host_message, on_host_failed };
	st->host = wv_host_new(st->webarea, &cfg, &cb, st);

	st->bridge = bridge_new(st->host);
	bridge_on(st->bridge, "sys.ready", on_ch_ready, st);
	bridge_on(st->bridge, "sys.ping",  on_ch_ping,  st);

	wv_host_navigate(st->host, "https://" GWV_VIRTUAL_HOST "/hello/index.html");

	/* Pre-initialize the engine now (rather than lazily on first tab reveal) so
	 * the pane opens instantly and startup is deterministic. */
	gtk_widget_realize(st->webarea);

	g_free(bridge_js);
	g_free(bridge_path);
	g_free(asset_root);
	g_free(dir);
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

	/* Sidebar page. */
	st->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	GtkNotebook *sb = GTK_NOTEBOOK(geany_data->main_widgets->sidebar_notebook);
	gtk_notebook_append_page(sb, st->panel, gtk_label_new(_("WebView")));
	gtk_widget_show_all(st->panel);

	/* Gate on OS floor, then WebView2 runtime availability. */
	if (!gwv_os_supported()) {
		show_pane_message(st,
			_("Geany WebView requires Windows 10 version 1809 (build 17763) or newer."),
			FALSE);
	} else {
		char *ver = NULL;
		gboolean avail = wv_host_runtime_available(&ver);
		g_debug("GWV: WebView2 runtime available=%d ver=%s", avail, ver ? ver : "(null)");
		if (!avail) {
			gchar *msg = g_strdup_printf(
				_("The Microsoft Edge WebView2 Runtime is required but was not found.\n(%s)"),
				ver ? ver : _("not detected"));
			show_pane_message(st, msg, TRUE);
			g_free(msg);
		} else {
			create_webview(st);
		}
		g_free(ver);
	}

	geany_plugin_set_data(plugin, st, NULL);
	return TRUE;
}

static void gwv_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	if (st == NULL)
		return;

	if (st->bridge != NULL) {
		bridge_free(st->bridge);
		st->bridge = NULL;
	}
	if (st->host != NULL) {
		wv_host_destroy(st->host);     /* closes the engine before its widget */
		st->host = NULL;
	}
	if (st->panel != NULL)
		gtk_widget_destroy(st->panel); /* also removes it from the notebook   */
	if (st->menu_item != NULL)
		gtk_widget_destroy(st->menu_item);

	/* Don't leave our status-bar text behind after unload. */
	ui_set_statusbar(FALSE, "%s", "");

	g_free(st);
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Geany WebView");
	plugin->info->description =
		_("Reusable WebView host pane (terminal, Markdown/HTML preview, …).");
	plugin->info->version = "0.1.0";
	plugin->info->author = "Geany WebView contributors";

	plugin->funcs->init      = gwv_init;
	plugin->funcs->cleanup   = gwv_cleanup;
	plugin->funcs->configure = NULL;
	plugin->funcs->help      = NULL;
	plugin->funcs->callbacks = NULL;

	/* Keep the DLL in memory across disable/enable. This plugin uses json-glib
	 * (and keeps a process-wide WebView2 environment); unloading the module
	 * would tear down json-glib's GObject types — re-registering them on reload
	 * corrupts the type system ("cannot register existing type 'JsonParser'")
	 * and hangs on re-enable. Resident keeps types and statics valid. */
	plugin_module_make_resident(plugin);

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
