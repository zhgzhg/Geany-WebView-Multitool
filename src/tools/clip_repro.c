/*
 * clip_repro.c — standalone reproduction of the WebView2 + GTK clipboard crash.
 *
 * Creates a live WebView2 in-process (via the real wv_host_new backend) parented
 * to a GTK window, then hammers the GTK clipboard (set + read back), mimicking a
 * user's repeated editor copy -> paste -> copy-longer churn. Unlike Geany — which
 * self-exits in a non-interactive session — this owns its gtk_main and stays
 * alive, so a debugger can attach AFTER startup (avoiding the NT debug-heap
 * artifact) and catch the genuine crash.
 *
 * Usage: clip_repro.exe <asset_root>   (asset_root = installed plugin asset dir)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <gtk/gtk.h>
#include "host/wvhost.h"

static int iter = 0;

/* One-shot: do a single copy(+paste) cycle, then reschedule AFTER it completes.
 * Non-reentrant on purpose — wait_for_text() nested-loops, and a repeating timer
 * would fire inside it, a reentrancy real editor copy/paste never produces. */
static gboolean stress_cb(gpointer user)
{
	(void) user;
	iter++;
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gchar *s = g_strnfill((gsize) (48 + iter * 40), (gchar) ('A' + (iter % 20)));
	gtk_clipboard_set_text(cb, s, -1);              /* "copy" */
	g_free(s);
	if (g_getenv("GWV_SET_ONLY") == NULL) {
		gchar *got = gtk_clipboard_wait_for_text(cb);   /* "paste" (read back) */
		g_free(got);
	}
	g_print("stress iter %d\n", iter);
	fflush(stdout);
	g_timeout_add(600, stress_cb, NULL);            /* reschedule after completion */
	return G_SOURCE_REMOVE;
}

static gboolean stress_start(gpointer user)
{
	(void) user;
	g_print("=== starting clipboard stress ===\n");
	fflush(stdout);
	g_timeout_add(600, stress_cb, NULL);
	return G_SOURCE_REMOVE;
}

static void on_ready(WvHost *host, gpointer user)
{
	(void) host; (void) user;
	g_print("=== webview ready ===\n");
	fflush(stdout);
	/* Delay so a debugger can attach after startup before the churn begins. */
	g_timeout_add(8000, stress_start, NULL);
}

int main(int argc, char **argv)
{
	gtk_init(&argc, &argv);

	/* --no-webview: churn the clipboard with NO WebView2 in the process, to
	 * prove whether the crash needs the webview or is a pure GTK-win32 bug. */
	gboolean no_webview = (argc > 1 && g_strcmp0(argv[1], "--no-webview") == 0);
	if (no_webview) {
		GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
		gtk_widget_show_all(w);
		g_print("=== clip_repro (NO webview); stress in 8s ===\n");
		fflush(stdout);
		g_timeout_add(8000, stress_start, NULL);
		gtk_main();
		return 0;
	}

	const char *asset_root = (argc > 1) ? argv[1] : NULL;
	gchar *bridge_js = NULL;
	if (asset_root != NULL) {
		gchar *bp = g_build_filename(asset_root, "bridge.js", NULL);
		g_file_get_contents(bp, &bridge_js, NULL, NULL);
		g_free(bp);
	}

	GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_default_size(GTK_WINDOW(win), 900, 600);
	GtkWidget *area = gtk_drawing_area_new();
	gtk_container_add(GTK_CONTAINER(win), area);
	gtk_widget_show_all(win);

	WvHostConfig    cfg = { "geanyview.local", bridge_js };
	(void) asset_root;   /* assets embedded since M6; argv dir feeds bridge_js only */
	WvHostCallbacks cb  = { on_ready, NULL, NULL };
	WvHost *host = wv_host_new(area, &cfg, &cb, NULL);

	if (asset_root != NULL)
		wv_host_navigate(host, "https://geanyview.local/preview/index.html");
	else
		wv_host_navigate(host, "about:blank");

	g_print("=== clip_repro up; stress begins ~8s after webview ready ===\n");
	fflush(stdout);

	gtk_main();
	return 0;
}
