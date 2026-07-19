/*
 * views/terminal.c — message-window terminal view: xterm.js wired to the native
 * ConPTY service (services/pty.h) over the bridge's pty.* channels.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "views/terminal.h"
#include "view.h"

static gchar *resolve_shell(void)
{
	gchar *p;
	if ((p = g_find_program_in_path("pwsh.exe")) != NULL)       return p;
	if ((p = g_find_program_in_path("powershell.exe")) != NULL) return p;
	if ((p = g_find_program_in_path("cmd.exe")) != NULL)        return p;
	return g_strdup("cmd.exe");
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
	gchar *cwd   = gwv_current_doc_dir();
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

/* Create the message-window terminal view and wire its bridge channels. */
void gwv_terminal_create(GwvState *st)
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
		bridge_on(st->terminal->bridge, "ui.copy",        gwv_on_ch_copy,     st->terminal);
	}
}

void gwv_terminal_destroy(GwvState *st)
{
	gwv_view_free(st->terminal);
	st->terminal = NULL;
}
