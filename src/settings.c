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

/* Parse a Pango-style "Family Size" description (Geany stores its fonts the
 * same way) into the terminal font fields. The number is used as px — the
 * xterm.js unit. Empty/unparsable input keeps the current values. */
static void settings_parse_font(GwvState *st, const char *desc)
{
	if (desc == NULL || *desc == '\0')
		return;
	PangoFontDescription *fd = pango_font_description_from_string(desc);
	if (fd == NULL)
		return;
	const char *family = pango_font_description_get_family(fd);
	if (family != NULL && *family != '\0') {
		g_free(st->term_font_family);
		st->term_font_family = g_strdup(family);
	}
	int size = pango_font_description_get_size(fd) / PANGO_SCALE;
	if (size > 0)
		st->term_font = CLAMP(size, 6, 32);
	pango_font_description_free(fd);
}

/* The stored/displayed counterpart: "Family Size". g_free() the result. */
static gchar *settings_font_desc(GwvState *st)
{
	return g_strdup_printf("%s %d",
	                       st->term_font_family != NULL ? st->term_font_family
	                                                    : "Monospace",
	                       st->term_font);
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
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "tools_simplify_typography", st->tools_typography);
	for (int i = 0; i < GWV_TYPO_COUNT; i++)
		g_key_file_set_boolean(kf, GWV_CFG_GROUP, gwv_typography_group_key(i),
		                       st->typography_groups[i]);
	/* 0 instances = that terminal pane is disabled (no enable flags). */
	g_key_file_set_integer(kf, GWV_CFG_GROUP, "terminal_instances", st->term_instances);
	g_key_file_set_integer(kf, GWV_CFG_GROUP, "terminal_side_instances", st->side_instances);
	gchar *font = settings_font_desc(st);
	g_key_file_set_string (kf, GWV_CFG_GROUP, "terminal_font", font);
	g_free(font);
	g_key_file_set_integer(kf, GWV_CFG_GROUP, "terminal_scrollback", st->term_scrollback);
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
	st->tools_typography  = TRUE;
	for (int i = 0; i < GWV_TYPO_COUNT; i++)
		st->typography_groups[i] = gwv_typography_group_default(i);
	st->term_instances  = 1;      /* bottom terminal on, single instance */
	st->side_instances  = 1;      /* side terminal on, single instance   */
	st->term_font       = 13;     /* xterm.js default */
	st->term_scrollback = 30000;  /* lines kept above the visible screen */
	st->preview_mode    = 0;      /* auto */
	g_free(st->term_font_family);
	/* fontconfig alias on the WebKitGTK platforms; unknown to DirectWrite, so
	 * Windows falls through to the page's built-in monospace stack. */
	st->term_font_family = g_strdup("Monospace");
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
		b = g_key_file_get_boolean(kf, GWV_CFG_GROUP, "tools_simplify_typography", &err);
		if (err == NULL) st->tools_typography = b; else g_clear_error(&err);
		for (int i = 0; i < GWV_TYPO_COUNT; i++) {
			b = g_key_file_get_boolean(kf, GWV_CFG_GROUP,
			                           gwv_typography_group_key(i), &err);
			if (err == NULL) st->typography_groups[i] = b; else g_clear_error(&err);
		}
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
		gchar *f = g_key_file_get_string(kf, GWV_CFG_GROUP, "terminal_font", NULL);
		if (f != NULL && *f != '\0') {
			settings_parse_font(st, f);
		} else {
			/* Migrate configs from before the family setting: a size-only
			 * key. The next save writes terminal_font and drops it. */
			n = g_key_file_get_integer(kf, GWV_CFG_GROUP, "terminal_font_size", &err);
			if (err == NULL) st->term_font = CLAMP(n, 6, 32); else g_clear_error(&err);
		}
		g_free(f);
		n = g_key_file_get_integer(kf, GWV_CFG_GROUP, "terminal_scrollback", &err);
		if (err == NULL) st->term_scrollback = CLAMP(n, 0, 1000000); else g_clear_error(&err);
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
	g_debug("GWV: settings loaded: preview=%d browser=%d term=%d side=%d "
	        "font=%s/%d scrollback=%d",
	        st->enable_preview, st->enable_browser,
	        st->term_instances, st->side_instances,
	        st->term_font_family, st->term_font, st->term_scrollback);
}

void settings_apply(GwvState *st, gboolean enable_preview, gboolean enable_browser,
                    const char *browser_home, gboolean term_primary,
                    gboolean term_search, gboolean tools_copy_path,
                    gboolean tools_typography, const gboolean *typography_groups,
                    int term_instances, int side_instances,
                    const char *term_font_desc, int term_scrollback,
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
	if (tools_typography != st->tools_typography) {
		st->tools_typography = tools_typography;
		if (tools_typography) gwv_typography_create(st);
		else                gwv_typography_destroy(st);
	}
	for (int i = 0; i < GWV_TYPO_COUNT; i++)   /* consulted live at click time */
		st->typography_groups[i] = typography_groups[i];
	st->term_primary = term_primary;   /* consulted live at message time */
	st->term_search  = term_search;    /* pushed with term.config below  */

	settings_parse_font(st, term_font_desc);   /* pushed with term.config below */
	st->term_scrollback = CLAMP(term_scrollback, 0, 1000000);   /* ditto */

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
	gchar *font = gtk_font_chooser_get_font(GTK_FONT_CHOOSER(st->cfg_font_btn));
	gboolean typo_groups[GWV_TYPO_COUNT];
	for (int i = 0; i < GWV_TYPO_COUNT; i++)
		typo_groups[i] = gtk_toggle_button_get_active(
			GTK_TOGGLE_BUTTON(st->cfg_chk_typo_groups[i]));
	settings_apply(st,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_preview)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_browser)),
		gtk_entry_get_text(GTK_ENTRY(st->cfg_entry_home)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_primary)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_search)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_copy_path)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_typography)),
		typo_groups,
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->cfg_spin_instances)),
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->cfg_spin_side)),
		font,
		gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(st->cfg_spin_scrollback)),
		settings_preview_mode_value(mode_id),
		gtk_entry_get_text(GTK_ENTRY(st->cfg_entry_shell)));
	g_free(font);
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

