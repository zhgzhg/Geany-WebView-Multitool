/*
 * settings.c — plugin settings, persisted to a GKeyFile in the Geany plugin config
 * dir, plus the Plugin Manager -> Preferences page. Toggling a view live creates
 * or destroys its pane.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "settings.h"
#include "views/preview.h"
#include "views/terminal.h"

#define GWV_CFG_GROUP "general"

void settings_save(GwvState *st)
{
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "enable_preview",  st->enable_preview);
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "enable_terminal", st->enable_terminal);
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "terminal_primary_selection", st->term_primary);
	g_key_file_set_string (kf, GWV_CFG_GROUP, "preview_theme",
	                       st->preview_theme ? st->preview_theme : "dark");

	gchar *dir = g_path_get_dirname(st->config_path);
	g_mkdir_with_parents(dir, 0755);
	g_free(dir);

	gsize len = 0;
	gchar *data = g_key_file_to_data(kf, &len, NULL);
	if (!g_file_set_contents(st->config_path, data, len, NULL))
		g_warning("GWV: could not write config %s", st->config_path);
	g_free(data);
	g_key_file_free(kf);
}

void settings_load(GwvState *st)
{
	/* defaults */
	st->enable_preview  = TRUE;
	st->enable_terminal = TRUE;
	st->term_primary    = TRUE;
	g_free(st->preview_theme);
	st->preview_theme   = g_strdup("dark");

	GKeyFile *kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, st->config_path, G_KEY_FILE_NONE, NULL)) {
		GError *err = NULL;
		gboolean b;
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "enable_preview", &err);
		if (err == NULL) st->enable_preview = b; else g_clear_error(&err);
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "enable_terminal", &err);
		if (err == NULL) st->enable_terminal = b; else g_clear_error(&err);
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "terminal_primary_selection", &err);
		if (err == NULL) st->term_primary = b; else g_clear_error(&err);
		gchar *t = g_key_file_get_string(kf, GWV_CFG_GROUP, "preview_theme", NULL);
		if (t != NULL && (g_strcmp0(t, "dark") == 0 || g_strcmp0(t, "light") == 0)) {
			g_free(st->preview_theme);
			st->preview_theme = t;
		} else {
			g_free(t);
		}
		g_key_file_free(kf);
	} else {
		g_key_file_free(kf);
		settings_save(st);   /* first run: create it with defaults */
	}
}

void settings_apply(GwvState *st, gboolean enable_preview, gboolean enable_terminal,
                    gboolean term_primary)
{
	if (enable_preview != st->enable_preview) {
		st->enable_preview = enable_preview;
		if (enable_preview) gwv_preview_create(st);
		else                gwv_preview_destroy(st);
	}
	if (enable_terminal != st->enable_terminal) {
		st->enable_terminal = enable_terminal;
		if (enable_terminal) gwv_terminal_create(st);
		else                 gwv_terminal_destroy(st);
	}
	st->term_primary = term_primary;   /* consulted live at message time */
	settings_save(st);
}

static void on_configure_response(GtkDialog *dialog, gint response, gpointer user)
{
	(void) dialog;
	if (response != GTK_RESPONSE_OK && response != GTK_RESPONSE_APPLY)
		return;
	GwvState *st = user;
	settings_apply(st,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_preview)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_terminal)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_primary)));
}

GtkWidget *gwv_configure(GeanyPlugin *plugin, GtkDialog *dialog, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	g_object_set(box, "margin", 6, NULL);

	st->cfg_chk_preview = gtk_check_button_new_with_mnemonic(
		_("Show Markdown / HTML _preview in the sidebar"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_preview), st->enable_preview);
	gtk_box_pack_start(GTK_BOX(box), st->cfg_chk_preview, FALSE, FALSE, 0);

	st->cfg_chk_terminal = gtk_check_button_new_with_mnemonic(
		_("Show _terminal in the message window"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_terminal), st->enable_terminal);
	gtk_box_pack_start(GTK_BOX(box), st->cfg_chk_terminal, FALSE, FALSE, 0);

	st->cfg_chk_primary = gtk_check_button_new_with_mnemonic(
		_("Allow primary _selection copy/paste in the terminal (select copies, middle-click pastes)"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_primary), st->term_primary);
	gtk_box_pack_start(GTK_BOX(box), st->cfg_chk_primary, FALSE, FALSE, 0);

	gtk_widget_show_all(box);
	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), st);
	return box;
}
