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
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "views/sideterm.h"
#include "views/terminal.h"
#include "view.h"

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