/* Height the settings page may take before it starts scrolling: the work area
 * of the monitor showing Geany, minus room for the dialog's decorations, the
 * notebook tabs and the button box. */
static int pref_max_height(GtkDialog *dialog)
{
	GdkDisplay *dpy = gtk_widget_get_display(GTK_WIDGET(dialog));
	GtkWindow *parent = gtk_window_get_transient_for(GTK_WINDOW(dialog));
	GdkWindow *gdkwin = parent != NULL ? gtk_widget_get_window(GTK_WIDGET(parent)) : NULL;
	GdkMonitor *mon = gdkwin != NULL ? gdk_display_get_monitor_at_window(dpy, gdkwin) : NULL;

	if (mon == NULL) mon = gdk_display_get_primary_monitor(dpy);
	if (mon == NULL) mon = gdk_display_get_monitor(dpy, 0);
	if (mon == NULL) return 480;

	GdkRectangle wa;
	gdk_monitor_get_workarea(mon, &wa);
	return MAX(240, wa.height - 160);
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

	gchar *font = settings_font_desc(st);
	st->cfg_font_btn = gtk_font_button_new_with_font(font);
	g_free(font);
	gtk_font_chooser_set_level(GTK_FONT_CHOOSER(st->cfg_font_btn),
	                           GTK_FONT_CHOOSER_LEVEL_FAMILY |
	                           GTK_FONT_CHOOSER_LEVEL_SIZE);
	gtk_widget_set_tooltip_text(st->cfg_font_btn,
		_("Family and size (px, 6-32) for both terminal panes; applies "
		  "immediately. Ctrl+= / Ctrl+- / Ctrl+0 zoom a terminal temporarily."));
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("_Font:"), st->cfg_font_btn, FALSE),
	                   FALSE, FALSE, 0);

	st->cfg_spin_scrollback = gtk_spin_button_new_with_range(0, 1000000, 1000);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(st->cfg_spin_scrollback), st->term_scrollback);
	gtk_widget_set_tooltip_text(st->cfg_spin_scrollback,
		_("Lines kept above the visible screen, per terminal instance. "
		  "0 disables scrollback. Applies to both panes, immediately."));
	gtk_box_pack_start(GTK_BOX(grp), pref_row(_("Scrollbac_k lines:"), st->cfg_spin_scrollback, FALSE),
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

	st->cfg_chk_typography = gtk_check_button_new_with_mnemonic(
		_("Simplify _typography menu item"));
	gtk_widget_set_tooltip_text(st->cfg_chk_typography,
		_("Adds \"" "Simplify Typography (WVM)" "\" to Geany's Tools menu; it replaces "
		  "Unicode symbols typical of LLM output with the plain ASCII a human "
		  "would type — in the selection, or the whole document when nothing "
		  "is selected. One undo step; the groups below pick what changes."));
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(st->cfg_chk_typography),
	                             st->tools_typography);
	gtk_box_pack_start(GTK_BOX(grp), st->cfg_chk_typography, FALSE, FALSE, 0);

	GtkWidget *typo = gtk_grid_new();
	gtk_grid_set_column_spacing(GTK_GRID(typo), 12);
	gtk_grid_set_row_spacing(GTK_GRID(typo), 2);
	g_object_set(typo, "margin-start", 18, NULL);
	for (int i = 0; i < GWV_TYPO_COUNT; i++) {
		GtkWidget *chk = gtk_check_button_new_with_label(gwv_typography_group_label(i));
		gtk_widget_set_tooltip_text(chk, gwv_typography_group_tip(i));
		gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chk), st->typography_groups[i]);
		gtk_grid_attach(GTK_GRID(typo), chk, i % 3, i / 3, 1, 1);
		st->cfg_chk_typo_groups[i] = chk;
	}
	/* The group checkboxes only mean something while the item exists. */
	g_object_bind_property(st->cfg_chk_typography, "active", typo, "sensitive",
	                       G_BINDING_SYNC_CREATE);
	gtk_box_pack_start(GTK_BOX(grp), typo, FALSE, FALSE, 0);

	/* Geany packs this page into an expanding notebook page, so the dialog is
	 * as tall as the tallest plugin's settings — off-screen, and unreachable,
	 * on a short display. Scroll vertically past the monitor's work area;
	 * below that the natural size still wins, so nothing changes visually on
	 * a roomy screen. The width is never scrolled — it always fits. */
	GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw),
	                               GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(sw), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(sw),
	                                           pref_max_height(dialog));
	gtk_container_add(GTK_CONTAINER(sw), box);

	gtk_widget_show_all(sw);
	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), st);
	return sw;
}
