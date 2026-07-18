/*
 * pty_win32.c — ConPTY implementation of the pty.h service.
 *
 * A reader thread pumps child output to the GTK main thread via g_idle_add; an
 * exit-watcher thread waits on the child and reports exit. Teardown follows the
 * documented ConPTY order to avoid the close-time deadlock.
 *
 * Build with -D_WIN32_WINNT=0x0A00 so the Win10-1809 pseudoconsole API is
 * visible. Requires Windows 10 1809+.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "pty.h"

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define PTY_READ_BUF     16384
#define PTY_MAX_PENDING  (4 * 1024 * 1024)   /* backpressure threshold (bytes) */

/* Async-safe link between a Pty and its in-flight g_idle callbacks. Everything
 * touching ->pty runs on the GTK main thread; the refcount governs the link's
 * own lifetime only. */
typedef struct {
	LONG  ref;
	Pty  *pty;
} PtyLink;

struct Pty {
	HPCON               hpc;
	HANDLE              input_write;   /* we write child input here */
	HANDLE              output_read;   /* we read child output here */
	PROCESS_INFORMATION pi;
	LPPROC_THREAD_ATTRIBUTE_LIST attr;
	HANDLE              reader_thread;
	HANDLE              exit_thread;
	PtyCallbacks        cb;
	gpointer            user;
	PtyLink            *link;
	volatile LONG       shutdown;
	volatile LONG       pending_bytes;
};

static PtyLink *pty_link_new(Pty *p) { PtyLink *l = g_new0(PtyLink, 1); l->ref = 1; l->pty = p; return l; }
static void     pty_link_ref(PtyLink *l)   { InterlockedIncrement(&l->ref); }
static void     pty_link_unref(PtyLink *l) { if (InterlockedDecrement(&l->ref) == 0) g_free(l); }

static wchar_t *u8_to_w(const char *s)
{
	return (wchar_t *) g_utf8_to_utf16(s, -1, NULL, NULL, NULL);
}

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
		InterlockedExchangeAdd(&p->pending_bytes, -(LONG) c->len);
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

static DWORD WINAPI reader_main(LPVOID param)
{
	Pty  *p = param;                 /* valid until pty_free joins this thread */
	char  buf[PTY_READ_BUF];

	for (;;) {
		DWORD n = 0;
		BOOL ok = ReadFile(p->output_read, buf, sizeof buf, &n, NULL);
		if (!ok || n == 0)
			break;                   /* pipe closed by ClosePseudoConsole/EOF */
		if (p->shutdown)
			continue;                /* draining during teardown — discard    */

		/* Non-blocking backpressure: if the UI thread is behind, pause reads
		 * (which pauses ConPTY, which pauses the shell) instead of buffering
		 * without bound. Never blocks on the main thread, so teardown is safe. */
		while (p->pending_bytes > PTY_MAX_PENDING && !p->shutdown)
			Sleep(2);
		if (p->shutdown)
			continue;

		DataChunk *c = g_new0(DataChunk, 1);
		c->buf = g_memdup2(buf, n);
		c->len = n;
		c->link = p->link;
		pty_link_ref(p->link);
		InterlockedExchangeAdd(&p->pending_bytes, (LONG) n);
		g_idle_add(deliver_data, c);
	}
	return 0;
}

