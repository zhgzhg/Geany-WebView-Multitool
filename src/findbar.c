/*
 * findbar.c — shared find-in-page bar (Ctrl+F) for WebView panes, built only
 * where the backend has no find UI of its own (WebKitGTK; on Windows Ctrl+F
 * is a browser accelerator and WebView2 shows the engine's bar). Drives the
 * portable wv_host_find*() API; match counts arrive via on_find_matches.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "findbar.h"
#include "view.h"

typedef struct {
	GwvView   *v;
	GtkWidget *bar;
	GtkWidget *entry;
	GtkWidget *match_label;
	guint      snooper;
} GwvFindBar;

static void findbar_close(GwvFindBar *fb)
{
	if (fb->bar == NULL || !gtk_widget_get_visible(fb->bar))
		return;
	wv_host_find_stop(fb->v->host);
	gtk_widget_hide(fb->bar);
	wv_host_focus(fb->v->host);
}

static void findbar_open(GwvFindBar *fb)
{
	gtk_widget_show(fb->bar);
	gtk_widget_grab_focus(fb->entry);
	gtk_editable_select_region(GTK_EDITABLE(fb->entry), 0, -1);
	const gchar *text = gtk_entry_get_text(GTK_ENTRY(fb->entry));
	if (*text != '\0')
		wv_host_find(fb->v->host, text);   /* re-highlight previous term */
}

static void on_find_changed(GtkSearchEntry *entry, gpointer user)
{
	GwvFindBar *fb = user;
	wv_host_find(fb->v->host, gtk_entry_get_text(GTK_ENTRY(entry)));
}

static void on_find_activate(GtkEntry *entry, gpointer user)
{
	(void) entry;
	GwvFindBar *fb = user;
	wv_host_find_next(fb->v->host, TRUE);
}

static void on_find_next(GtkWidget *w, gpointer user)
{
	(void) w;
	GwvFindBar *fb = user;
	wv_host_find_next(fb->v->host, TRUE);
}

static void on_find_prev(GtkWidget *w, gpointer user)
{
	(void) w;
	GwvFindBar *fb = user;
	wv_host_find_next(fb->v->host, FALSE);
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
	GwvFindBar *fb = v->findbar;
	if (fb == NULL || fb->match_label == NULL)
		return;
	const gchar *text = gtk_entry_get_text(GTK_ENTRY(fb->entry));
	if (*text == '\0') {
		gtk_label_set_text(GTK_LABEL(fb->match_label), "");
	} else if (count == 0) {
		gtk_label_set_text(GTK_LABEL(fb->match_label), _("No matches"));
	} else {
		gchar *s = g_strdup_printf(g_dngettext(NULL, "%u match", "%u matches",
		                                       count), count);
		gtk_label_set_text(GTK_LABEL(fb->match_label), s);
		g_free(s);
	}
}

/* Geany's keybinding handler on the main window would eat Ctrl+F (Find) and
 * Escape before the pane ever sees them — same story as the terminal, same
 * cure: a snooper that acts only while focus is inside this view's panel.
 * Everything else passes through, so Geany's other bindings keep working. */
static gint findbar_key_snooper(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	GwvFindBar *fb = data;
	GwvView *v = fb->v;
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
		findbar_open(fb);
		return TRUE;
	}
	if (event->keyval == GDK_KEY_Escape && gtk_widget_get_visible(fb->bar)) {
		findbar_close(fb);
		return TRUE;
	}
	return FALSE;
}

void gwv_findbar_attach(GwvView *v, gint position)
{
	if (v == NULL || v->webarea == NULL || v->findbar != NULL ||
	    !wv_host_needs_find_ui())
		return;

	GwvFindBar *fb = g_new0(GwvFindBar, 1);
	fb->v = v;
	v->findbar = fb;

	fb->bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
	g_object_set(fb->bar, "margin", 2, NULL);

	fb->entry = gtk_search_entry_new();
	gtk_widget_set_hexpand(fb->entry, TRUE);
	g_signal_connect(fb->entry, "search-changed",
	                 G_CALLBACK(on_find_changed), fb);
	g_signal_connect(fb->entry, "activate",
	                 G_CALLBACK(on_find_activate), fb);
	g_signal_connect(fb->entry, "next-match",
	                 G_CALLBACK(on_find_next), fb);
	g_signal_connect(fb->entry, "previous-match",
	                 G_CALLBACK(on_find_prev), fb);
	g_signal_connect(fb->entry, "stop-search",
	                 G_CALLBACK(on_find_stop_search), fb);
	gtk_box_pack_start(GTK_BOX(fb->bar), fb->entry, TRUE, TRUE, 0);

	gtk_box_pack_start(GTK_BOX(fb->bar),
		gwv_icon_button("go-up-symbolic", _("Previous match (Shift+Enter, Ctrl+Shift+G)"),
		                G_CALLBACK(on_find_prev), fb), FALSE, FALSE, 0);
	gtk_box_pack_start(GTK_BOX(fb->bar),
		gwv_icon_button("go-down-symbolic", _("Next match (Enter, Ctrl+G)"),
		                G_CALLBACK(on_find_next), fb), FALSE, FALSE, 0);

	fb->match_label = gtk_label_new("");
	gtk_widget_set_sensitive(fb->match_label, FALSE);   /* dim */
	gtk_box_pack_start(GTK_BOX(fb->bar), fb->match_label, FALSE, FALSE, 4);

	gtk_box_pack_start(GTK_BOX(fb->bar),
		gwv_icon_button("window-close-symbolic", _("Close (Escape)"),
		                G_CALLBACK(on_find_close_clicked), fb), FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(v->panel), fb->bar, FALSE, FALSE, 0);
	gtk_box_reorder_child(GTK_BOX(v->panel), fb->bar, position);
	gtk_widget_show_all(fb->bar);
	gtk_widget_hide(fb->bar);
	gtk_widget_set_no_show_all(fb->bar, TRUE);   /* survive panel show_all */

	v->on_find_matches = on_view_find_matches;
	G_GNUC_BEGIN_IGNORE_DEPRECATIONS
	fb->snooper = gtk_key_snooper_install(findbar_key_snooper, fb);
	G_GNUC_END_IGNORE_DEPRECATIONS
}

void gwv_findbar_free(GwvView *v)
{
	GwvFindBar *fb = (v != NULL) ? v->findbar : NULL;
	if (fb == NULL)
		return;
	if (fb->snooper != 0) {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		gtk_key_snooper_remove(fb->snooper);
		G_GNUC_END_IGNORE_DEPRECATIONS
	}
	v->findbar = NULL;
	g_free(fb);      /* the widgets die with v->panel */
}
