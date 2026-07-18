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

#include <glib/gstdio.h>

#include <geanyplugin.h>

#include "wvhost.h"
#include "bridge.h"
#include "util.h"
#include "services/pty.h"

#define GWV_HTMLPREVIEW_FILE "_htmlpreview.html"

#define GWV_VIRTUAL_HOST "geanyview.local"
#define GWV_DOC_HOST     "geanyview.doc"   /* current doc's dir, for relative images */
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
	GwvView     *preview;    /* sidebar: Markdown/HTML preview               */
	GwvView     *terminal;   /* message window: ConPTY terminal             */
	GtkWidget   *menu_preview;
	GtkWidget   *menu_terminal;
	guint        preview_timer;  /* debounce source id, 0 if none           */
	int          preview_mode;   /* 0 auto (by filetype), 1 markdown, 2 html */
	int          html_ver;       /* cache-buster for the served HTML preview */
	gchar       *doc_host_dir;   /* dir currently mapped to GWV_DOC_HOST     */

	/* Settings, persisted to a GKeyFile in the Geany plugin config dir. */
	gchar       *config_path;
	gboolean     enable_preview;
	gboolean     enable_terminal;
	gchar       *preview_theme;   /* "dark" | "light"                        */
	GtkWidget   *cfg_chk_preview;  /* config-dialog widgets (per-open)        */
	GtkWidget   *cfg_chk_terminal;
} GwvState;

/* Single-instance plugin; keybinding callbacks (which get no user data) reach
 * state through this. */
static GwvState *g_gwv_state = NULL;

enum { KB_FOCUS_TERMINAL, KB_FOCUS_PREVIEW, KB_COUNT };

/* --------------------------------------------------------------- settings */

#define GWV_CFG_GROUP "general"

static void config_save(GwvState *st)
{
	GKeyFile *kf = g_key_file_new();
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "enable_preview",  st->enable_preview);
	g_key_file_set_boolean(kf, GWV_CFG_GROUP, "enable_terminal", st->enable_terminal);
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

/* Load settings; if the file does not exist yet, seed it with the defaults. */
static void config_load(GwvState *st)
{
	/* defaults */
	st->enable_preview  = TRUE;
	st->enable_terminal = TRUE;
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
		config_save(st);   /* first run: create it with defaults */
	}
}

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

/* --------------------------------------------------------- preview glue */

/* HTML previews as a *real* served resource (not srcdoc) so it renders without
 * inheriting the preview page's CSP. We write it into the served asset folder
 * and point the iframe at it with a cache-busting query. */
static void push_html_preview(GwvState *st, const char *html)
{
	gchar *path = g_build_filename(st->asset_root, GWV_HTMLPREVIEW_FILE, NULL);
	gboolean ok = g_file_set_contents(path, html != NULL ? html : "", -1, NULL);
	g_free(path);
	if (!ok) {
		bridge_post(st->preview->bridge, "preview.empty", NULL);
		return;
	}
	st->html_ver++;
	gchar *url = g_strdup_printf("https://" GWV_VIRTUAL_HOST "/" GWV_HTMLPREVIEW_FILE "?v=%d",
	                             st->html_ver);
	bridge_post_text(st->preview->bridge, "preview.html", "url", url);
	g_free(url);
}

