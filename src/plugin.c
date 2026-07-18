/*
 * plugin.c — Geany WebView: reusable WebView host panes for Geany 2.x.
 *
 * Registers the modern GeanyPlugin API and creates two views, each a WvHost
 * serving the bundled assets/ over https://geanyview.local/ with an injected JS
 * bridge:
 *   - "WebView"  in the sidebar          (hello demo view)
 *   - "Terminal" in the message window   (xterm.js over a ConPTY shell)
 *
 * The terminal view bridges pty.* channels to the PTY service (services/pty.h).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <geanyplugin.h>

#include "wvhost.h"
#include "bridge.h"
#include "util.h"
#include "services/pty.h"

#define GWV_VIRTUAL_HOST "geanyview.local"
#define GWV_ASSET_SUBDIR "geanywebview"
#define GWV_WEBVIEW2_URL "https://developer.microsoft.com/microsoft-edge/webview2/"

typedef struct {
	GeanyPlugin *plugin;
	GtkWidget   *panel;      /* notebook page (a GtkBox)                    */
	GtkWidget   *webarea;    /* GtkDrawingArea the browser is parented onto */
	WvHost      *host;
	Bridge      *bridge;
	Pty         *pty;        /* terminal view only; NULL otherwise          */
} GwvView;

typedef struct {
	GeanyPlugin *plugin;
	gchar       *asset_root; /* local folder served at the virtual host     */
	gchar       *bridge_js;  /* injected shim contents                       */
	GwvView     *hello;
	GwvView     *terminal;
	GtkWidget   *menu_webview;
	GtkWidget   *menu_terminal;
} GwvState;

/* Single-instance plugin; keybinding callbacks (which get no user data) reach
 * state through this. */
static GwvState *g_gwv_state = NULL;

enum { KB_FOCUS_TERMINAL, KB_FOCUS_WEBVIEW, KB_COUNT };

/* ------------------------------------------------------------ messages ui */

static GtkWidget *make_message_widget(const char *text, gboolean install_link)
{
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
	g_object_set(box, "margin", 16, NULL);

	GtkWidget *lbl = gtk_label_new(text);
	gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
	gtk_label_set_xalign(GTK_LABEL(lbl), 0.0);
	gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);

	if (install_link) {
		GtkWidget *link = gtk_link_button_new_with_label(
			GWV_WEBVIEW2_URL, _("Install the WebView2 Runtime"));
		gtk_widget_set_halign(link, GTK_ALIGN_START);
		gtk_box_pack_start(GTK_BOX(box), link, FALSE, FALSE, 0);
	}
	return box;
}

static void show_view_message(GwvView *v, const char *text, gboolean install_link)
{
	GtkWidget *msg = make_message_widget(text, install_link);
	gtk_box_pack_start(GTK_BOX(v->panel), msg, FALSE, FALSE, 0);
	gtk_widget_show_all(v->panel);
}

/* -------------------------------------------------------------- host cbs */

static void on_host_ready(WvHost *host, gpointer user)
{
	(void) host; (void) user;
	g_debug("GWV: on_host_ready");
}

static void on_host_message(WvHost *host, const char *json, gpointer user)
{
	(void) host;
	GwvView *v = user;
	if (v->bridge != NULL)
		bridge_handle(v->bridge, json);
}

static void on_host_failed(WvHost *host, const char *error, gpointer user)
{
	(void) host;
	GwvView *v = user;
	g_warning("GWV: on_host_failed: %s", error ? error : "(null)");
	if (v->webarea != NULL)
		gtk_widget_hide(v->webarea);
	gchar *msg = g_strdup_printf(_("WebView failed to start: %s"),
	                             error ? error : _("unknown error"));
	GtkWidget *w = make_message_widget(msg, TRUE);
	gtk_box_pack_start(GTK_BOX(v->panel), w, FALSE, FALSE, 0);
	gtk_widget_show_all(w);
	g_free(msg);
}

/* --------------------------------------------------------- common bridge */

