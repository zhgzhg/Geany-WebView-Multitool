/*
 * views/terminal.c — message-window terminal view: xterm.js wired to the native
 * ConPTY service (services/pty.h) over the bridge's pty.* channels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "views/terminal.h"
#include "view.h"

static gchar *resolve_shell(GwvState *st)
{
	if (st->term_shell != NULL && *st->term_shell != '\0')
		return g_strdup(st->term_shell);       /* user-configured command */
#ifdef G_OS_WIN32
	gchar *p;
	if ((p = g_find_program_in_path("pwsh.exe")) != NULL)       return p;
	if ((p = g_find_program_in_path("powershell.exe")) != NULL) return p;
	if ((p = g_find_program_in_path("cmd.exe")) != NULL)        return p;
	return g_strdup("cmd.exe");
#else
	const gchar *sh = g_getenv("SHELL");
	if (sh != NULL && *sh != '\0')
		return g_strdup(sh);
	return g_strdup("/bin/bash");
#endif
}

/* One terminal instance: the page multiplexes several xterm.js terminals over
 * the shared bridge — pty.* payloads carry an "id" — and each id gets its own
 * PTY here. Slots live in view->ptys; the table's destroy func kills the shell. */
typedef struct {
	GwvState *st;
	Pty      *pty;
	int       id;
} TermSlot;

static void term_slot_free(gpointer data)
{
	TermSlot *slot = data;
	if (slot->pty != NULL)
		pty_free(slot->pty);
	g_free(slot);
}

static TermSlot *term_slot_lookup(GwvState *st, const char *payload, int *id_out)
{
	int id = 1;
	bridge_payload_get_int(payload, "id", &id);
	if (id_out != NULL)
		*id_out = id;
	if (st->terminal == NULL || st->terminal->ptys == NULL)
		return NULL;
	return g_hash_table_lookup(st->terminal->ptys, GINT_TO_POINTER(id));
}

/* PTY -> page */
static void on_pty_data(Pty *pty, const char *bytes, gsize len, gpointer user)
{
	(void) pty;
	TermSlot *slot = user;
	GwvView  *v = slot->st->terminal;
	if (v == NULL || v->bridge == NULL)
		return;
	gchar *b64 = g_base64_encode((const guchar *) bytes, len);
	gchar *payload = g_strdup_printf("{\"id\":%d,\"data\":\"%s\"}", slot->id, b64);
	bridge_post(v->bridge, "pty.data", payload);
	g_free(payload);
	g_free(b64);
}

static void on_pty_exit(Pty *pty, int code, gpointer user)
{
	(void) pty;
	TermSlot *slot = user;
	GwvView  *v = slot->st->terminal;
	if (v == NULL || v->bridge == NULL)
		return;
	gchar *payload = g_strdup_printf("{\"id\":%d,\"code\":%d}", slot->id, code);
	bridge_post(v->bridge, "pty.exit", payload);
	g_free(payload);
}

/* page -> PTY */
static void on_ch_pty_start(Bridge *bridge, const char *payload, gpointer user)
{
	GwvState *st = user;
	GwvView  *v = st->terminal;
	if (v == NULL || v->ptys == NULL)
		return;
	int cols = 80, rows = 24, id = 1;
	bridge_payload_get_int(payload, "cols", &cols);
	bridge_payload_get_int(payload, "rows", &rows);

	TermSlot *slot = term_slot_lookup(st, payload, &id);
	if (slot == NULL) {
		slot = g_new0(TermSlot, 1);
		slot->st = st;
		slot->id = id;
		g_hash_table_insert(v->ptys, GINT_TO_POINTER(id), slot);
	} else if (slot->pty != NULL) {   /* restart */
		pty_free(slot->pty);
		slot->pty = NULL;
	}
	gchar *shell = resolve_shell(st);
	gchar *cwd   = gwv_current_doc_dir();
	PtyCallbacks pcb = { on_pty_data, on_pty_exit };
	slot->pty = pty_spawn(shell, cwd, cols, rows, &pcb, slot);
	g_debug("GWV: pty.start id=%d shell=%s cwd=%s %dx%d -> %s",
	        id, shell, cwd, cols, rows, slot->pty ? "ok" : "FAILED");
	g_free(shell);
	g_free(cwd);
	if (slot->pty == NULL) {
		gchar *fail = g_strdup_printf("{\"id\":%d,\"code\":-1}", id);
		bridge_post(bridge, "pty.exit", fail);
		g_free(fail);
	}
}

static void on_ch_pty_data(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvState *st = user;
	TermSlot *slot = term_slot_lookup(st, payload, NULL);
	gchar *b64 = bridge_payload_get_string(payload, "data");
	if (b64 != NULL && slot != NULL && slot->pty != NULL) {
		gsize len = 0;
		guchar *bytes = g_base64_decode(b64, &len);
		pty_write(slot->pty, (const char *) bytes, len);
		g_free(bytes);
	}
	g_free(b64);
}