/* Push the current document to the preview view (Markdown or HTML). */
static void update_preview(GwvState *st)
{
	GwvView *v = st->preview;
	if (v == NULL || v->bridge == NULL)
		return;
	GeanyDocument *doc = document_get_current();
	if (doc == NULL || doc->editor == NULL) {
		bridge_post(v->bridge, "preview.empty", NULL);
		return;
	}
	guint ftid = (doc->file_type != NULL) ? doc->file_type->id : GEANY_FILETYPES_NONE;

	gboolean as_html, as_md;
	if (st->preview_mode == 1) {           /* forced Markdown */
		as_html = FALSE; as_md = TRUE;
	} else if (st->preview_mode == 2) {    /* forced HTML */
		as_html = TRUE;  as_md = FALSE;
	} else {                               /* auto by filetype */
		as_html = (ftid == GEANY_FILETYPES_HTML);
		/* Untitled/plain documents (no filetype yet) preview as Markdown, so a
		 * brand-new unsaved file can be previewed while typing. */
		as_md   = (ftid == GEANY_FILETYPES_MARKDOWN || ftid == GEANY_FILETYPES_NONE);
	}

	if (as_html) {
		gchar *text = sci_get_contents(doc->editor->sci, -1);
		push_html_preview(st, text);
		g_free(text);
	} else if (as_md) {
		gchar *text = sci_get_contents(doc->editor->sci, -1);
		/* Serve the document's own directory so relative images (![](pic.png))
		 * resolve; the page sets its <base> to it. Untitled docs have no dir. */
		gchar *dir = current_doc_dir();
		const char *base = "";
		if (dir != NULL) {
			if (g_strcmp0(dir, st->doc_host_dir) != 0) {
				wv_host_map_dir(v->host, GWV_DOC_HOST, dir);
				g_free(st->doc_host_dir);
				st->doc_host_dir = g_strdup(dir);
			}
			base = "https://" GWV_DOC_HOST "/";
		}
		bridge_post_text(v->bridge, "preview.base", "url", base);
		bridge_post_text(v->bridge, "preview.md",   "text", text ? text : "");
		g_free(dir);
		g_free(text);
	} else {
		bridge_post(v->bridge, "preview.empty", NULL);
	}
}

static gboolean preview_timer_cb(gpointer data)
{
	GwvState *st = data;
	st->preview_timer = 0;
	update_preview(st);
	return G_SOURCE_REMOVE;
}

static void schedule_preview_update(GwvState *st)
{
	if (st->preview_timer != 0)
		g_source_remove(st->preview_timer);
	st->preview_timer = g_timeout_add(300, preview_timer_cb, st);
}

/* The preview page finished loading — push the saved theme, then render. */
static void on_preview_ready(Bridge *bridge, const char *payload, gpointer user)
{
	(void) payload;
	GwvState *st = user;
	bridge_post_text(bridge, "preview.theme", "theme",
	                 st->preview_theme ? st->preview_theme : "dark");
	update_preview(st);
}

/* Diagnostic: the page confirms it rendered (proves the JS pipeline ran). */
static void on_preview_rendered(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) user;
	g_debug("GWV: preview.rendered %s", payload);
}

/* Toolbar: mode selector (auto/md/html) and refresh. */
static void on_ch_preview_set_mode(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvState *st = user;
	gchar *mode = bridge_payload_string(payload);   /* payload is a JSON string */
	if (mode == NULL) {
		/* payload may be an object {mode:"..."}; try that form too. */
		return;
	}
	if      (g_strcmp0(mode, "md") == 0)   st->preview_mode = 1;
	else if (g_strcmp0(mode, "html") == 0) st->preview_mode = 2;
	else                                   st->preview_mode = 0;
	g_free(mode);
	update_preview(st);
}

static void on_ch_preview_refresh(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) payload;
	update_preview(user);
}

/* Copy text from a view (code-block button, terminal selection) to Geany's own
 * GTK clipboard, so it lands on the same clipboard the editor uses. */
static void on_ch_copy(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) user;
	gchar *text = bridge_payload_string(payload);
	if (text != NULL) {
		GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
		gtk_clipboard_set_text(cb, text, -1);
	}
	g_free(text);
}

/* The preview toolbar toggled its background; remember it in the config. */
static void on_ch_set_theme(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvState *st = user;
	gchar *theme = bridge_payload_string(payload);
	if (theme != NULL && (g_strcmp0(theme, "dark") == 0 || g_strcmp0(theme, "light") == 0) &&
	    g_strcmp0(theme, st->preview_theme) != 0) {
		g_free(st->preview_theme);
		st->preview_theme = g_strdup(theme);
		config_save(st);
	}
	g_free(theme);
}

static void on_doc_activity(GObject *obj, GeanyDocument *doc, gpointer user)
{
	(void) obj; (void) doc;
	schedule_preview_update(user);
}

static void on_doc_filetype_set(GObject *obj, GeanyDocument *doc,
                                GeanyFiletype *old, gpointer user)
{
	(void) obj; (void) doc; (void) old;
	schedule_preview_update(user);
}