static void on_ch_ready(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) user;
	g_debug("GWV: view sys.ready %s", payload);
}

/* --------------------------------------------------------- terminal glue */

static gchar *resolve_shell(void)
{
	gchar *p;
	if ((p = g_find_program_in_path("pwsh.exe")) != NULL)       return p;
	if ((p = g_find_program_in_path("powershell.exe")) != NULL) return p;
	if ((p = g_find_program_in_path("cmd.exe")) != NULL)        return p;
	return g_strdup("cmd.exe");
}

static gchar *current_doc_dir(void)
{
	GeanyDocument *doc = document_get_current();
	if (doc != NULL && doc->file_name != NULL)
		return g_path_get_dirname(doc->file_name);
	return g_strdup(g_get_home_dir());
}

/* PTY -> page */
static void on_pty_data(Pty *pty, const char *bytes, gsize len, gpointer user)
{
	(void) pty;
	GwvView *v = user;
	gchar *b64 = g_base64_encode((const guchar *) bytes, len);
	gchar *payload = g_strdup_printf("\"%s\"", b64);
	bridge_post(v->bridge, "pty.data", payload);
	g_free(payload);
	g_free(b64);
}

static void on_pty_exit(Pty *pty, int code, gpointer user)
{
	(void) pty;
	GwvView *v = user;
	gchar *payload = g_strdup_printf("{\"code\":%d}", code);
	bridge_post(v->bridge, "pty.exit", payload);
	g_free(payload);
}

/* page -> PTY */
static void on_ch_pty_start(Bridge *bridge, const char *payload, gpointer user)
{
	GwvView *v = user;
	int cols = 80, rows = 24;
	bridge_payload_get_int(payload, "cols", &cols);
	bridge_payload_get_int(payload, "rows", &rows);

	if (v->pty != NULL) {          /* restart */
		pty_free(v->pty);
		v->pty = NULL;
	}
	gchar *shell = resolve_shell();
	gchar *cwd   = current_doc_dir();
	PtyCallbacks pcb = { on_pty_data, on_pty_exit };
	v->pty = pty_spawn(shell, cwd, cols, rows, &pcb, v);
	g_debug("GWV: pty.start shell=%s cwd=%s %dx%d -> %s",
	        shell, cwd, cols, rows, v->pty ? "ok" : "FAILED");
	g_free(shell);
	g_free(cwd);
	if (v->pty == NULL)
		bridge_post(bridge, "pty.exit", "{\"code\":-1}");
}

static void on_ch_pty_data(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvView *v = user;
	gchar *b64 = bridge_payload_string(payload);
	if (b64 != NULL && v->pty != NULL) {
		gsize len = 0;
		guchar *bytes = g_base64_decode(b64, &len);
		pty_write(v->pty, (const char *) bytes, len);
		g_free(bytes);
	}
	g_free(b64);
}

static void on_ch_pty_resize(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvView *v = user;
	int cols = 0, rows = 0;
	if (v->pty != NULL &&
	    bridge_payload_get_int(payload, "cols", &cols) &&
	    bridge_payload_get_int(payload, "rows", &rows))
		pty_resize(v->pty, cols, rows);
}

/* Escape chord from the terminal: move keyboard focus to the editor. */
static void on_ch_focus_editor(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) payload; (void) user;
	keybindings_send_command(GEANY_KEY_GROUP_FOCUS, GEANY_KEYS_FOCUS_EDITOR);
}

/* ------------------------------------------------------------- view mgmt */

