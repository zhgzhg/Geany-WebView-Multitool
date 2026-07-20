/*
 * assets.c — access to the web assets embedded in the plugin module.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "assets.h"

#include <gio/gio.h>
#include <string.h>

#define GWV_RES_PREFIX "/geany/webview/"

/* Development override directory, or NULL. */
static const char *assets_override_dir(void)
{
	const char *dir = g_getenv("GWV_ASSET_DIR");
	return (dir != NULL && *dir != '\0') ? dir : NULL;
}

/* Read `path` from the override dir; NULL when absent or escaping the dir. */
static GBytes *lookup_override(const char *dir, const char *path)
{
	gchar *full = g_build_filename(dir, path, NULL);
	gchar *canon = g_canonicalize_filename(full, NULL);
	gchar *root = g_canonicalize_filename(dir, NULL);
	GBytes *bytes = NULL;

	if (g_str_has_prefix(canon, root)) {
		gchar *data = NULL;
		gsize len = 0;
		if (g_file_get_contents(canon, &data, &len, NULL))
			bytes = g_bytes_new_take(data, len);
	}
	g_free(full);
	g_free(canon);
	g_free(root);
	return bytes;
}

GBytes *gwv_assets_lookup(const char *path)
{
	if (path == NULL || *path == '\0' || strstr(path, "..") != NULL)
		return NULL;

	const char *dir = assets_override_dir();
	if (dir != NULL) {
		GBytes *bytes = lookup_override(dir, path);
		if (bytes != NULL)
			return bytes;
		/* fall through: override dirs may hold only the files being edited */
	}

	gchar *res = g_strconcat(GWV_RES_PREFIX, path, NULL);
	GBytes *bytes = g_resources_lookup_data(res, G_RESOURCE_LOOKUP_FLAGS_NONE, NULL);
	g_free(res);
	return bytes;
}

gchar *gwv_assets_mime(const char *path)
{
	static const struct { const char *ext, *mime; } kTypes[] = {
		{ ".html", "text/html" },
		{ ".htm",  "text/html" },
		{ ".js",   "text/javascript" },
		{ ".mjs",  "text/javascript" },
		{ ".css",  "text/css" },
		{ ".json", "application/json" },
		{ ".map",  "application/json" },
		{ ".svg",  "image/svg+xml" },
		{ ".png",  "image/png" },
		{ ".jpg",  "image/jpeg" },
		{ ".jpeg", "image/jpeg" },
		{ ".gif",  "image/gif" },
		{ ".webp", "image/webp" },
		{ ".ico",  "image/x-icon" },
		{ ".woff", "font/woff" },
		{ ".woff2","font/woff2" },
		{ ".ttf",  "font/ttf" },
		{ ".txt",  "text/plain" },
		{ ".md",   "text/markdown" },
	};
	if (path != NULL) {
		for (gsize i = 0; i < G_N_ELEMENTS(kTypes); i++)
			if (g_str_has_suffix(path, kTypes[i].ext))
				return g_strdup(kTypes[i].mime);
	}
	return g_strdup("application/octet-stream");
}