static gboolean on_editor_notify(GObject *obj, GeanyEditor *editor,
                                 SCNotification *nt, gpointer user)
{
	(void) obj; (void) editor;
	if (nt->nmhdr.code == SCN_MODIFIED &&
	    (nt->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)))
		schedule_preview_update(user);
	return FALSE;
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

/* Create the sidebar preview view and wire its bridge channels. No-op if it
 * already exists. */
static void gwv_preview_create(GwvState *st)
{
	if (st->preview != NULL)
		return;
	st->preview = gwv_view_new(st,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook),
		_("Preview"), "preview/index.html", TRUE);
	if (st->preview->bridge != NULL) {
		bridge_on(st->preview->bridge, "sys.ready",        on_preview_ready,       st);
		bridge_on(st->preview->bridge, "preview.rendered", on_preview_rendered,    st);
		bridge_on(st->preview->bridge, "preview.setMode",  on_ch_preview_set_mode, st);
		bridge_on(st->preview->bridge, "preview.refresh",  on_ch_preview_refresh,  st);
		bridge_on(st->preview->bridge, "ui.copy",          on_ch_copy,             st);
		bridge_on(st->preview->bridge, "ui.theme",         on_ch_set_theme,        st);
	}
}

static void gwv_preview_destroy(GwvState *st)
{
	gwv_view_free(st->preview);
	st->preview = NULL;
}

/* Create the message-window terminal view and wire its bridge channels. */
static void gwv_terminal_create(GwvState *st)
{
	if (st->terminal != NULL)
		return;
	st->terminal = gwv_view_new(st,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->message_window_notebook),
		_("Terminal"), "terminal/index.html", FALSE);
	if (st->terminal->bridge != NULL) {
		bridge_on(st->terminal->bridge, "pty.start",      on_ch_pty_start,    st->terminal);
		bridge_on(st->terminal->bridge, "pty.data",       on_ch_pty_data,     st->terminal);
		bridge_on(st->terminal->bridge, "pty.resize",     on_ch_pty_resize,   st->terminal);
		bridge_on(st->terminal->bridge, "ui.focusEditor", on_ch_focus_editor, st->terminal);
		bridge_on(st->terminal->bridge, "ui.copy",        on_ch_copy,         st->terminal);
	}
}

static void gwv_terminal_destroy(GwvState *st)
{
	gwv_view_free(st->terminal);
	st->terminal = NULL;
}

/* --------------------------------------------------------------- menus */

static void on_menu_preview(GtkMenuItem *item, gpointer user)
{
	(void) item;
	GwvState *st = user;
	gwv_view_reveal(st->preview,
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

static void kb_focus_preview(guint key_id)
{
	(void) key_id;
	if (g_gwv_state != NULL)
		on_menu_preview(NULL, g_gwv_state);
}

/* ------------------------------------------------------------- settings ui */

/* Apply enable/disable changes: create/destroy views, update menu sensitivity,
 * and persist to the config file. */
static void config_apply(GwvState *st, gboolean enable_preview, gboolean enable_terminal)
{
	if (enable_preview != st->enable_preview) {
		st->enable_preview = enable_preview;
		if (enable_preview) gwv_preview_create(st);
		else                gwv_preview_destroy(st);
		gtk_widget_set_sensitive(st->menu_preview, enable_preview);
	}
	if (enable_terminal != st->enable_terminal) {
		st->enable_terminal = enable_terminal;
		if (enable_terminal) gwv_terminal_create(st);
		else                 gwv_terminal_destroy(st);
		gtk_widget_set_sensitive(st->menu_terminal, enable_terminal);
	}
	config_save(st);
}

static void on_configure_response(GtkDialog *dialog, gint response, gpointer user)
{
	(void) dialog;
	if (response != GTK_RESPONSE_OK && response != GTK_RESPONSE_APPLY)
		return;
	GwvState *st = user;
	config_apply(st,
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_preview)),
		gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(st->cfg_chk_terminal)));
}

