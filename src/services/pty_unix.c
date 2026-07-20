/*
 * pty_unix.c — forkpty implementation of the pty.h service (Linux/BSD/macOS).
 *
 * A reader thread pumps child output to the GTK main thread via g_idle_add and
 * reaps the child when the master EOFs/EIOs. Teardown kills the child, which
 * wakes the blocked read() — closing the master fd from another thread does
 * not reliably do that, so the fd is only closed after the reader joins.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#define _GNU_SOURCE 1   /* setenv, forkpty — must precede every include */
#ifdef __APPLE__
# define _DARWIN_C_SOURCE 1   /* keep forkpty visible despite strict POSIX macros */
#endif

#include "services/pty.h"

#include <errno.h>
#ifdef __APPLE__
# include <util.h>      /* forkpty lives here on macOS */
#else
# include <pty.h>       /* forkpty (glibc; in libc since 2.34, else -lutil) */
#endif
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define PTY_READ_BUF     16384
#define PTY_MAX_PENDING  (4 * 1024 * 1024)   /* backpressure threshold (bytes) */

/* Async-safe link between a Pty and its in-flight g_idle callbacks. Everything
 * touching ->pty runs on the GTK main thread; the refcount governs the link's
 * own lifetime only. */
typedef struct {
	gint  ref;
	Pty  *pty;
} PtyLink;

struct Pty {
	int           master;
	pid_t         child;
	GThread      *reader;
	PtyCallbacks  cb;
	gpointer      user;
	PtyLink      *link;
	gint          shutdown;        /* atomic */
	gint          pending_bytes;   /* atomic */
};

static PtyLink *pty_link_new(Pty *p) { PtyLink *l = g_new0(PtyLink, 1); l->ref = 1; l->pty = p; return l; }
static void     pty_link_ref(PtyLink *l)   { g_atomic_int_inc(&l->ref); }
static void     pty_link_unref(PtyLink *l) { if (g_atomic_int_dec_and_test(&l->ref)) g_free(l); }

/* ----------------------------- main-thread delivery ----------------------- */

typedef struct { PtyLink *link; char *buf; gsize len; } DataChunk;
typedef struct { PtyLink *link; int code; }            ExitChunk;

static gboolean deliver_data(gpointer data)
{
	DataChunk *c = data;
	Pty *p = c->link->pty;
	if (p != NULL) {
		if (p->cb.on_data != NULL)
			p->cb.on_data(p, c->buf, c->len, p->user);
		g_atomic_int_add(&p->pending_bytes, -(gint) c->len);
	}
	g_free(c->buf);
	pty_link_unref(c->link);
	g_free(c);
	return G_SOURCE_REMOVE;
}

static gboolean deliver_exit(gpointer data)
{
	ExitChunk *c = data;
	Pty *p = c->link->pty;
	if (p != NULL && p->cb.on_exit != NULL)
		p->cb.on_exit(p, c->code, p->user);
	pty_link_unref(c->link);
	g_free(c);
	return G_SOURCE_REMOVE;
}

/* ------------------------------------ threads ----------------------------- */

static gpointer reader_main(gpointer param)
{
	Pty  *p = param;                 /* valid until pty_free joins this thread */
	char  buf[PTY_READ_BUF];

	for (;;) {
		ssize_t n = read(p->master, buf, sizeof buf);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			break;                   /* EOF, or EIO once the child exits */
		if (g_atomic_int_get(&p->shutdown))
			continue;                /* draining during teardown — discard   */

		/* Non-blocking backpressure: if the UI thread is behind, pause reads
		 * (which fills the pty buffer, which pauses the shell) instead of
		 * buffering without bound. */
		while (g_atomic_int_get(&p->pending_bytes) > PTY_MAX_PENDING &&
		       !g_atomic_int_get(&p->shutdown))
			g_usleep(2000);
		if (g_atomic_int_get(&p->shutdown))
			continue;

		DataChunk *c = g_new0(DataChunk, 1);
		c->buf = g_memdup2(buf, (gsize) n);
		c->len = (gsize) n;
		c->link = p->link;
		pty_link_ref(p->link);
		g_atomic_int_add(&p->pending_bytes, (gint) n);
		g_idle_add(deliver_data, c);
	}

	/* The child is gone (or going): reap it and report the exit code. The
	 * queued idle is neutralized by pty_free via link->pty, like data chunks. */
	int status = 0, code = -1;
	if (waitpid(p->child, &status, 0) == p->child) {
		if (WIFEXITED(status))        code = WEXITSTATUS(status);
		else if (WIFSIGNALED(status)) code = 128 + WTERMSIG(status);
	}
	ExitChunk *c = g_new0(ExitChunk, 1);
	c->link = p->link;
	c->code = code;
	pty_link_ref(p->link);
	g_idle_add(deliver_exit, c);
	return NULL;
}

/* -------------------------------------- API ------------------------------- */

Pty *pty_spawn(const char *shell, const char *cwd, int cols, int rows,
               const PtyCallbacks *cb, gpointer user)
{
	/* Parse before fork: allocation isn't async-signal-safe in the child. */
	gchar **argv = NULL;
	if (!g_shell_parse_argv(shell, NULL, &argv, NULL) || argv == NULL || argv[0] == NULL) {
		g_strfreev(argv);
		return NULL;
	}

	struct winsize ws;
	memset(&ws, 0, sizeof ws);
	ws.ws_col = (unsigned short) cols;
	ws.ws_row = (unsigned short) rows;

	int master = -1;
	pid_t pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0) {
		g_strfreev(argv);
		return NULL;
	}
	if (pid == 0) {
		/* Child: a fresh login-ish environment on the slave pty. */
		if (cwd != NULL && *cwd != '\0')
			if (chdir(cwd) != 0) { /* keep inherited cwd */ }
		setenv("TERM", "xterm-256color", 1);
		setenv("COLORTERM", "truecolor", 1);
		signal(SIGPIPE, SIG_DFL);
		execvp(argv[0], argv);
		_exit(127);
	}
	g_strfreev(argv);

	Pty *p = g_new0(Pty, 1);
	p->master = master;
	p->child = pid;
	if (cb != NULL)
		p->cb = *cb;
	p->user = user;
	p->link = pty_link_new(p);
	p->reader = g_thread_new("gwv-pty-reader", reader_main, p);
	return p;
}

void pty_write(Pty *p, const char *bytes, gsize len)
{
	if (p == NULL || p->master < 0 || len == 0)
		return;
	gsize off = 0;
	while (off < len) {
		ssize_t n = write(p->master, bytes + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;                       /* EIO once the child is gone */
		}
		off += (gsize) n;
	}
}

void pty_resize(Pty *p, int cols, int rows)
{
	if (p == NULL || p->master < 0)
		return;
	struct winsize ws;
	memset(&ws, 0, sizeof ws);
	ws.ws_col = (unsigned short) cols;
	ws.ws_row = (unsigned short) rows;
	ioctl(p->master, TIOCSWINSZ, &ws);
}

void pty_free(Pty *p)
{
	if (p == NULL)
		return;

	g_atomic_int_set(&p->shutdown, 1);

	/* Kill the child: its exit EIOs the master and wakes the blocked read()
	 * (closing the fd from here would not). The reader then reaps it. */
	if (p->child > 0) {
		kill(p->child, SIGHUP);
		kill(p->child, SIGKILL);
	}
	if (p->reader != NULL)
		g_thread_join(p->reader);

	if (p->master >= 0)
		close(p->master);

	p->link->pty = NULL;         /* neutralize any queued idle callbacks */
	pty_link_unref(p->link);
	g_free(p);
}
