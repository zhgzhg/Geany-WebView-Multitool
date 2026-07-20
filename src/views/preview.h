/*
 * views/preview.h — the sidebar Markdown / HTML preview view.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef GEANYWEBVIEW_VIEWS_PREVIEW_H
#define GEANYWEBVIEW_VIEWS_PREVIEW_H

#include "plugin.h"

G_BEGIN_DECLS

void gwv_preview_create(GwvState *st);
void gwv_preview_destroy(GwvState *st);

/* Connect the document/editor signals that refresh the preview (debounced). */
void gwv_preview_connect_signals(GwvState *st);

/* Push st->preview_mode to the page (toolbar highlight) and re-render. */
void gwv_preview_sync_mode(GwvState *st);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEWS_PREVIEW_H */
