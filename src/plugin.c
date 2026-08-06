/*
 * plugin.c — Geany WebView plugin entry point.
 *
 * Registers the modern GeanyPlugin API, loads settings, and creates the enabled
 * views. The generic view lifecycle lives in view.c, per-view behaviour in
 * views/<name>.c, settings in settings.c, and the per-OS WebView backend in
 * host/<os>.{cc,m}.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <glib/gstdio.h>

#include "plugin.h"
#include "assets.h"
#include "settings.h"
#include "view.h"
#include "gwvutil.h"
#include "views/browser.h"
#include "views/preview.h"
#include "views/sideterm.h"
#include "views/terminal.h"

/* Single-instance plugin; keybinding callbacks (which get no user data) reach
 * state through this. */
static GwvState *g_gwv_state = NULL;

/* Directory of a document (or home if untitled/NULL). Caller g_free()s. */
gchar *gwv_doc_dir(GeanyDocument *doc)
{
	if (doc != NULL && doc->file_name != NULL)
		return g_path_get_dirname(doc->file_name);
	return g_strdup(g_get_home_dir());
}

/* Directory of the current document (or home if untitled). Caller g_free()s. */
gchar *gwv_current_doc_dir(void)
{
	return gwv_doc_dir(document_get_current());
}

/* ----------------------------------------------- Tools: copy file path */

/* Copy the active document's absolute path to the clipboard. */
static void on_copy_path_activate(GtkMenuItem *item, gpointer user)
{
	(void) item; (void) user;
	GeanyDocument *doc = document_get_current();
	if (doc == NULL || doc->file_name == NULL) {
		ui_set_statusbar(TRUE, "%s", _("No file path — the document is not saved."));
		return;
	}
	gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD),
	                       doc->file_name, -1);
	ui_set_statusbar(TRUE, _("Copied path: %s"), doc->file_name);
}

/* Grey the item out when there's no saved document to copy. Refreshed each time
 * the Tools menu opens; connect_object ties this handler's life to the item, so
 * it is removed automatically when the item is destroyed. */
static void on_tools_menu_show(GtkWidget *menu, gpointer item)
{
	(void) menu;
	GeanyDocument *doc = document_get_current();
	gtk_widget_set_sensitive(GTK_WIDGET(item),
	                         doc != NULL && doc->file_name != NULL);
}

void gwv_copy_path_create(GwvState *st)
{
	if (st->menu_copy_path != NULL)
		return;
	GtkWidget *tools_menu = st->plugin->geany_data->main_widgets->tools_menu;
	st->menu_copy_path = gtk_menu_item_new_with_mnemonic(_("Copy File _Path (WVM)"));
	gtk_widget_set_tooltip_text(st->menu_copy_path,
		_("Copy the absolute path of the active document to the clipboard."));
	g_signal_connect(st->menu_copy_path, "activate",
	                 G_CALLBACK(on_copy_path_activate), st);
	g_signal_connect_object(tools_menu, "show",
	                        G_CALLBACK(on_tools_menu_show), st->menu_copy_path, 0);
	gtk_widget_show(st->menu_copy_path);
	gtk_container_add(GTK_CONTAINER(tools_menu), st->menu_copy_path);
}

void gwv_copy_path_destroy(GwvState *st)
{
	if (st->menu_copy_path == NULL)
		return;
	gtk_widget_destroy(st->menu_copy_path);   /* also drops the menu 'show' handler */
	st->menu_copy_path = NULL;
}

/* --------------------------------------------------------- reveal actions */

static void reveal_preview(GwvState *st)
{
	gwv_view_reveal(st->preview,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook));
}

static void reveal_terminal(GwvState *st)
{
	if (st->terminal != NULL) {
		GtkWidget *nb = st->plugin->geany_data->main_widgets->message_window_notebook;
		/* Show the message window if it's currently hidden. */
		if (!gtk_widget_get_mapped(nb))
			keybindings_send_command(GEANY_KEY_GROUP_VIEW, GEANY_KEYS_VIEW_MESSAGEWINDOW);
		gwv_view_reveal(st->terminal, GTK_NOTEBOOK(nb));
	} else if (st->sideterm != NULL && st->sideterm->host != NULL) {
		wv_host_focus(st->sideterm->host);   /* only the side pane exists */
	}
}

static void kb_focus_terminal(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		reveal_terminal(g_gwv_state);
}

static void kb_focus_preview(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		reveal_preview(g_gwv_state);
}

/* ------------------------------------------------------------- plugin funcs */

