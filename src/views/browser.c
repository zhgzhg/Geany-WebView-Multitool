/*
 * views/browser.c — sidebar web browser pane: the WebView host in free-browsing
 * mode (allow_browsing) with a native GTK navigation toolbar. No bridge script
 * is injected — arbitrary websites must not see the host message channel; the
 * toolbar is plain GTK, so none is needed.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>

#include "views/browser.h"
#include "view.h"

typedef struct {
	GtkWidget *entry;       /* address bar */
	GtkWidget *home_btn;    /* tooltip shows the configured home page */
	GtkWidget *findbar;     /* find-in-page row (WebKitGTK only), hidden */
	GtkWidget *find_entry;
	GtkWidget *match_label;
} BrowserUi;

static const char *browser_home_url(GwvState *st)
{
	return (st->browser_home != NULL && *st->browser_home != '\0')
	       ? st->browser_home : "about:blank";
}

/* "localhost:5173" -> http://…, "example.org" -> https://…; full URLs pass. */
static gchar *normalize_url(const char *text)
{
	if (strstr(text, "://") != NULL || g_str_has_prefix(text, "about:"))
		return g_strdup(text);
	if (g_str_has_prefix(text, "localhost") || g_str_has_prefix(text, "127.") ||
	    g_str_has_prefix(text, "192.168.")  || g_str_has_prefix(text, "10.") ||
	    g_str_has_prefix(text, "[::1]"))
		return g_strconcat("http://", text, NULL);   /* dev servers are http */
	return g_strconcat("https://", text, NULL);
}

/* ------------------------------- callbacks ------------------------------- */

static void on_nav_back(GtkButton *btn, gpointer user)
{
	(void) btn;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_go_back(st->browser->host);
}

static void on_nav_forward(GtkButton *btn, gpointer user)
{
	(void) btn;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_go_forward(st->browser->host);
}

static void on_nav_reload(GtkButton *btn, gpointer user)
{
	(void) btn;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_reload(st->browser->host);
}

static void on_nav_home(GtkButton *btn, gpointer user)
{
	(void) btn;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_navigate(st->browser->host, browser_home_url(st));
}

static void on_entry_activate(GtkEntry *entry, gpointer user)
{
	GwvState *st = user;
	const gchar *text = gtk_entry_get_text(entry);
	if (st->browser == NULL || text == NULL || *text == '\0')
		return;
	gchar *url = normalize_url(text);
	wv_host_navigate(st->browser->host, url);
	wv_host_focus(st->browser->host);
	g_free(url);
}

/* Document URL changed -> reflect it in the address bar (not while the user is
 * typing there; about:blank shows as empty so the placeholder invites input). */
static void on_url_changed(GwvView *v, const char *url)
{
	BrowserUi *ui = v->view_data;
	if (ui == NULL || ui->entry == NULL || gtk_widget_has_focus(ui->entry))
		return;
	if (url == NULL || g_strcmp0(url, "about:blank") == 0)
		url = "";
	gtk_entry_set_text(GTK_ENTRY(ui->entry), url);
}

/* ------------------------------ find in page ----------------------------- */

/* Only built where the backend has no find bar of its own (WebKitGTK); on
 * Windows Ctrl+F is a browser accelerator and WebView2 shows the Edge bar. */

static void findbar_close(GwvState *st)
{
	BrowserUi *ui = (st->browser != NULL) ? st->browser->view_data : NULL;
	if (ui == NULL || ui->findbar == NULL || !gtk_widget_get_visible(ui->findbar))
		return;
	wv_host_find_stop(st->browser->host);
	gtk_widget_hide(ui->findbar);
	wv_host_focus(st->browser->host);
}

static void findbar_open(GwvState *st)
{
	BrowserUi *ui = (st->browser != NULL) ? st->browser->view_data : NULL;
	if (ui == NULL || ui->findbar == NULL)
		return;
	gtk_widget_show(ui->findbar);
	gtk_widget_grab_focus(ui->find_entry);
	gtk_editable_select_region(GTK_EDITABLE(ui->find_entry), 0, -1);
	const gchar *text = gtk_entry_get_text(GTK_ENTRY(ui->find_entry));
	if (*text != '\0')
		wv_host_find(st->browser->host, text);   /* re-highlight previous term */
}

static void on_find_changed(GtkSearchEntry *entry, gpointer user)
{
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_find(st->browser->host, gtk_entry_get_text(GTK_ENTRY(entry)));
}

static void on_find_activate(GtkEntry *entry, gpointer user)
{
	(void) entry;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_find_next(st->browser->host, TRUE);
}