static GwvView *gwv_view_new(GwvState *st, GtkNotebook *notebook,
                             const char *label, const char *view_path,
                             gboolean eager)
{
	GwvView *v = g_new0(GwvView, 1);
	v->plugin = st->plugin;

	v->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_notebook_append_page(notebook, v->panel, gtk_label_new(label));
	gtk_widget_show_all(v->panel);

	if (!gwv_os_supported()) {
		show_view_message(v,
			_("Geany WebView requires Windows 10 version 1809 (build 17763) or newer."),
			FALSE);
		return v;
	}
	char *ver = NULL;
	gboolean avail = wv_host_runtime_available(&ver);
	if (!avail) {
		gchar *msg = g_strdup_printf(
			_("The Microsoft Edge WebView2 Runtime is required but was not found.\n(%s)"),
			ver ? ver : _("not detected"));
		show_view_message(v, msg, TRUE);
		g_free(msg);
		g_free(ver);
		return v;
	}
	g_free(ver);

	v->webarea = gtk_drawing_area_new();
	gtk_widget_set_hexpand(v->webarea, TRUE);
	gtk_widget_set_vexpand(v->webarea, TRUE);
	gtk_box_pack_start(GTK_BOX(v->panel), v->webarea, TRUE, TRUE, 0);
	gtk_widget_show_all(v->panel);

	WvHostConfig    cfg = { GWV_VIRTUAL_HOST, st->asset_root, st->bridge_js };
	WvHostCallbacks cb  = { on_host_ready, on_host_message, on_host_failed };
	v->host = wv_host_new(v->webarea, &cfg, &cb, v);

	v->bridge = bridge_new(v->host);
	bridge_on(v->bridge, "sys.ready", on_ch_ready, v);

	gchar *url = g_strdup_printf("https://" GWV_VIRTUAL_HOST "/%s", view_path);
	wv_host_navigate(v->host, url);
	g_free(url);

	/* Eager views pre-initialize now (instant open); lazy views wait until the
	 * pane is first shown (so a terminal shell isn't spawned until opened). */
	if (eager)
		gtk_widget_realize(v->webarea);

	return v;
}

static void gwv_view_reveal(GwvView *v, GtkNotebook *notebook)
{
	if (v == NULL)
		return;
	gint num = gtk_notebook_page_num(notebook, v->panel);
	if (num >= 0)
		gtk_notebook_set_current_page(notebook, num);
	/* Bring it up if it was created lazily. */
	if (v->webarea != NULL && !gtk_widget_get_realized(v->webarea))
		gtk_widget_realize(v->webarea);
	if (v->host != NULL)
		wv_host_focus(v->host);
}

static void gwv_view_free(GwvView *v)
{
	if (v == NULL)
		return;
	if (v->pty != NULL) {
		pty_free(v->pty);
		v->pty = NULL;
	}
	if (v->bridge != NULL) {
		bridge_free(v->bridge);
		v->bridge = NULL;
	}
	if (v->host != NULL) {
		wv_host_destroy(v->host);
		v->host = NULL;
	}
	if (v->panel != NULL)
		gtk_widget_destroy(v->panel);
	g_free(v);
}

/* --------------------------------------------------------------- menus */

static void on_menu_webview(GtkMenuItem *item, gpointer user)
{
	(void) item;
	GwvState *st = user;
	gwv_view_reveal(st->hello,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook));
}

static void on_menu_terminal(GtkMenuItem *item, gpointer user)
{
	(void) item;
	GwvState *st = user;
	GtkWidget *nb = st->plugin->geany_data->main_widgets->message_window_notebook;
	/* Show the message window if it's currently hidden (toggle is a no-op-safe
	 * only when hidden, so guard on mapped state). */
	if (!gtk_widget_get_mapped(nb))
		keybindings_send_command(GEANY_KEY_GROUP_VIEW, GEANY_KEYS_VIEW_MESSAGEWINDOW);
	gwv_view_reveal(st->terminal, GTK_NOTEBOOK(nb));
}

static void kb_focus_terminal(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		on_menu_terminal(NULL, g_gwv_state);
}

static void kb_focus_webview(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		on_menu_webview(NULL, g_gwv_state);
}


/* ------------------------------------------------------------- plugin funcs */

