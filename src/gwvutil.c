/*
 * gwvutil.c — small cross-platform helpers for the plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef _WIN32
# define _GNU_SOURCE 1   /* dladdr() — must precede every include */
#endif

#include "gwvutil.h"

#ifdef G_OS_WIN32

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>   /* RTL_OSVERSIONINFOW */

gboolean gwv_os_supported(void)
{
	typedef LONG (WINAPI *RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);
	HMODULE nt = GetModuleHandleW(L"ntdll.dll");
	if (nt != NULL) {
		RtlGetVersionPtr get = (RtlGetVersionPtr) (void *) GetProcAddress(nt, "RtlGetVersion");
		if (get != NULL) {
			RTL_OSVERSIONINFOW vi;
			ZeroMemory(&vi, sizeof vi);
			vi.dwOSVersionInfoSize = sizeof vi;
			if (get(&vi) == 0)
				return (vi.dwMajorVersion > 10) ||
				       (vi.dwMajorVersion == 10 && vi.dwBuildNumber >= 17763);
		}
	}
	return TRUE;   /* couldn't determine — don't block */
}

#else /* !G_OS_WIN32 */

gboolean gwv_os_supported(void)
{
	return TRUE;
}

#endif /* G_OS_WIN32 */
