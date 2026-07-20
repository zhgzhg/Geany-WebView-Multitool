/*
 * pty_test.c — standalone harness for the Unix PTY service (diagnostics).
 *
 * Validates, without Geany: shell spawn, output delivery on the main loop,
 * MID-STREAM teardown (pty_free while the child is flooding — the risky path:
 * a blocked read() must be woken by killing the child, then joined without
 * deadlock), and exit reporting for a short-lived child.
 *
 * Exit code 0 = PASS.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "services/pty.h"

#include <stdio.h>
#include <string.h>

static GMainLoop *loop;
static Pty       *pty;
static GString   *collected;
static gboolean   got_exit = FALSE;
static int        exit_code = -999;
static gboolean   ok_stream = FALSE, ok_exit = FALSE;

static void on_data(Pty *p, const char *bytes, gsize len, gpointer user)
{
	(void) p; (void) user;
	g_string_append_len(collected, bytes, (gssize) len);
}

static void on_exit_cb(Pty *p, int code, gpointer user)
{
	(void) p; (void) user;
	got_exit = TRUE;
	exit_code = code;
}

/* Phase 1 check: marker received + data flowing, then free mid-stream. */
static gboolean phase1_check(gpointer user)
{
	(void) user;
	gboolean marker = strstr(collected->str, "GWV_MARKER") != NULL;
	g_print("phase1: %zu bytes, marker=%s -> freeing mid-stream\n",
	        collected->len, marker ? "yes" : "NO");
	ok_stream = marker && collected->len > 100000;
	pty_free(pty);                    /* child still flooding: must not hang */
	pty = NULL;
	g_print("phase1: pty_free returned (no deadlock)\n");
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static gboolean phase2_check(gpointer user)
{
	(void) user;
	g_print("phase2: exit=%s code=%d\n", got_exit ? "yes" : "NO", exit_code);
	ok_exit = got_exit && exit_code == 42;
	pty_free(pty);                    /* free after exit: also safe */
	pty = NULL;
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

int main(void)
{
	PtyCallbacks cb = { on_data, on_exit_cb };
	collected = g_string_new(NULL);
	loop = g_main_loop_new(NULL, FALSE);

	/* Phase 1: flooding child, teardown mid-stream. */
	pty = pty_spawn("/bin/sh -c 'echo GWV_MARKER; yes gwv-flood; sleep 30'",
	                "/tmp", 80, 24, &cb, NULL);
	if (pty == NULL) { g_printerr("spawn failed\n"); return 1; }
	g_timeout_add(1500, phase1_check, NULL);
	g_main_loop_run(loop);

	/* Phase 2: short-lived child, exit code delivery. */
	g_string_truncate(collected, 0);
	got_exit = FALSE;
	pty = pty_spawn("/bin/sh -c 'exit 42'", "/tmp", 80, 24, &cb, NULL);
	if (pty == NULL) { g_printerr("spawn2 failed\n"); return 1; }
	g_timeout_add(1200, phase2_check, NULL);
	g_main_loop_run(loop);

	g_print("RESULT: stream=%s exit=%s\n",
	        ok_stream ? "PASS" : "FAIL", ok_exit ? "PASS" : "FAIL");
	return (ok_stream && ok_exit) ? 0 : 1;
}
