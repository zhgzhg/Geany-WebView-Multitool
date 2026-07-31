/*
 * views/preview.c — sidebar Markdown / HTML preview: pushes the active document
 * to the preview page (debounced), serves the document's directory for relative
 * images, and persists the dark/light background theme.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "views/preview.h"
#include "view.h"
#include "findbar.h"
#include "settings.h"

/* HTML previews as a *real* served resource (not srcdoc) so it renders without
 * inheriting the preview page's CSP: published as an in-memory document on the
 * asset host, pointed at with a cache-busting query.
 *
 * The served charset mirrors the document's encoding (Document > Set Encoding).
 * Editor buffers are always UTF-8 regardless of that setting, so honoring it
 * means converting the text back; without an explicit charset the engine falls
 * back to Windows-1252 and multibyte chars (€, …) render as mojibake. */
static void push_html_preview(GwvState *st, GeanyDocument *doc, const char *html)
{
	const char  *body = (html != NULL) ? html : "";
	gssize       body_len = -1;
	const gchar *enc = (doc != NULL) ? doc->encoding : NULL;
	gchar       *converted = NULL;
	gchar       *mime = NULL;

	if (enc != NULL && g_ascii_strcasecmp(enc, "None") == 0) {
		/* Opened without conversion: raw bytes of unknown encoding — declare
		 * none and let the engine sniff (BOM, <meta charset>). */
		mime = g_strdup("text/html");
	} else if (enc != NULL && g_ascii_strcasecmp(enc, "UTF-8") != 0) {
		gsize written = 0;
		converted = g_convert(body, -1, enc, "UTF-8", NULL, &written, NULL);
		if (converted != NULL) {
			body = converted;
			body_len = (gssize) written;
			mime = g_strdup_printf("text/html; charset=%s", enc);
		}
	}
	if (mime == NULL)   /* UTF-8 doc, or text not representable in enc */
		mime = g_strdup("text/html; charset=utf-8");

	wv_host_put_virtual(st->preview->host, GWV_HTMLPREVIEW_FILE,
	                    body, body_len, mime);
	g_debug("GWV: preview push html v=%d (%s)", st->html_ver + 1, mime);
	g_free(mime);
	g_free(converted);
	st->html_ver++;
	gchar *file = g_strdup_printf(GWV_HTMLPREVIEW_FILE "?v=%d", st->html_ver);
	gchar *url = wv_host_format_url(GWV_VIRTUAL_HOST, file);
	bridge_post_text(st->preview->bridge, "preview.html", "url", url);
	g_free(url);
	g_free(file);
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

	/* Tell the page which document this render belongs to: it preserves the
	 * scroll position across re-renders of the same document (typing) and
	 * jumps to the top when the document changes. */
	gchar *docid = (doc->file_name != NULL)
	               ? g_strdup(doc->file_name)
	               : g_strdup_printf("untitled-%p", (gpointer) doc);
	bridge_post_text(v->bridge, "preview.doc", "id", docid);
	g_free(docid);

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
		push_html_preview(st, doc, text);
		g_free(text);
	} else if (as_md) {
		gchar *text = sci_get_contents(doc->editor->sci, -1);
		/* Serve the document's own directory so relative images (![](pic.png))
		 * resolve; the page sets its <base> to it. Untitled docs have no dir. */
		gchar *dir = gwv_current_doc_dir();
		gchar *base = NULL;
		if (dir != NULL) {
			if (g_strcmp0(dir, st->doc_host_dir) != 0) {
				wv_host_map_dir(v->host, GWV_DOC_HOST, dir);
				g_free(st->doc_host_dir);
				st->doc_host_dir = g_strdup(dir);
			}
			base = wv_host_format_url(GWV_DOC_HOST, "");
		}
		bridge_post_text(v->bridge, "preview.base", "url", base != NULL ? base : "");
		bridge_post_text(v->bridge, "preview.md",   "text", text ? text : "");
		g_free(base);
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

/* Push the saved/current mode to the page (toolbar highlight) and re-render. */
void gwv_preview_sync_mode(GwvState *st)
{
	if (st->preview == NULL || st->preview->bridge == NULL)
		return;
	bridge_post_text(st->preview->bridge, "preview.mode", "mode",
	                 settings_preview_mode_name(st->preview_mode));
	update_preview(st);
}

/* The preview page finished loading — push the saved theme + mode, render. */
static void on_preview_ready(Bridge *bridge, const char *payload, gpointer user)
{
	(void) payload;
	GwvState *st = user;
	bridge_post_text(bridge, "preview.theme", "theme",
	                 st->preview_theme ? st->preview_theme : "dark");
	gwv_preview_sync_mode(st);
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
	st->preview_mode = settings_preview_mode_value(mode);
	g_free(mode);
	update_preview(st);
	settings_save(st);   /* the chosen mode persists, like the theme */
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

/* A link in the rendered markdown resolved against the document host — i.e. a
 * local file next to the current document (the page's <base> points there).
 * Open it in the editor: the preview follows the newly active document, and
 * the preview page itself never navigates away. All doc-host URLs are
 * consumed (missing files just report to the status bar); anything else
 * returns FALSE and goes to the OS browser as before. */
static gboolean on_preview_navigate(GwvView *v, const char *url)
{
	GwvState *st = v->st;
	GUri *u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (u == NULL)
		return FALSE;
	const gchar *host = g_uri_get_host(u);
	const gchar *path = g_uri_get_path(u);        /* percent-decoded, no #/? */
	if (g_strcmp0(host, GWV_DOC_HOST) != 0) {
		g_uri_unref(u);
		return FALSE;                             /* a real external link */
	}
	if (st->doc_host_dir == NULL || path == NULL || *path == '\0') {
		g_uri_unref(u);
		return TRUE;
	}
	/* Resolve within the document's folder only (no ../ escapes). */
	gchar *full = g_build_filename(st->doc_host_dir, path + 1, NULL);
	gchar *canon = g_canonicalize_filename(full, NULL);
	gchar *root = g_canonicalize_filename(st->doc_host_dir, NULL);
	gchar *root_sl = g_strconcat(root, G_DIR_SEPARATOR_S, NULL);
	if (g_str_has_prefix(canon, root_sl)) {
		gchar *locale = utils_get_locale_from_utf8(canon);
		if (g_file_test(locale, G_FILE_TEST_IS_REGULAR)) {
			g_debug("GWV: preview link -> editor: %s", canon);
			document_open_file(locale, FALSE, NULL, NULL);
		} else {
			ui_set_statusbar(TRUE, _("File not found: %s"), canon);
		}
		g_free(locale);
	} else {
		ui_set_statusbar(TRUE, _("Link outside the document's folder: %s"), path);
	}
	g_free(full);
	g_free(canon);
	g_free(root);
	g_free(root_sl);
	g_uri_unref(u);
	return TRUE;
}

/* Belt and braces: the preview must never leave its own page. If some
 * unanticipated navigation mode gets it off the asset host anyway, snap back —
 * the reloaded page's sys.ready re-pushes theme, mode and content. */
static void on_preview_url_changed(GwvView *v, const char *url)
{
	if (url == NULL || *url == '\0' || g_str_has_prefix(url, "about:"))
		return;
	GUri *u = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
	if (u == NULL)
		return;
	gboolean ours = (g_strcmp0(g_uri_get_host(u), GWV_VIRTUAL_HOST) == 0);
	g_uri_unref(u);
	if (!ours) {
		g_warning("GWV: preview navigated away (%s) — recovering", url);
		gchar *home = wv_host_format_url(GWV_VIRTUAL_HOST, "preview/index.html");
		wv_host_navigate(v->host, home);
		g_free(home);
	}
}

/* Create the sidebar preview view (eager) and wire its bridge channels. */
void gwv_preview_create(GwvState *st)
{
	if (st->preview != NULL)
		return;
	/* Pinned to the virtual host, with find-in-page (Ctrl+F). */
	WvHostConfig cfg = { GWV_VIRTUAL_HOST, st->bridge_js, FALSE, TRUE };
	gchar *url = wv_host_format_url(GWV_VIRTUAL_HOST, "preview/index.html");
	st->preview = gwv_view_new_full(st,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->sidebar_notebook),
		_(GWV_PREVIEW_LABEL), &cfg, url, TRUE, TRUE);
	g_free(url);
	gwv_findbar_attach(st->preview, 0);   /* above the pane */
	st->preview->on_navigate_external = on_preview_navigate;
	st->preview->on_url_changed = on_preview_url_changed;
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
