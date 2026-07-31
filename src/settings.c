/*
 * settings.c — plugin settings, persisted to a GKeyFile in the Geany plugin config
 * dir, plus the Plugin Manager -> Preferences page. Toggling a view live creates
 * or destroys its pane.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "settings.h"
#include "views/browser.h"
#include "views/preview.h"
#include "views/sideterm.h"
#include "views/terminal.h"

#define GWV_CFG_GROUP "general"

const char *settings_preview_mode_name(int mode)
{
	if (mode == 1) return "markdown";
	if (mode == 2) return "html";
	return "auto";
}

int settings_preview_mode_value(const char *name)
{
	if (g_strcmp0(name, "markdown") == 0 || g_strcmp0(name, "md") == 0) return 1;
	if (g_strcmp0(name, "html") == 0)                                   return 2;
	return 0;
}

void settings_save(GwvState *st)
{
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "enable_preview",  st->enable_preview);
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "enable_browser",  st->enable_browser);
	g_key_file_set_string (kf, GWV_CFG_GROUP, "browser_home",
	                       st->browser_home ? st->browser_home : "");
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "terminal_primary_selection", st->term_primary);
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "terminal_search", st->term_search);
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "tools_copy_file_path", st->tools_copy_path);
	/* 0 instances = that terminal pane is disabled (no enable flags). */
	g_key_file_set_integer(kf, GWV_CFG_GROUP, "terminal_instances", st->term_instances);
	g_key_file_set_integer(kf, GWV_CFG_GROUP, "terminal_side_instances", st->side_instances);
	g_key_file_set_integer(kf, GWV_CFG_GROUP, "terminal_font_size", st->term_font);
	g_key_file_set_string (kf, GWV_CFG_GROUP, "terminal_shell",
	                       st->term_shell ? st->term_shell : "");
	g_key_file_set_string (kf, GWV_CFG_GROUP, "preview_mode",
	                       settings_preview_mode_name(st->preview_mode));
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
	st->enable_browser  = TRUE;
	st->term_primary    = TRUE;
	st->term_search     = TRUE;
	st->tools_copy_path = TRUE;
	st->term_instances  = 1;      /* bottom terminal on, single instance */
	st->side_instances  = 1;      /* side terminal on, single instance   */
	st->term_font       = 13;     /* xterm.js default */
	st->preview_mode    = 0;      /* auto */
	g_free(st->term_shell);
	st->term_shell      = NULL;   /* platform auto-detection */
	g_free(st->browser_home);
	st->browser_home    = NULL;   /* about:blank */
	g_free(st->preview_theme);
	st->preview_theme   = g_strdup("dark");

	GKeyFile *kf = g_key_file_new();
	if (g_key_file_load_from_file(kf, st->config_path, G_KEY_FILE_NONE, NULL)) {
		GError *err = NULL;
		gboolean b;
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "enable_preview", &err);
		if (err == NULL) st->enable_preview = b; else g_clear_error(&err);
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "enable_browser", &err);
		if (err == NULL) st->enable_browser = b; else g_clear_error(&err);
		gchar *home = g_key_file_get_string(kf, GWV_CFG_GROUP, "browser_home", NULL);
		if (home != NULL && *home != '\0')
			st->browser_home = home;
		else
			g_free(home);
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "terminal_primary_selection", &err);
		if (err == NULL) st->term_primary = b; else g_clear_error(&err);
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "terminal_search", &err);
		if (err == NULL) st->term_search = b; else g_clear_error(&err);
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "tools_copy_file_path", &err);
		if (err == NULL) st->tools_copy_path = b; else g_clear_error(&err);
		gint n = g_key_file_get_integer(kf, GWV_CFG_GROUP, "terminal_instances", &err);
		if (err == NULL) st->term_instances = CLAMP(n, 0, 8); else g_clear_error(&err);
		n = g_key_file_get_integer(kf, GWV_CFG_GROUP, "terminal_side_instances", &err);
		if (err == NULL) {
			st->side_instances = CLAMP(n, 0, 8);
		} else {
			g_clear_error(&err);
			/* Migrate configs from before per-pane counts: the side pane was a
			 * flag sharing the bottom count. An explicit flag wins either way;
			 * only configs that never saw the flag keep the default. */
			b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "terminal_side_panel", &err);
			if (err == NULL)
				st->side_instances = b ? MAX(1, st->term_instances) : 0;
			else
				g_clear_error(&err);
		}
		n = g_key_file_get_integer(kf, GWV_CFG_GROUP, "terminal_font_size", &err);
		if (err == NULL) st->term_font = CLAMP(n, 6, 32); else g_clear_error(&err);
		/* Legacy enable flag (pre-0-means-off): off overrides the count. */
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "enable_terminal", &err);
		if (err == NULL && !b) st->term_instances = 0; else g_clear_error(&err);
		gchar *sh = g_key_file_get_string(kf, GWV_CFG_GROUP, "terminal_shell", NULL);
		if (sh != NULL && *sh != '\0')
			st->term_shell = sh;
		else
			g_free(sh);
		gchar *m = g_key_file_get_string(kf, GWV_CFG_GROUP, "preview_mode", NULL);
		if (m != NULL)
			st->preview_mode = settings_preview_mode_value(m);
		g_free(m);
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
	g_debug("GWV: settings loaded: preview=%d browser=%d term=%d side=%d",
	        st->enable_preview, st->enable_browser,
	        st->term_instances, st->side_instances);
}

