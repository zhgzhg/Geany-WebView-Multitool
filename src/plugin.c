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
#include "assets.h"
#include "settings.h"
#include "view.h"
#include "gwvutil.h"
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

/* --------------------------------------------------------- reveal actions */

static void reveal_preview(GwvState *st)
{
	gwv_view_reveal(st->preview,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook));
}

static void reveal_terminal(GwvState *st)
{
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
	 * lazy so a shell isn't spawned until opened). No Tools-menu entries: the
	 * panes are reachable via their tabs, and the keybindings below can be
	 * bound in Preferences -> Keybindings. */
	if (st->enable_preview)
		gwv_preview_create(st);
	if (st->enable_terminal)
		gwv_terminal_create(st);

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

	gwv_terminal_destroy(st);
	gwv_preview_destroy(st);

	ui_set_statusbar(FALSE, "%s", "");
	g_free(st->bridge_js);
	g_free(st->doc_host_dir);
	g_free(st->config_path);
	g_free(st->preview_theme);
	g_free(st->term_shell);
	g_free(st);
	g_gwv_state = NULL;
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Geany WebView Multitool (WV)");
	plugin->info->description =
		_("A set of tools in embedded WebView panes — adds the \"" GWV_PREVIEW_LABEL "\" "
		  "sidebar tab (Markdown/HTML) and the \"" GWV_TERMINAL_LABEL "\" message-window tab.");
	plugin->info->version = "0.2.0";
	plugin->info->author = "zhgzhg @@ github.com";

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