/* Plugin Manager -> Preferences page for this plugin. */
static GtkWidget *gwv_configure(GeanyPlugin *plugin, GtkDialog *dialog, gpointer pdata)
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

	gtk_widget_show_all(box);
	g_signal_connect(dialog, "response", G_CALLBACK(on_configure_response), st);
	return box;
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
	st->menu_preview = gtk_menu_item_new_with_mnemonic(_("Show _Preview"));
	gtk_widget_show(st->menu_preview);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_preview);
	g_signal_connect(st->menu_preview, "activate", G_CALLBACK(on_menu_preview), st);

	st->menu_terminal = gtk_menu_item_new_with_mnemonic(_("Open _Terminal"));
	gtk_widget_show(st->menu_terminal);
	gtk_container_add(GTK_CONTAINER(geany_data->main_widgets->tools_menu), st->menu_terminal);
	g_signal_connect(st->menu_terminal, "activate", G_CALLBACK(on_menu_terminal), st);

	/* Settings: create the file with defaults on first run, else load it. */
	st->config_path = g_build_filename(geany_data->app->configdir,
	                                   "plugins", "geanywebview.conf", NULL);
	config_load(st);

	/* Create the enabled views (preview eager so it renders at once; terminal
	 * lazy so a shell isn't spawned until opened). */
	if (st->enable_preview)
		gwv_preview_create(st);
	if (st->enable_terminal)
		gwv_terminal_create(st);
	gtk_widget_set_sensitive(st->menu_preview,  st->enable_preview);
	gtk_widget_set_sensitive(st->menu_terminal, st->enable_terminal);

	/* Refresh the preview on document changes (debounced). */
	plugin_signal_connect(plugin, NULL, "document-activate", TRUE, G_CALLBACK(on_doc_activity), st);
	plugin_signal_connect(plugin, NULL, "document-open", TRUE, G_CALLBACK(on_doc_activity), st);
	plugin_signal_connect(plugin, NULL, "document-save", TRUE, G_CALLBACK(on_doc_activity), st);
	plugin_signal_connect(plugin, NULL, "document-reload", TRUE, G_CALLBACK(on_doc_activity), st);
	plugin_signal_connect(plugin, NULL, "document-filetype-set", TRUE, G_CALLBACK(on_doc_filetype_set), st);
	plugin_signal_connect(plugin, NULL, "editor-notify", TRUE, G_CALLBACK(on_editor_notify), st);

	/* Keybindings (unbound by default; the user assigns them in Preferences). */
	g_gwv_state = st;
	GeanyKeyGroup *kg = plugin_set_key_group(plugin, "geany_webview", KB_COUNT, NULL);
	keybindings_set_item(kg, KB_FOCUS_TERMINAL, kb_focus_terminal, 0, (GdkModifierType) 0,
	                     "focus_terminal", _("Focus terminal"), st->menu_terminal);
	keybindings_set_item(kg, KB_FOCUS_PREVIEW, kb_focus_preview, 0, (GdkModifierType) 0,
	                     "focus_preview", _("Show preview"), st->menu_preview);

	geany_plugin_set_data(plugin, st, NULL);
	return TRUE;
}

static void gwv_cleanup(GeanyPlugin *plugin, gpointer pdata)
{
	(void) plugin;
	GwvState *st = pdata;
	if (st == NULL)
		return;

	if (st->preview_timer != 0)
		g_source_remove(st->preview_timer);
	gwv_view_free(st->terminal);
	gwv_view_free(st->preview);
	if (st->menu_terminal != NULL)
		gtk_widget_destroy(st->menu_terminal);
	if (st->menu_preview != NULL)
		gtk_widget_destroy(st->menu_preview);

	ui_set_statusbar(FALSE, "%s", "");
	if (st->asset_root != NULL) {
		gchar *hp = g_build_filename(st->asset_root, GWV_HTMLPREVIEW_FILE, NULL);
		g_unlink(hp);
		g_free(hp);
	}
	g_free(st->asset_root);
	g_free(st->bridge_js);
	g_free(st->doc_host_dir);
	g_free(st->config_path);
	g_free(st->preview_theme);
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
	plugin->funcs->configure = gwv_configure;
	plugin->funcs->help      = NULL;
	plugin->funcs->callbacks = NULL;

	/* Keep the DLL resident: json-glib's GObject types (and our process-wide
	 * WebView2 environment) must survive disable/enable. */
	plugin_module_make_resident(plugin);

	GEANY_PLUGIN_REGISTER(plugin, 235);
}