void settings_apply(GwvState *st, gboolean enable_preview, gboolean enable_browser,
                    const char *browser_home, gboolean term_primary,
                    gboolean term_search, gboolean tools_copy_path,
                    int term_instances, int side_instances, int term_font,
                    int preview_mode, const char *term_shell)
{
	if (enable_preview != st->enable_preview) {
		st->enable_preview = enable_preview;
		if (enable_preview) gwv_preview_create(st);
		else                gwv_preview_destroy(st);
	}
	g_free(st->browser_home);          /* used on Home and at pane creation */
	st->browser_home = (browser_home != NULL && *browser_home != '\0')
	                   ? g_strdup(browser_home) : NULL;
	if (enable_browser != st->enable_browser) {
		st->enable_browser = enable_browser;
		if (enable_browser) gwv_browser_create(st);
		else                gwv_browser_destroy(st);
	}
	gwv_browser_sync_home(st);         /* keep the Home tooltip truthful */
	if (tools_copy_path != st->tools_copy_path) {
		st->tools_copy_path = tools_copy_path;
		if (tools_copy_path) gwv_copy_path_create(st);
		else                 gwv_copy_path_destroy(st);
	}
	st->term_primary = term_primary;   /* consulted live at message time */
	st->term_search  = term_search;    /* pushed with term.config below  */

	st->term_font = CLAMP(term_font, 6, 32);   /* pushed with term.config below */

	/* Instance counts drive the panes: 0 = pane disabled. */
	st->term_instances = CLAMP(term_instances, 0, 8);
	if (st->term_instances == 0)           gwv_terminal_destroy(st);
	else if (st->terminal == NULL)         gwv_terminal_create(st);
	st->side_instances = CLAMP(side_instances, 0, 8);
	if (st->side_instances == 0)           gwv_sideterm_destroy(st);
	else if (st->sideterm == NULL)         gwv_sideterm_create(st);
	gwv_terminal_sync_instances(st);       /* pages add/remove tabs live */

	g_free(st->term_shell);            /* applies when the shell next starts */
	st->term_shell = (term_shell != NULL && *term_shell != '\0')
	                 ? g_strdup(term_shell) : NULL;

	if (preview_mode != st->preview_mode) {
		st->preview_mode = preview_mode;
		gwv_preview_sync_mode(st);     /* toolbar highlight + re-render */
	}
	settings_save(st);
}

static void on_configure_response(GtkDialog *dialog, gint response, gpointer user)
{
	(void) dialog;
	if (response != GTK_RESPONSE_OK && response != GTK_RESPONSE_APPLY)
		return;
	GwvState *st = user;
	const gchar *mode_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(st->cfg_combo_mode));
	settings_apply(st,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_preview)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_browser)),
		gtk_entry_get_text(GTK_ENTRY(st->cfg_entry_home)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_primary)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_search)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_copy_path)),
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->cfg_spin_instances)),
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->cfg_spin_side)),
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->cfg_spin_font)),
		settings_preview_mode_value(mode_id),
		gtk_entry_get_text(GTK_ENTRY(st->cfg_entry_shell)));
}

