/*
 * pty.h — a small pseudo-terminal service.
 *
 * Backends: ConPTY (src/services/pty_win32.c), forkpty (src/services/pty_posix.c, M4).
 * Transport-agnostic: it deals in raw bytes; base64/JSON framing lives in the
 * bridge layer.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_PTY_H
#define GEANYWEBVIEW_PTY_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct Pty Pty;

typedef struct {
	/* Output bytes from the child. Always on the GTK main thread. */
	void (*on_data)(Pty *pty, const char *bytes, gsize len, gpointer user);
	/* The child process exited. Always on the GTK main thread. */
	void (*on_exit)(Pty *pty, int exit_code, gpointer user);
} PtyCallbacks;

/*
 * Spawn `shell` (a full command line) under a new pseudo-terminal sized
 * cols x rows, with working directory `cwd` (NULL = inherit). Returns NULL on
 * failure. Callbacks fire on the GTK main thread.
 */
Pty  *pty_spawn (const char *shell, const char *cwd, int cols, int rows,
                 const PtyCallbacks *cb, gpointer user);

/* Write input bytes to the child. */
void  pty_write (Pty *pty, const char *bytes, gsize len);

/* Resize the pseudo-terminal. */
void  pty_resize(Pty *pty, int cols, int rows);

/* Tear down: terminate the child, stop threads, release everything. Safe to
 * call from on_exit or at any time on the main thread. Safe with NULL. */
void  pty_free  (Pty *pty);

G_END_DECLS

#endif /* GEANYWEBVIEW_PTY_H */