static void on_ch_pty_resize(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvState *st = user;
	TermSlot *slot = term_slot_lookup(st, payload, NULL);
	int cols = 0, rows = 0;
	if (slot != NULL && slot->pty != NULL &&
	    bridge_payload_get_int(payload, "cols", &cols) &&
	    bridge_payload_get_int(payload, "rows", &rows))
		pty_resize(slot->pty, cols, rows);
}

/* The page dropped a tab (instance count lowered): kill that shell. */
static void on_ch_pty_stop(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvState *st = user;
	int id = 1;
	bridge_payload_get_int(payload, "id", &id);
	if (st->terminal != NULL && st->terminal->ptys != NULL &&
	    g_hash_table_remove(st->terminal->ptys, GINT_TO_POINTER(id)))
		g_debug("GWV: pty.stop id=%d", id);
}

/* The page asks for its configuration on load. */
static void on_ch_term_init(Bridge *bridge, const char *payload, gpointer user)
{
	(void) payload;
	GwvState *st = user;
	gchar *cfg = g_strdup_printf("{\"count\":%d}", st->term_instances);
	bridge_post(bridge, "term.config", cfg);
	g_free(cfg);
}

void gwv_terminal_sync_instances(GwvState *st)
{
	if (st->terminal != NULL && st->terminal->bridge != NULL)
		on_ch_term_init(st->terminal->bridge, NULL, st);
}

/* Escape chord from the terminal: move keyboard focus to the editor. */
static void on_ch_focus_editor(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) payload; (void) user;
	keybindings_send_command(GEANY_KEY_GROUP_FOCUS, GEANY_KEYS_FOCUS_EDITOR);
}

/* Page asks to paste: read Geany's clipboard natively and hand the text to
 * xterm.js (term.paste), which applies bracketed-paste. Avoids the browser
 * clipboard-permission path entirely. */
static void on_ch_term_paste(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) payload;
	GwvView *v = user;
	GtkClipboard *cb = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
	gchar *text = gtk_clipboard_wait_for_text(cb);
	if (text != NULL && v->bridge != NULL)
		bridge_post_text(v->bridge, "term.paste", "text", text);
	g_free(text);
}

/* Terminal selection -> X11 PRIMARY, so it middle-click-pastes anywhere,
 * without touching the main clipboard. Gated by the terminal_primary_selection
 * setting. (On Windows GTK emulates PRIMARY process-locally.) */
static void on_ch_set_primary(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge;
	GwvState *st = user;
	if (!st->term_primary)
		return;
	gchar *text = bridge_payload_string(payload);
	if (text != NULL && *text != '\0')
		gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY), text, -1);
	g_free(text);
}

/* Middle-click paste requested from page JS. Only reachable where the browser
 * dispatches DOM mouse events to the page — i.e. WebView2 on Windows, whose
 * input goes to a native HWND the GTK handler below can never see. On the
 * WebKitGTK platforms that handler consumes button-2 before the DOM ever sees
 * it, so this channel stays silent there (no double-paste). */
static void on_ch_paste_primary(Bridge *bridge, const char *payload, gpointer user)
{
	(void) bridge; (void) payload;
	GwvState *st = user;
	if (!st->term_primary || st->terminal == NULL || st->terminal->bridge == NULL)
		return;
	gchar *text = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_PRIMARY));
	if (text != NULL)
		bridge_post_text(st->terminal->bridge, "term.paste", "text", text);
	g_free(text);
}

/* The backend's web widget inside the container (first child on GTK; win32
 * hosts input natively and has no GTK child). */
static GtkWidget *terminal_web_child(GwvView *v)
{
	if (v == NULL || v->webarea == NULL || !GTK_IS_CONTAINER(v->webarea))
		return NULL;
	GList *kids = gtk_container_get_children(GTK_CONTAINER(v->webarea));
	GtkWidget *w = (kids != NULL) ? kids->data : NULL;
	g_list_free(kids);
	return w;
}

/* Middle-click in the terminal -> paste whatever PRIMARY holds (a selection
 * made here, in the editor, or in any other app). Handled at the GTK level and
 * consumed BEFORE WebKit sees it: WebKit has its own built-in middle-click
 * global-selection paste that would otherwise fire as well and double-paste. */