/* A labelled row (label + control) for inside a group. */
static GtkWidget *pref_row(const gchar *mnemonic_label, GtkWidget *control,
                           gboolean control_expands)
{
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
	GtkWidget *lbl = gtk_label_new_with_mnemonic(mnemonic_label);
	gtk_label_set_mnemonic_widget(GTK_LABEL(lbl), control);
	gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(row), control, control_expands, control_expands, 0);
	return row;
}

/* A titled settings group (bold header, indented content), in the style of
 * Geany's own preference frames. Returns the box to pack rows into. */
static GtkWidget *pref_group(GtkWidget *parent_box, const gchar *title)
{
	GtkWidget *frame = gtk_frame_new(NULL);
	gtk_frame_set_shadow_type(GTK_FRAME(frame), GTK_SHADOW_NONE);
	GtkWidget *lbl = gtk_label_new(NULL);
	gchar *markup = g_markup_printf_escaped("<b>%s</b>", title);
	gtk_label_set_markup(GTK_LABEL(lbl), markup);
	g_free(markup);
	gtk_frame_set_label_widget(GTK_FRAME(frame), lbl);

	GtkWidget *grp = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
	g_object_set(grp, "margin-start", 12, "margin-top", 6, NULL);
	gtk_container_add(GTK_CONTAINER(frame), grp);
	gtk_box_pack_start(GTK_BOX(parent_box), frame, FALSE, FALSE, 0);
	return grp;
}

GtkWidget *gwv_configure(GeanyPlugin *plugin, GtkDialog *dialog, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
	g_object_set(box, "margin", 6, NULL);

	/* ------------------------------ Preview ------------------------------ */
	GtkWidget *grp = pref_group(box, _(GWV_PREVIEW_LABEL));

	st->cfg_chk_preview = gtk_check_button_new_with_mnemonic(_("Show in the _sidebar"));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_preview), st->enable_preview);
	gtk_box_pack_start(GTK_BOX(grp), st->cfg_chk_preview, FALSE, FALSE, 0);

	st->cfg_combo_mode = gtk_combo_box_text_new();
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->cfg_combo_mode), "auto",
	                          _("Auto (by file type)"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->cfg_combo_mode), "markdown",
	                          _("Markdown"));
	gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(st->cfg_combo_mode), "html",
	                          _("HTML"));
	gtk_combo_box_set_active_id(GTK_COMBO_BOX(st->cfg_combo_mode),
	                            settings_preview_mode_name(st->preview_mode));
	gtk_widget_set_tooltip_text(st->cfg_combo_mode,
		_("The preview toolbar changes this too; both are remembered."));
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("Default _mode:"), st->cfg_combo_mode, FALSE),
	                   FALSE, FALSE, 0);

	/* ------------------------------ Browser ------------------------------ */
	grp = pref_group(box, _(GWV_BROWSER_LABEL));

	st->cfg_chk_browser = gtk_check_button_new_with_mnemonic(_("Show in the side_bar"));
	gtk_widget_set_tooltip_text(st->cfg_chk_browser,
		_("A web browser pane — browse documentation or a local dev server "
		  "without leaving Geany."));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_browser), st->enable_browser);
	gtk_box_pack_start(GTK_BOX(grp), st->cfg_chk_browser, FALSE, FALSE, 0);

	st->cfg_entry_home = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(st->cfg_entry_home),
	                   st->browser_home != NULL ? st->browser_home : "");
	gtk_entry_set_placeholder_text(GTK_ENTRY(st->cfg_entry_home), _("about:blank"));
	gtk_widget_set_tooltip_text(st->cfg_entry_home,
		_("Loaded when the pane opens and on the Home button."));
	gtk_widget_set_hexpand(st->cfg_entry_home, TRUE);
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("_Home page:"), st->cfg_entry_home, TRUE),
	                   FALSE, FALSE, 0);

	/* --------------------- Terminal: shared settings --------------------- */
	grp = pref_group(box, _(GWV_TERMINAL_LABEL));

	st->cfg_entry_shell = gtk_entry_new();
	gtk_entry_set_text(GTK_ENTRY(st->cfg_entry_shell),
	                   st->term_shell != NULL ? st->term_shell : "");
