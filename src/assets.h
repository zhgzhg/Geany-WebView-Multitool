/*
 * assets.h — access to the web assets embedded in the plugin module.
 *
 * The assets tree is compiled into the module as a GResource (see
 * scripts/gen-gresource.sh and meson.build). For development,
 * GWV_ASSET_DIR (a directory) serves from that directory instead (falling back to the
 * embedded copy per file), so assets can be edited without rebuilding.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_ASSETS_H
#define GEANYWEBVIEW_ASSETS_H

#include <glib.h>

G_BEGIN_DECLS

/* Bytes of the asset at `path` (e.g. "preview/index.html"), from the
 * GWV_ASSET_DIR override when set, else from the embedded resources.
 * NULL if not found. Caller g_bytes_unref()s. */
GBytes *gwv_assets_lookup(const char *path);

/* MIME type for an asset path, by extension (deterministic across OSes).
 * Caller g_free()s. */
gchar *gwv_assets_mime(const char *path);

G_END_DECLS

#endif /* GEANYWEBVIEW_ASSETS_H */