static void on_find_next(GtkWidget *w, gpointer user)
{
	(void) w;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_find_next(st->browser->host, TRUE);
}

static void on_find_prev(GtkWidget *w, gpointer user)
{
	(void) w;
	GwvState *st = user;
	if (st->browser != NULL)
		wv_host_find_next(st->browser->host, FALSE);
}

/* GtkSearchEntry emits stop-search on Escape. */
static void on_find_stop_search(GtkSearchEntry *entry, gpointer user)
{
	(void) entry;
	findbar_close(user);
}

static void on_find_close_clicked(GtkButton *btn, gpointer user)
{
	(void) btn;
	findbar_close(user);
}

/* Match count from the engine -> the little label. */
static void on_view_find_matches(GwvView *v, guint count)
{
	BrowserUi *ui = v->view_data;
	if (ui == NULL || ui->match_label == NULL)
		return;
	const gchar *text = gtk_entry_get_text(GTK_ENTRY(ui->find_entry));
	if (*text == '\0') {
		gtk_label_set_text(GTK_LABEL(ui->match_label), "");
	} else if (count == 0) {
		gtk_label_set_text(GTK_LABEL(ui->match_label), _("No matches"));
	} else {
		gchar *s = g_strdup_printf(g_dngettext(NULL, "%u match", "%u matches",
		                                       count), count);
		gtk_label_set_text(GTK_LABEL(ui->match_label), s);
		g_free(s);
	}
}

/* Geany's keybinding handler on the main window would eat Ctrl+F (Find) and
 * Escape before the pane ever sees them — same story as the terminal, same
 * cure: a snooper that acts only while focus is inside the browser panel.
 * Everything else passes through, so Geany's other bindings keep working. */
static gint browser_key_snooper(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	GwvState *st = data;
	GwvView  *v = st->browser;
	if (v == NULL || v->panel == NULL || event->type != GDK_KEY_PRESS)
		return FALSE;
	GtkWidget *top = gtk_widget_get_toplevel(widget);
	if (!GTK_IS_WINDOW(top))
		return FALSE;
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(top));
	if (focus == NULL ||
	    (focus != v->panel && !gtk_widget_is_ancestor(focus, v->panel)))
		return FALSE;

	if ((event->state & GDK_CONTROL_MASK) &&
	    (event->keyval == GDK_KEY_f || event->keyval == GDK_KEY_F)) {
		findbar_open(st);
		return TRUE;
	}
	BrowserUi *ui = v->view_data;
	if (event->keyval == GDK_KEY_Escape && ui != NULL && ui->findbar != NULL &&
	    gtk_widget_get_visible(ui->findbar)) {
		findbar_close(st);
		return TRUE;
	}
	return FALSE;
}

/* Reflect the configured home page in the Home button's tooltip (also called
 * from settings_apply, so it stays truthful when the setting changes). */
void gwv_browser_sync_home(GwvState *st)
{
	BrowserUi *ui = (st->browser != NULL) ? st->browser->view_data : NULL;
	if (ui == NULL || ui->home_btn == NULL)
		return;
	gchar *tip = g_strdup_printf(
		_("Home page: %s\n(set it in the plugin preferences)"),
		browser_home_url(st));
	gtk_widget_set_tooltip_text(ui->home_btn, tip);
	g_free(tip);
}

/* ------------------------------- lifecycle ------------------------------- */

static GtkWidget *nav_button(const char *icon, const char *tip,
                             GCallback cb, gpointer user)
{
	GtkWidget *btn = gtk_button_new_from_icon_name(icon, GTK_ICON_SIZE_SMALL_TOOLBAR);
	gtk_button_set_relief(GTK_BUTTON(btn), GTK_RELIEF_NONE);
	gtk_widget_set_tooltip_text(btn, tip);
	g_signal_connect(btn, "clicked", cb, user);
	return btn;
}

