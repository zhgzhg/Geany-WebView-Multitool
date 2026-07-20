/*
 * gwvutil.h — small cross-platform helpers for the plugin.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_GWVUTIL_H
#define GEANYWEBVIEW_GWVUTIL_H

#include <glib.h>

G_BEGIN_DECLS

/* Absolute directory containing this plugin's shared object (UTF-8).
 * Caller g_free()s. */
gchar *gwv_plugin_dir(void);

/* TRUE if the OS meets the plugin's floor: Windows 10 1809 (build 17763) or
 * newer. Always TRUE on non-Windows platforms. */
gboolean gwv_os_supported(void);

G_END_DECLS

#endif /* GEANYWEBVIEW_GWVUTIL_H */