#ifdef G_OS_WIN32
	gtk_entry_set_placeholder_text(GTK_ENTRY(st->cfg_entry_shell),
	                               _("auto (pwsh, powershell or cmd)"));
#else
	gtk_entry_set_placeholder_text(GTK_ENTRY(st->cfg_entry_shell), _("auto ($SHELL)"));
#endif
	gtk_widget_set_tooltip_text(st->cfg_entry_shell,
		_("Full command line; leave empty for the platform default. "
		  "Applies when the terminal shell next starts (type `exit`, then Enter)."));
	gtk_widget_set_hexpand(st->cfg_entry_shell, TRUE);
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("Shell _command:"), st->cfg_entry_shell, TRUE),
	                   FALSE, FALSE, 0);

	st->cfg_chk_primary = gtk_check_button_new_with_mnemonic(
		_("Allow primary se_lection copy/paste"));
	gtk_widget_set_tooltip_text(st->cfg_chk_primary,
		_("Selecting text copies it to the primary selection; middle-click pastes it. "
		  "Independent of the regular clipboard."));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_primary), st->term_primary);
	gtk_box_pack_start(GTK_BOX(grp), st->cfg_chk_primary, FALSE, FALSE, 0);

	st->cfg_chk_search = gtk_check_button_new_with_mnemonic(
		_("Ctrl+F searches the terminal"));
	gtk_widget_set_tooltip_text(st->cfg_chk_search,
		_("Opens a find bar searching the visible terminal including its "
		  "scrollback. Turn off if a program inside the terminal needs to "
		  "receive Ctrl+F itself."));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_search), st->term_search);
	gtk_box_pack_start(GTK_BOX(grp), st->cfg_chk_search, FALSE, FALSE, 0);

	st->cfg_spin_font = gtk_spin_button_new_with_range(6, 32, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->cfg_spin_font), st->term_font);
	gtk_widget_set_tooltip_text(st->cfg_spin_font,
		_("Applies to both terminal panes, immediately."));
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("Font si_ze:"), st->cfg_spin_font, FALSE),
	                   FALSE, FALSE, 0);

	/* ------------------ Terminal: per-pane instance counts --------------- */
	const gchar *instances_tip =
		_("0 disables this terminal pane. With more than one, a tab row inside "
		  "the pane switches between them; each shell starts when its tab is "
		  "first opened. Lowering the count closes the highest-numbered "
		  "terminals and ends their shells.");

	grp = pref_group(box, _(GWV_TERMINAL_LABEL " — message window"));
	st->cfg_spin_instances = gtk_spin_button_new_with_range(0, 8, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->cfg_spin_instances), st->term_instances);
	gtk_widget_set_tooltip_text(st->cfg_spin_instances, instances_tip);
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("_Instances:"), st->cfg_spin_instances, FALSE),
	                   FALSE, FALSE, 0);

	grp = pref_group(box, _(GWV_TERMINAL_LABEL " — right of the editor"));
	st->cfg_spin_side = gtk_spin_button_new_with_range(0, 8, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->cfg_spin_side), st->side_instances);
	gtk_widget_set_tooltip_text(st->cfg_spin_side, instances_tip);
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("I_nstances:"), st->cfg_spin_side, FALSE),
	                   FALSE, FALSE, 0);

	/* ------------------------------- Tools ------------------------------- */
	grp = pref_group(box, _("Tools"));

	st->cfg_chk_copy_path = gtk_check_button_new_with_mnemonic(
		_("_Copy file path menu item"));
	gtk_widget_set_tooltip_text(st->cfg_chk_copy_path,
		_("Adds \"" "Copy File Path (WVM)" "\" to Geany's Tools menu; it copies the "
		  "absolute path of the active document to the clipboard."));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_copy_path), st->tools_copy_path);
	gtk_box_pack_start(GTK_BOX(grp), st->cfg_chk_copy_path, FALSE, FALSE, 0);

	gtk_widget_show_all(box);
	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), st);
	return box;
}
