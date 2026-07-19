/*
 * plugin.c — Geany WebView plugin entry point.
 *
 * Registers the modern GeanyPlugin API, loads settings, and creates the enabled
 * views. The generic view lifecycle lives in view.c, per-view behaviour in
 * views/<name>.c, settings in settings.c, and the per-OS WebView backend in
 * host/<os>.{cc,m}.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <glib/gstdio.h>

#include "plugin.h"
#include "settings.h"
#include "view.h"
#include "util.h"
#include "views/preview.h"
#include "views/terminal.h"

/* Single-instance plugin; keybinding callbacks (which get no user data) reach
 * state through this. */
static GwvState *g_gwv_state = NULL;

/* Directory of the current document (or home if untitled). Caller g_free()s. */
gchar *gwv_current_doc_dir(void)
{
	GeanyDocument *doc = document_get_current();
	if (doc != NULL && doc->file_name != NULL)
		return g_path_get_dirname(doc->file_name);
	return g_strdup(g_get_home_dir());
}

/* --------------------------------------------------------------- menus */

static void on_menu_preview(GtkMenuItem *item, gpointer user)
{
	(void) item;
	GwvState *st = user;
	gwv_view_reveal(st->preview,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook));
}

static void on_menu_terminal(GtkMenuItem *item, gpointer user)
{
	(void) item;
	GwvState *st = user;
	GtkWidget *nb = st->plugin->geany_data->main_widgets->message_window_notebook;
	/* Show the message window if it's currently hidden. */
	if (!gtk_widget_get_mapped(nb))
		keybindings_send_command(GEANY_KEY_GROUP_VIEW, GEANY_KEYS_VIEW_MESSAGEWINDOW);
	gwv_view_reveal(st->terminal, GTK_NOTEBOOK(nb));
}

static void kb_focus_terminal(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		on_menu_terminal(NULL, g_gwv_state);
}

static void kb_focus_preview(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		on_menu_preview(NULL, g_gwv_state);
}

/* ------------------------------------------------------------- plugin funcs */

static gboolean gwv_init(GeanyPlugin *plugin, gpointer pdata)
{
	(void) pdata;
	GeanyData *geany_data = plugin->geany_data;
	GwvState  *st = g_new0(GwvState, 1);
	st->plugin = plugin;

	/* Shared assets: the plugin's install dir + the injected bridge shim. */
	gchar *dir = gwv_plugin_dir();
	st->asset_root = g_build_filename(dir, GWV_ASSET_SUBDIR, NULL);
	g_free(dir);
	gchar *bridge_path = g_build_filename(st->asset_root, "bridge.js", NULL);
	if (!g_file_get_contents(bridge_path, &st->bridge_js, NULL, NULL))
		g_warning("GWV: could not read %s", bridge_path);
	g_free(bridge_path);

	/* Tools menu. */
	st->menu_preview = gtk_menu_item_new_with_mnemonic(_("Show _Preview"));
	gtk_widget_show(st->menu_preview);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_preview);
	g_signal_connect(st->menu_preview, "activate", G_CALLBACK(on_menu_preview), st);

	st->menu_terminal = gtk_menu_item_new_with_mnemonic(_("Open _Terminal"));
	gtk_widget_show(st->menu_terminal);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_terminal);
	g_signal_connect(st->menu_terminal, "activate", G_CALLBACK(on_menu_terminal), st);

	/* Settings: create the file with defaults on first run, else load it. */
	st->config_path = g_build_filename(geany_data->app->configdir,
	                                   "plugins", "geanywebview.conf", NULL);
	settings_load(st);

	/* Create the enabled views (preview eager so it renders at once; terminal
	 * lazy so a shell isn't spawned until opened). */
	if (st->enable_preview)
		gwv_preview_create(st);
	if (st->enable_terminal)
		gwv_terminal_create(st);
	gtk_widget_set_sensitive(st->menu_preview,  st->enable_preview);
	gtk_widget_set_sensitive(st->menu_terminal, st->enable_terminal);

	/* Refresh the preview on document changes (debounced). */
	gwv_preview_connect_signals(st);

	/* Keybindings (unbound by default; the user assigns them in Preferences). */
	g_gwv_state = st;
	GeanyKeyGroup *kg = plugin_set_key_group(plugin, "geany_webview", KB_COUNT, NULL);
	keybindings_set_item(kg, KB_FOCUS_TERMINAL, kb_focus_terminal, 0, (GdkModifierType) 0,
	                     "focus_terminal", _("Focus terminal"), st->menu_terminal);
	keybindings_set_item(kg, KB_FOCUS_PREVIEW, kb_focus_preview, 0, (GdkModifierType) 0,
	                     "focus_preview", _("Show preview"), st->menu_preview);

	geany_plugin_set_data(plugin, st, NULL);
	return TRUE;
}

static void gwv_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	if (st == NULL)
		return;

	gwv_terminal_destroy(st);
	gwv_preview_destroy(st);
	if (st->menu_terminal != NULL)
		gtk_widget_destroy(st->menu_terminal);
	if (st->menu_preview != NULL)
		gtk_widget_destroy(st->menu_preview);

	ui_set_statusbar(FALSE, "%s", "");
	if (st->asset_root != NULL) {
		gchar *hp = g_build_filename(st->asset_root, GWV_HTMLPREVIEW_FILE, NULL);
		g_unlink(hp);
		g_free(hp);
	}
	g_free(st->asset_root);
	g_free(st->bridge_js);
	g_free(st->doc_host_dir);
	g_free(st->config_path);
	g_free(st->preview_theme);
	g_free(st);
	g_gwv_state = NULL;
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Geany WebView");
	plugin->info->description =
		_("Reusable WebView host panes: terminal (ConPTY) and Markdown/HTML preview.");
	plugin->info->version = "0.2.0";
	plugin->info->author = "Geany WebView contributors";

	plugin->funcs->init      = gwv_init;
	plugin->funcs->cleanup   = gwv_cleanup;
	plugin->funcs->configure = gwv_configure;
	plugin->funcs->help      = NULL;
	plugin->funcs->callbacks = NULL;

	/* Keep the DLL resident: json-glib's GObject types (and our process-wide
	 * WebView2 environment) must survive disable/enable. */
	plugin_module_make_resident(plugin);

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