static gboolean gwv_init(GeanyPlugin *plugin, gpointer pdata)
{
	(void) pdata;
	GeanyData *geany_data = plugin->geany_data;
	GwvState  *st = g_new0(GwvState, 1);
	st->plugin = plugin;

	/* Shared assets: the plugin's install dir + the injected bridge shim. */
	gchar *dir = gwv_plugin_dir();
	st->asset_root = g_build_filename(dir, GWV_ASSET_SUBDIR, NULL);
	g_free(dir);
	gchar *bridge_path = g_build_filename(st->asset_root, "bridge.js", NULL);
	if (!g_file_get_contents(bridge_path, &st->bridge_js, NULL, NULL))
		g_warning("GWV: could not read %s", bridge_path);
	g_free(bridge_path);

	/* Tools menu. */
	st->menu_webview = gtk_menu_item_new_with_mnemonic(_("_Geany WebView"));
	gtk_widget_show(st->menu_webview);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_webview);
	g_signal_connect(st->menu_webview, "activate", G_CALLBACK(on_menu_webview), st);

	st->menu_terminal = gtk_menu_item_new_with_mnemonic(_("Open _Terminal"));
	gtk_widget_show(st->menu_terminal);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_terminal);
	g_signal_connect(st->menu_terminal, "activate", G_CALLBACK(on_menu_terminal), st);

	/* Sidebar hello view (eager). */
	st->hello = gwv_view_new(st,
		GTK_NOTEBOOK(geany_data->main_widgets->sidebar_notebook),
		_("WebView"), "hello/index.html", TRUE);

	/* Message-window terminal view (lazy: shell spawns on first open). */
	st->terminal = gwv_view_new(st,
		GTK_NOTEBOOK(geany_data->main_widgets->message_window_notebook),
		_("Terminal"), "terminal/index.html", FALSE);
	if (st->terminal->bridge != NULL) {
		bridge_on(st->terminal->bridge, "pty.start",     on_ch_pty_start,   st->terminal);
		bridge_on(st->terminal->bridge, "pty.data",      on_ch_pty_data,    st->terminal);
		bridge_on(st->terminal->bridge, "pty.resize",    on_ch_pty_resize,  st->terminal);
		bridge_on(st->terminal->bridge, "ui.focusEditor", on_ch_focus_editor, st->terminal);
	}

	/* Keybindings (unbound by default; the user assigns them in Preferences). */
	g_gwv_state = st;
	GeanyKeyGroup *kg = plugin_set_key_group(plugin, "geany_webview", KB_COUNT, NULL);
	keybindings_set_item(kg, KB_FOCUS_TERMINAL, kb_focus_terminal, 0, (GdkModifierType) 0,
	                     "focus_terminal", _("Focus terminal"), st->menu_terminal);
	keybindings_set_item(kg, KB_FOCUS_WEBVIEW, kb_focus_webview, 0, (GdkModifierType) 0,
	                     "focus_webview", _("Focus WebView"), st->menu_webview);

	geany_plugin_set_data(plugin, st, NULL);
	return TRUE;
}

static void gwv_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	if (st == NULL)
		return;

	gwv_view_free(st->terminal);
	gwv_view_free(st->hello);
	if (st->menu_terminal != NULL)
		gtk_widget_destroy(st->menu_terminal);
	if (st->menu_webview != NULL)
		gtk_widget_destroy(st->menu_webview);

	ui_set_statusbar(FALSE, "%s", "");
	g_free(st->asset_root);
	g_free(st->bridge_js);
	g_free(st);
	g_gwv_state = NULL;
}

G_MODULE_EXPORT
void geany_load_module(GeanyPlugin *plugin)
{
	plugin->info->name = _("Geany WebView");
	plugin->info->description =
		_("Reusable WebView host panes: terminal (ConPTY) and Markdown/HTML preview.");
	plugin->info->version = "0.2.0";
	plugin->info->author = "Geany WebView contributors";

	plugin->funcs->init      = gwv_init;
	plugin->funcs->cleanup   = gwv_cleanup;
	plugin->funcs->configure = NULL;
	plugin->funcs->help      = NULL;
	plugin->funcs->callbacks = NULL;

	/* Keep the DLL resident: json-glib's GObject types (and our process-wide
	 * WebView2 environment) must survive disable/enable. */
	plugin_module_make_resident(plugin);

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