static DWORD WINAPI exit_main(LPVOID param)
{
	Pty  *p = param;
	WaitForSingleObject(p->pi.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(p->pi.hProcess, &code);

	ExitChunk *c = g_new0(ExitChunk, 1);
	c->link = p->link;
	c->code = (int) code;
	pty_link_ref(p->link);
	g_idle_add(deliver_exit, c);
	return 0;
}

/* -------------------------------------- API ------------------------------- */

Pty *pty_spawn(const char *shell, const char *cwd, int cols, int rows,
               const PtyCallbacks *cb, gpointer user)
{
	HANDLE in_read = NULL, in_write = NULL, out_read = NULL, out_write = NULL;
	if (!CreatePipe(&in_read, &in_write, NULL, 0))
		return NULL;
	if (!CreatePipe(&out_read, &out_write, NULL, 0)) {
		CloseHandle(in_read); CloseHandle(in_write);
		return NULL;
	}

	HPCON hpc = NULL;
	COORD size = { (SHORT) cols, (SHORT) rows };
	HRESULT hr_pc = CreatePseudoConsole(size, in_read, out_write, 0, &hpc);
	if (FAILED(hr_pc))
		goto fail_pipes;

	/* Child inherits the pseudoconsole via a process attribute. */
	SIZE_T need = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &need);
	LPPROC_THREAD_ATTRIBUTE_LIST attr = malloc(need);
	if (attr == NULL || !InitializeProcThreadAttributeList(attr, 1, 0, &need))
		goto fail_attr;
	if (!UpdateProcThreadAttribute(attr, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
	                               hpc, sizeof hpc, NULL, NULL))
		goto fail_attrlist;

	STARTUPINFOEXW si;
	ZeroMemory(&si, sizeof si);
	si.StartupInfo.cb = sizeof si;
	si.lpAttributeList = attr;

	PROCESS_INFORMATION pi;
	ZeroMemory(&pi, sizeof pi);

	wchar_t *cmd_w = u8_to_w(shell);              /* CreateProcessW may modify */
	wchar_t *cwd_w = (cwd != NULL) ? u8_to_w(cwd) : NULL;
	BOOL ok = CreateProcessW(NULL, cmd_w, NULL, NULL, FALSE,
	                         EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
	                         NULL, cwd_w, &si.StartupInfo, &pi);
	g_free(cmd_w);
	g_free(cwd_w);
	if (!ok)
		goto fail_attrlist;

	/* The pseudoconsole holds its own copies of the child-side pipe ends. */
	CloseHandle(in_read);   in_read = NULL;
	CloseHandle(out_write); out_write = NULL;

	Pty *p = g_new0(Pty, 1);
	p->hpc = hpc;
	p->input_write = in_write;
	p->output_read = out_read;
	p->pi = pi;
	p->attr = attr;
	if (cb != NULL)
		p->cb = *cb;
	p->user = user;
	p->link = pty_link_new(p);
	p->reader_thread = CreateThread(NULL, 0, reader_main, p, 0, NULL);
	p->exit_thread   = CreateThread(NULL, 0, exit_main,   p, 0, NULL);
	return p;

fail_attrlist:
	DeleteProcThreadAttributeList(attr);
fail_attr:
	free(attr);
	ClosePseudoConsole(hpc);
fail_pipes:
	if (in_read)   CloseHandle(in_read);
	if (in_write)  CloseHandle(in_write);
	if (out_read)  CloseHandle(out_read);
	if (out_write) CloseHandle(out_write);
	return NULL;
}

void pty_write(Pty *p, const char *bytes, gsize len)
{
	if (p == NULL || p->input_write == NULL || len == 0)
		return;
	DWORD written = 0;
	WriteFile(p->input_write, bytes, (DWORD) len, &written, NULL);
}

void pty_resize(Pty *p, int cols, int rows)
{
	if (p == NULL || p->hpc == NULL)
		return;
	COORD size = { (SHORT) cols, (SHORT) rows };
	ResizePseudoConsole(p->hpc, size);
}

void pty_free(Pty *p)
{
	if (p == NULL)
		return;

	InterlockedExchange(&p->shutdown, 1);

	/* Stop writing, force the child to exit. */
	if (p->input_write != NULL) { CloseHandle(p->input_write); p->input_write = NULL; }
	if (p->pi.hProcess != NULL) TerminateProcess(p->pi.hProcess, 0);

	/* Close the pseudoconsole: it flushes a final frame that the (still-running)
	 * reader drains, then the output pipe EOFs and the reader exits. Must not be
	 * called on the reader thread — we are on the main thread. */
	if (p->hpc != NULL) { ClosePseudoConsole(p->hpc); p->hpc = NULL; }

	if (p->reader_thread != NULL) {
		WaitForSingleObject(p->reader_thread, INFINITE);
		CloseHandle(p->reader_thread);
	}
	if (p->exit_thread != NULL) {
		WaitForSingleObject(p->exit_thread, INFINITE);
		CloseHandle(p->exit_thread);
	}

	if (p->output_read != NULL) CloseHandle(p->output_read);
	if (p->pi.hThread  != NULL) CloseHandle(p->pi.hThread);
	if (p->pi.hProcess != NULL) CloseHandle(p->pi.hProcess);
	if (p->attr != NULL) { DeleteProcThreadAttributeList(p->attr); free(p->attr); }

	p->link->pty = NULL;         /* neutralize any queued idle callbacks */
	pty_link_unref(p->link);
	g_free(p);
}
