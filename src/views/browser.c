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
#include "findbar.h"

typedef struct {
	GtkWidget *entry;       /* address bar */
	GtkWidget *home_btn;    /* tooltip shows the configured home page */
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

void gwv_browser_create(GwvState *st)
{
	if (st->browser != NULL)
		return;
	/* No bridge; free browsing (which implies find-in-page). */
	WvHostConfig cfg = { GWV_VIRTUAL_HOST, NULL, TRUE, FALSE };
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
		gwv_icon_button("go-previous-symbolic", _("Back"),
		                G_CALLBACK(on_nav_back), st), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tb),
		gwv_icon_button("go-next-symbolic", _("Forward"),
		                G_CALLBACK(on_nav_forward), st), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(tb),
		gwv_icon_button("view-refresh-symbolic", _("Reload"),
		                G_CALLBACK(on_nav_reload), st), FALSE, FALSE, 0);
	ui->home_btn = gwv_icon_button("go-home-symbolic", _("Home page"),
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

	gwv_findbar_attach(st->browser, 1);   /* under the address bar */

	gwv_browser_sync_home(st);
}

void gwv_browser_destroy(GwvState *st)
{
	if (st->browser == NULL)
		return;
	g_clear_pointer(&st->browser->view_data, g_free);   /* widgets die with panel */
	gwv_view_free(st->browser);
	st->browser = NULL;
}
