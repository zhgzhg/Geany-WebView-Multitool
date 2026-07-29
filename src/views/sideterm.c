/*
 * views/sideterm.c — a terminal pane side-by-side with (right of) the editor
 * area, in the spirit of the Split Window plugin.
 *
 * Geany's layout: hpaned1 = sidebar | <editor area>, where <editor area> is
 * the documents notebook — or the Split Window plugin's own paned once it has
 * split. We wrap whatever CURRENTLY occupies that slot in a fresh horizontal
 * paned, with the terminal on the right. That single choice makes the two
 * plugins compose in either activation order:
 *
 *   - we split first: Split Window wraps the notebook inside our LEFT slot
 *     -> (editor | split view) | terminal
 *   - Split Window first: we wrap its whole pane
 *     -> (editor | split view) | terminal
 *
 * Both its split and unsplit vacate a paned slot before re-adding
 * (ref/remove/destroy/add — geany/plugins/splitwindow.c), and both plugins
 * re-resolve parents fresh, so deactivation in any order restores cleanly.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "views/sideterm.h"
#include "views/terminal.h"
#include "view.h"

/* The drag handle on the terminal's left edge is the paned separator, a hairline
 * under most themes — a fussy target to catch with the mouse. Give it a floor
 * wide enough to grab, plus wide-handle so themes draw it as a deliberate grip
 * rather than stretching a line. Scoped by a style class, so the rule touches
 * only our own paned and leaves the rest of Geany's layout alone; the provider
 * must be screen-wide (one added to the widget's own style context does not
 * reach child CSS nodes such as `separator`) and so is dropped again when the
 * pane goes away. min-width only ever raises the size: under a theme that
 * already gives this much or more — Geany's Windows theme is itself 7px — the
 * rule is a silent no-op and the handle keeps the theme's width. */
#define GWV_SIDE_PANE_CLASS "gwv-side-pane"
#define GWV_SIDE_HANDLE_PX  7

static GtkCssProvider *side_handle_css = NULL;

static void side_handle_css_add(GtkWidget *pane)
{
	GdkScreen *screen = gtk_widget_get_screen(pane);

	gtk_style_context_add_class(gtk_widget_get_style_context(pane),
	                            GWV_SIDE_PANE_CLASS);
	/* wide-handle makes themes draw the separator as a deliberate grip rather
	 * than stretching a hairline; our min-width then sets the exact size. */
	gtk_paned_set_wide_handle(GTK_PANED(pane), TRUE);

	if (side_handle_css != NULL || screen == NULL)
		return;
	gchar *rules = g_strdup_printf("paned." GWV_SIDE_PANE_CLASS
	                               " > separator { min-width: %dpx; }",
	                               GWV_SIDE_HANDLE_PX);
	side_handle_css = gtk_css_provider_new();
	gtk_css_provider_load_from_data(side_handle_css, rules, -1, NULL);
	gtk_style_context_add_provider_for_screen(screen,
	                                          GTK_STYLE_PROVIDER(side_handle_css),
	                                          GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	g_free(rules);
}

static void side_handle_css_remove(GtkWidget *pane)
{
	GdkScreen *screen = (pane != NULL) ? gtk_widget_get_screen(pane) : NULL;

	if (side_handle_css == NULL)
		return;
	if (screen != NULL)
		gtk_style_context_remove_provider_for_screen(screen,
		                                             GTK_STYLE_PROVIDER(side_handle_css));
	g_clear_object(&side_handle_css);
}

/* Give the terminal ~1/3 of the editor area once the paned has a size (it is
 * created before the first allocation when enabled at startup). One-shot. */
static void on_pane_size_allocate(GtkWidget *pane, GdkRectangle *alloc, gpointer user)
{
	(void) user;
	if (alloc->width <= 1)
		return;
	gtk_paned_set_position(GTK_PANED(pane), MAX(200, alloc->width * 2 / 3));
	g_signal_handlers_disconnect_by_func(pane, (gpointer) on_pane_size_allocate, user);
}

/* The paned holding sidebar | editor-area, by its stable glade name. */
static GtkPaned *editor_hpaned(GwvState *st)
{
	GtkWidget *w = ui_lookup_widget(st->plugin->geany_data->main_widgets->window,
	                                "hpaned1");
	if (!GTK_IS_PANED(w)) {
		g_warning("GWV: hpaned1 not found — side terminal unavailable");
		return NULL;
	}
	return GTK_PANED(w);
}

void gwv_sideterm_create(GwvState *st)
{
	if (st->sideterm != NULL)
		return;
	GtkPaned *hp = editor_hpaned(st);
	if (hp == NULL)
		return;
	GtkWidget *area = gtk_paned_get_child2(GTK_PANED(hp));
	if (area == NULL) {
		g_warning("GWV: editor area slot is empty — side terminal unavailable");
		return;
	}

	/* Unattached terminal view; its panel becomes the paned's right side. */
	st->sideterm = gwv_terminal_new_view(st, NULL);

	GtkWidget *pane = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
	g_object_ref(area);
	gtk_container_remove(GTK_CONTAINER(hp), area);
	gtk_paned_pack2(GTK_PANED(hp), pane, TRUE, TRUE);
	gtk_paned_pack1(GTK_PANED(pane), area, TRUE, FALSE);
	g_object_unref(area);
	gtk_paned_pack2(GTK_PANED(pane), st->sideterm->panel, FALSE, TRUE);
	st->side_pane = pane;
	side_handle_css_add(pane);

	g_signal_connect(pane, "size-allocate", G_CALLBACK(on_pane_size_allocate), st);
	gtk_widget_show_all(pane);
	/* The panel is mapped right away, so the lazy first-load fires now and the
	 * shell spawns immediately — it is visible, that is what the user asked. */

	gwv_terminal_snooper_sync(st);
}

void gwv_sideterm_destroy(GwvState *st)
{
	if (st->sideterm == NULL)
		return;
	GtkWidget *pane = st->side_pane;
	GtkWidget *hp = (pane != NULL) ? gtk_widget_get_parent(pane) : NULL;

	gwv_view_free(st->sideterm);      /* destroys the panel (pane's child2) */
	st->sideterm = NULL;
	gwv_terminal_snooper_sync(st);

	if (pane == NULL)
		return;
	side_handle_css_remove(pane);     /* while pane still has a screen */
	st->side_pane = NULL;
	/* Whatever occupies our left slot now (the notebook, or Split Window's
	 * paned) goes back where the editor area was. */
	GtkWidget *area = gtk_paned_get_child1(GTK_PANED(pane));
	if (area != NULL) {
		g_object_ref(area);
		gtk_container_remove(GTK_CONTAINER(pane), area);
		gtk_widget_destroy(pane);
		if (hp != NULL)
			gtk_paned_pack2(GTK_PANED(hp), area, TRUE, TRUE);
		g_object_unref(area);
	} else {
		gtk_widget_destroy(pane);
	}
}
