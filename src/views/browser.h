/*
 * views/browser.h — the sidebar free-browsing web pane.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef GEANYWEBVIEW_VIEWS_BROWSER_H
#define GEANYWEBVIEW_VIEWS_BROWSER_H

#include "plugin.h"

G_BEGIN_DECLS

void gwv_browser_create(GwvState *st);
void gwv_browser_destroy(GwvState *st);

/* Refresh UI derived from settings (Home button tooltip). */
void gwv_browser_sync_home(GwvState *st);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEWS_BROWSER_H */
