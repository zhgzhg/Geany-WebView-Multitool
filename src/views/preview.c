/*
 * views/preview.c — sidebar Markdown / HTML preview: pushes the active document
 * to the preview page (debounced), serves the document's directory for relative
 * images, and persists the dark/light background theme.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "views/preview.h"
#include "view.h"
#include "settings.h"

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
		gchar *dir = gwv_current_doc_dir();
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
		settings_save(st);
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

/* Create the sidebar preview view (eager) and wire its bridge channels. */
void gwv_preview_create(GwvState *st)
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
		bridge_on(st->preview->bridge, "ui.copy",          gwv_on_ch_copy,         st);
		bridge_on(st->preview->bridge, "ui.theme",         on_ch_set_theme,        st);
	}
}

void gwv_preview_destroy(GwvState *st)
{
	if (st->preview_timer != 0) {
		g_source_remove(st->preview_timer);
		st->preview_timer = 0;
	}
	gwv_view_free(st->preview);
	st->preview = NULL;
}

void gwv_preview_connect_signals(GwvState *st)
{
	GeanyPlugin *plugin = st->plugin;
	plugin_signal_connect(plugin, NULL, "document-activate",     TRUE, G_CALLBACK(on_doc_activity),     st);
	plugin_signal_connect(plugin, NULL, "document-open",         TRUE, G_CALLBACK(on_doc_activity),     st);
	plugin_signal_connect(plugin, NULL, "document-save",         TRUE, G_CALLBACK(on_doc_activity),     st);
	plugin_signal_connect(plugin, NULL, "document-reload",       TRUE, G_CALLBACK(on_doc_activity),     st);
	plugin_signal_connect(plugin, NULL, "document-filetype-set", TRUE, G_CALLBACK(on_doc_filetype_set), st);
	plugin_signal_connect(plugin, NULL, "editor-notify",         TRUE, G_CALLBACK(on_editor_notify),    st);
}
