/*
 * plugin.h — shared types and core declarations for the Geany WebView plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_PLUGIN_H
#define GEANYWEBVIEW_PLUGIN_H

#include <geanyplugin.h>

#include "bridge.h"
#include "host/wvhost.h"
#include "services/pty.h"

G_BEGIN_DECLS

#define GWV_HTMLPREVIEW_FILE "_htmlpreview.html"
#define GWV_VIRTUAL_HOST     "geanyview.local"
#define GWV_DOC_HOST         "geanyview.doc"   /* current doc's dir, for relative images */
#define GWV_WEBVIEW2_URL     "https://developer.microsoft.com/microsoft-edge/webview2/"

/* The "(WV)" postfix ties every pane to this plugin (and keeps our terminal
 * distinct from Geany's built-in VTE "Terminal" tab on Linux). Compose derived
 * strings by literal concatenation, e.g. _("Show " GWV_PREVIEW_LABEL) — the
 * brand names are not meant to be translated. (If gettext extraction is ever
 * added, composed strings must become printf-style instead: xgettext does not
 * expand macros.) */
#define GWV_PREVIEW_LABEL  "File Preview (WV)"
#define GWV_TERMINAL_LABEL "Terminal (WV)"

/* One embedded WebView bound to a notebook page. */
typedef struct {
	GeanyPlugin *plugin;
	GtkWidget   *panel;      /* notebook page (a GtkBox)                    */
	GtkWidget   *webarea;    /* GtkDrawingArea the browser is parented onto */
	WvHost      *host;
	Bridge      *bridge;
	GHashTable  *ptys;       /* terminal view only: id -> TermSlot; else NULL */
} GwvView;

/* Whole-plugin state. */
typedef struct {
	GeanyPlugin *plugin;
	gchar       *bridge_js;  /* injected shim contents (embedded asset)      */
	GwvView     *preview;    /* sidebar: Markdown/HTML preview               */
	GwvView     *terminal;   /* message window: shell terminal              */
	guint        preview_timer;  /* debounce source id, 0 if none           */
	int          preview_mode;   /* 0 auto (by filetype), 1 markdown, 2 html */
	int          html_ver;       /* cache-buster for the served HTML preview */
	gchar       *doc_host_dir;   /* dir currently mapped to GWV_DOC_HOST     */

	/* Settings, persisted to a GKeyFile in the Geany plugin config dir. */
	gchar       *config_path;
	gboolean     enable_preview;
	gboolean     enable_terminal;
	gboolean     term_primary;    /* PRIMARY selection + middle-click paste   */
	gboolean     tools_copy_path; /* "Copy File Path (WV)" in the Tools menu   */
	int          term_instances;  /* terminal count (in-page tab row if > 1)   */
	gchar       *term_shell;      /* custom shell command; NULL/"" = auto     */
	gchar       *preview_theme;   /* "dark" | "light"                        */
	GtkWidget   *menu_copy_path;   /* Tools-menu item, NULL when disabled     */
	GtkWidget   *cfg_chk_preview;  /* config-dialog widgets (per-open)        */
	GtkWidget   *cfg_chk_terminal;
	GtkWidget   *cfg_chk_primary;
	GtkWidget   *cfg_chk_copy_path;
	GtkWidget   *cfg_combo_mode;
	GtkWidget   *cfg_entry_shell;
	GtkWidget   *cfg_spin_instances;
	guint        term_snooper;     /* key snooper id while terminal exists    */
} GwvState;

enum { KB_FOCUS_TERMINAL, KB_FOCUS_PREVIEW, KB_COUNT };

/* Directory of the current document (or home if untitled). Caller g_free()s. */
gchar *gwv_current_doc_dir(void);

/* "Copy File Path (WV)" Tools-menu item — create/remove per the setting. */
void gwv_copy_path_create(GwvState *st);
void gwv_copy_path_destroy(GwvState *st);

G_END_DECLS

#endif /* GEANYWEBVIEW_PLUGIN_H */
