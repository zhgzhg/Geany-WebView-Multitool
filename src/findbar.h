/*
 * findbar.h — shared find-in-page bar for WebView panes (platform-agnostic).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef GEANYWEBVIEW_FINDBAR_H
#define GEANYWEBVIEW_FINDBAR_H

#include "plugin.h"

G_BEGIN_DECLS

/* Attach a hidden find bar to v->panel at `position` and open it on Ctrl+F
 * while focus is inside the panel. No-op on backends whose engine ships its
 * own find UI (WebView2 — see wv_host_needs_find_ui()) and on views whose
 * engine never came up. */
void gwv_findbar_attach(GwvView *v, gint position);

/* Remove the snooper and free the bar state (the widgets die with v->panel).
 * Safe on views without a bar. */
void gwv_findbar_free(GwvView *v);

G_END_DECLS

#endif /* GEANYWEBVIEW_FINDBAR_H */
