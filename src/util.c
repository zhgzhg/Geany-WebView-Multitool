/*
 * util.c — small cross-platform helpers for the plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "util.h"

#ifdef G_OS_WIN32

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winternl.h>   /* RTL_OSVERSIONINFOW */

gchar *gwv_plugin_dir(void)
{
	HMODULE self = NULL;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCWSTR) &gwv_plugin_dir, &self);
	wchar_t buf[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
	gchar *full = g_utf16_to_utf8((const gunichar2 *) buf, n, NULL, NULL, NULL);
	gchar *dir = g_path_get_dirname(full);
	g_free(full);
	return dir;
}

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

#include <dlfcn.h>

gchar *gwv_plugin_dir(void)
{
	Dl_info info;
	if (dladdr((void *) &gwv_plugin_dir, &info) && info.dli_fname != NULL)
		return g_path_get_dirname(info.dli_fname);
	return g_strdup(".");
}

gboolean gwv_os_supported(void)
{
	return TRUE;
}

#endif /* G_OS_WIN32 */