void gwv_browser_create(GwvState *st)
{
	if (st->browser != NULL)
		return;
	WvHostConfig cfg = { GWV_VIRTUAL_HOST, NULL, TRUE };   /* no bridge; browse */
	st->browser = gwv_view_new_full(st,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook),
		_(GWV_BROWSER_LABEL), &cfg, browser_home_url(st),
		FALSE /* lazy: no web process until the tab is shown */, FALSE);
	if (st->browser->webarea == NULL)
		return;                     /* runtime missing — message already shown */

	BrowserUi *ui = g_new0(BrowserUi, 1);
	st->browser->view_data = ui;
	st->browser->on_url_changed = on_url_changed;

	GtkWidget *tb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	g_object_set(tb, "margin", 2, NULL);
	gtk_box_pack_start(GTK_BOX(tb),
		nav_button("go-previous-symbolic", _("Back"),
		           G_CALLBACK(on_nav_back), st), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tb),
		nav_button("go-next-symbolic", _("Forward"),
		           G_CALLBACK(on_nav_forward), st), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tb),
		nav_button("view-refresh-symbolic", _("Reload"),
		           G_CALLBACK(on_nav_reload), st), FALSE, FALSE, 0);
	ui->home_btn = nav_button("go-home-symbolic", _("Home page"),
	                          G_CALLBACK(on_nav_home), st);
	gtk_box_pack_start(GTK_BOX(tb), ui->home_btn, FALSE, FALSE, 0);

	ui->entry = gtk_entry_new();
	gtk_entry_set_placeholder_text(GTK_ENTRY(ui->entry),
		_("Address — e.g. localhost:5173 or a docs URL"));
	gtk_entry_set_input_purpose(GTK_ENTRY(ui->entry), GTK_INPUT_PURPOSE_URL);
	gtk_widget_set_hexpand(ui->entry, TRUE);
	g_signal_connect(ui->entry, "activate", G_CALLBACK(on_entry_activate), st);
	gtk_box_pack_start(GTK_BOX(tb), ui->entry, TRUE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(st->browser->panel), tb, FALSE, FALSE, 0);
	gtk_box_reorder_child(GTK_BOX(st->browser->panel), tb, 0);
	gtk_widget_show_all(tb);

	/* Find-in-page bar (engines without a built-in one), hidden until Ctrl+F. */
	if (wv_host_needs_find_ui()) {
		GtkWidget *fb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
		g_object_set(fb, "margin", 2, NULL);
		ui->findbar = fb;

		ui->find_entry = gtk_search_entry_new();
		gtk_widget_set_hexpand(ui->find_entry, TRUE);
		g_signal_connect(ui->find_entry, "search-changed",
		                 G_CALLBACK(on_find_changed), st);
		g_signal_connect(ui->find_entry, "activate",
		                 G_CALLBACK(on_find_activate), st);
		g_signal_connect(ui->find_entry, "next-match",
		                 G_CALLBACK(on_find_next), st);
		g_signal_connect(ui->find_entry, "previous-match",
		                 G_CALLBACK(on_find_prev), st);
		g_signal_connect(ui->find_entry, "stop-search",
		                 G_CALLBACK(on_find_stop_search), st);
		gtk_box_pack_start(GTK_BOX(fb), ui->find_entry, TRUE, TRUE, 0);

		gtk_box_pack_start(GTK_BOX(fb),
			nav_button("go-up-symbolic", _("Previous match (Shift+Enter, Ctrl+Shift+G)"),
			           G_CALLBACK(on_find_prev), st), FALSE, FALSE, 0);
		gtk_box_pack_start(GTK_BOX(fb),
			nav_button("go-down-symbolic", _("Next match (Enter, Ctrl+G)"),
			           G_CALLBACK(on_find_next), st), FALSE, FALSE, 0);

		ui->match_label = gtk_label_new("");
		gtk_widget_set_sensitive(ui->match_label, FALSE);   /* dim */
		gtk_box_pack_start(GTK_BOX(fb), ui->match_label, FALSE, FALSE, 4);

		gtk_box_pack_start(GTK_BOX(fb),
			nav_button("window-close-symbolic", _("Close (Escape)"),
			           G_CALLBACK(on_find_close_clicked), st), FALSE, FALSE, 0);

		gtk_box_pack_start(GTK_BOX(st->browser->panel), fb, FALSE, FALSE, 0);
		gtk_box_reorder_child(GTK_BOX(st->browser->panel), fb, 1);
		gtk_widget_show_all(fb);
		gtk_widget_hide(fb);
		gtk_widget_set_no_show_all(fb, TRUE);   /* survive panel show_all */

		st->browser->on_find_matches = on_view_find_matches;
		if (st->browser_snooper == 0) {
			G_GNUC_BEGIN_IGNORE_DEPRECATIONS
			st->browser_snooper = gtk_key_snooper_install(browser_key_snooper, st);
			G_GNUC_END_IGNORE_DEPRECATIONS
		}
	}

	gwv_browser_sync_home(st);
}

void gwv_browser_destroy(GwvState *st)
{
	if (st->browser == NULL)
		return;
	if (st->browser_snooper != 0) {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		gtk_key_snooper_remove(st->browser_snooper);
		G_GNUC_END_IGNORE_DEPRECATIONS
		st->browser_snooper = 0;
	}
	g_clear_pointer(&st->browser->view_data, g_free);   /* widgets die with panel */
	gwv_view_free(st->browser);
	st->browser = NULL;
}