static gboolean on_term_button(GtkWidget *widget, GdkEventButton *ev, gpointer user)
{
	GwvState *st = user;
	if (ev->button != 2)
		return FALSE;
	if (ev->type == GDK_BUTTON_PRESS && st->term_primary &&
	    st->terminal != NULL && st->terminal->bridge != NULL) {
		gtk_widget_grab_focus(widget);   /* blocked press won't focus for us */
		gchar *text = gtk_clipboard_wait_for_text(
			gtk_clipboard_get(GDK_SELECTION_PRIMARY));
		if (text != NULL)
			bridge_post_text(st->terminal->bridge, "term.paste", "text", text);
		g_free(text);
	}
	return TRUE;   /* always own button 2 here, press and release */
}

/* While the terminal has keyboard focus, keys belong to the shell. Geany's
 * keybinding handler sits on the main window's key-press-event and runs before
 * anything the plugin can connect there (handlers run in connection order), so
 * Ctrl+W (close document), Ctrl+K etc. fire in Geany instead of reaching
 * readline. A key snooper runs before ALL widget dispatch — Geany's handler
 * included — so it can forward the key straight to the terminal and swallow
 * the event: the same semantics as Geany's built-in VTE "override Geany
 * keybindings". Deprecated API, but stable for the life of GTK3, and the only
 * hook that precedes another party's window handler. (On Windows this never
 * triggers: WebView2 keys go to its native HWND and bypass GTK.) */
static gint term_key_snooper(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
	GwvState *st = data;
	GwvView  *v = st->terminal;
	if (v == NULL || v->webarea == NULL)
		return FALSE;
	GtkWidget *top = gtk_widget_get_toplevel(widget);
	if (!GTK_IS_WINDOW(top))
		return FALSE;
	GtkWidget *focus = gtk_window_get_focus(GTK_WINDOW(top));
	if (focus == NULL ||
	    (focus != v->webarea && !gtk_widget_is_ancestor(focus, v->webarea)))
		return FALSE;
	gtk_widget_event(focus, (GdkEvent *) event);   /* direct, no snooper re-entry */
	return TRUE;
}

/* Create the message-window terminal view and wire its bridge channels. */
void gwv_terminal_create(GwvState *st)
{
	if (st->terminal != NULL)
		return;
	st->terminal = gwv_view_new(st,
		GTK_NOTEBOOK(st->plugin->geany_data->main_widgets->message_window_notebook),
		_(GWV_TERMINAL_LABEL), "terminal/index.html", FALSE);
	st->terminal->ptys = g_hash_table_new_full(g_direct_hash, g_direct_equal,
	                                           NULL, term_slot_free);
	if (st->terminal->bridge != NULL) {
		bridge_on(st->terminal->bridge, "term.init",      on_ch_term_init,    st);
		bridge_on(st->terminal->bridge, "pty.start",      on_ch_pty_start,    st);
		bridge_on(st->terminal->bridge, "pty.data",       on_ch_pty_data,     st);
		bridge_on(st->terminal->bridge, "pty.resize",     on_ch_pty_resize,   st);
		bridge_on(st->terminal->bridge, "pty.stop",       on_ch_pty_stop,     st);
		bridge_on(st->terminal->bridge, "ui.focusEditor", on_ch_focus_editor, st->terminal);
		bridge_on(st->terminal->bridge, "ui.copy",        gwv_on_ch_copy,     st->terminal);
		bridge_on(st->terminal->bridge, "ui.pasteTerminal", on_ch_term_paste, st->terminal);
		bridge_on(st->terminal->bridge, "ui.setPrimary",    on_ch_set_primary,   st);
		bridge_on(st->terminal->bridge, "ui.pastePrimary",  on_ch_paste_primary, st);
	}
	/* Keybinding override while the terminal is focused (removed on destroy). */
	if (st->terminal->webarea != NULL && st->term_snooper == 0) {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		st->term_snooper = gtk_key_snooper_install(term_key_snooper, st);
		G_GNUC_END_IGNORE_DEPRECATIONS
	}
	/* Own the middle button on the web widget (PRIMARY paste, no double-paste
	 * from WebKit's builtin). Handlers die with the widget. */
	{
		GtkWidget *web = terminal_web_child(st->terminal);
		if (web != NULL) {
			g_signal_connect(web, "button-press-event",   G_CALLBACK(on_term_button), st);
			g_signal_connect(web, "button-release-event", G_CALLBACK(on_term_button), st);
		}
	}
	/* Diagnostic: GWV_WARM_TERMINAL=1 brings the terminal up immediately so
	 * headless runs can exercise the pty/bridge without clicking the tab. */
	if (g_getenv("GWV_WARM_TERMINAL") != NULL && st->terminal->host != NULL)
		wv_host_warmup(st->terminal->host);
}

void gwv_terminal_destroy(GwvState *st)
{
	if (st->term_snooper != 0) {
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		gtk_key_snooper_remove(st->term_snooper);
		G_GNUC_END_IGNORE_DEPRECATIONS
		st->term_snooper = 0;
	}
	gwv_view_free(st->terminal);
	st->terminal = NULL;
}