static gboolean gwv_init(GeanyPlugin *plugin, gpointer pdata)
{
	(void) pdata;
	GeanyData *geany_data = plugin->geany_data;
	GwvState  *st = g_new0(GwvState, 1);
	st->plugin = plugin;

	/* The injected bridge shim, from the embedded assets. */
	GBytes *bridge = gwv_assets_lookup("bridge.js");
	if (bridge != NULL) {
		gsize blen = 0;
		gconstpointer bdata = g_bytes_get_data(bridge, &blen);
		st->bridge_js = g_strndup(bdata, blen);
		g_bytes_unref(bridge);
	} else {
		g_warning("GWV: embedded asset bridge.js not found");
	}

	/* Settings: create the file with defaults on first run, else load it. It
	 * lives in the plugin's own subfolder — the geany-plugins convention that
	 * keeps <config>/plugins/ tidy. Migrate the flat file older versions wrote
	 * directly into plugins/. */
	st->config_path = g_build_filename(geany_data->app->configdir,
	                                   "plugins", "geanywebview",
	                                   "geanywebview.conf", NULL);
	gchar *old_conf = g_build_filename(geany_data->app->configdir,
	                                   "plugins", "geanywebview.conf", NULL);
	if (!g_file_test(st->config_path, G_FILE_TEST_EXISTS) &&
	    g_file_test(old_conf, G_FILE_TEST_EXISTS)) {
		gchar *conf_dir = g_path_get_dirname(st->config_path);
		g_mkdir_with_parents(conf_dir, 0755);
		g_free(conf_dir);
		if (g_rename(old_conf, st->config_path) != 0)
			g_warning("GWV: could not migrate settings to %s", st->config_path);
	}
	g_free(old_conf);
	settings_load(st);

	/* Create the enabled views (preview eager so it renders at once; terminal
	 * lazy so a shell isn't spawned until opened). The panes have no Tools-menu
	 * entries — they're reachable via their tabs and the keybindings below. The
	 * only optional Tools items are the "Copy File Path (WVM)" and "Simplify
	 * Typography (WVM)" actions, which have no pane of their own to reach them
	 * from. */
	if (st->enable_preview)
		gwv_preview_create(st);
	if (st->enable_browser)
		gwv_browser_create(st);
	if (st->term_instances > 0)
		gwv_terminal_create(st);
	if (st->side_instances > 0)
		gwv_sideterm_create(st);
	if (st->tools_copy_path)
		gwv_copy_path_create(st);
	if (st->tools_typography)
		gwv_typography_create(st);

	/* Refresh the preview on document changes (debounced). */
	gwv_preview_connect_signals(st);

	/* Keybindings (unbound by default; the user assigns them in Preferences). */
	g_gwv_state = st;
	GeanyKeyGroup *kg = plugin_set_key_group(plugin, "geany_webview", KB_COUNT, NULL);
	keybindings_set_item(kg, KB_FOCUS_TERMINAL, kb_focus_terminal, 0, (GdkModifierType) 0,
	                     "focus_terminal", _("Focus " GWV_TERMINAL_LABEL), NULL);
	keybindings_set_item(kg, KB_FOCUS_PREVIEW, kb_focus_preview, 0, (GdkModifierType) 0,
	                     "focus_preview", _("Show " GWV_PREVIEW_LABEL), NULL);

	geany_plugin_set_data(plugin, st, NULL);
	return TRUE;
}

static void gwv_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	if (st == NULL)
		return;

	gwv_sideterm_destroy(st);   /* also restores the editor-area layout */
	gwv_terminal_destroy(st);
	gwv_browser_destroy(st);
	gwv_preview_destroy(st);
	gwv_copy_path_destroy(st);
	gwv_typography_destroy(st);

	ui_set_statusbar(FALSE, "%s", "");
	g_free(st->bridge_js);
	g_free(st->doc_host_dir);
	g_free(st->config_path);
	g_free(st->preview_theme);
	g_free(st->term_font_family);
	g_free(st->term_shell);
	g_free(st->browser_home);
	g_free(st);
	g_gwv_state = NULL;
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Geany WebView Multitool (WVM)");
	plugin->info->description =
		_("A set of tools in embedded WebView panes — the \"" GWV_PREVIEW_LABEL "\" "
		  "sidebar tab (Markdown/HTML), \"" GWV_TERMINAL_LABEL "\" instances in the "
		  "message window and right of the editor, a \"" GWV_BROWSER_LABEL "\" tab, "
		  "and extra Tools menu actions.\n"
		  "https://github.com/zhgzhg/Geany-WebView-Multitool");
	plugin->info->version = GWV_VERSION;   /* from the VERSION file, via meson */
	plugin->info->author = "zhgzhg @@ github.com";

	plugin->funcs->init      = gwv_init;
	plugin->funcs->cleanup   = gwv_cleanup;
	plugin->funcs->configure = gwv_configure;
	plugin->funcs->help      = NULL;
	plugin->funcs->callbacks = NULL;

	/* Keep the DLL resident: the process-wide WebView2 environment (and other
	 * static host state) must survive disable/enable. */
	plugin_module_make_resident(plugin);

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
