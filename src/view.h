/*
 * view.h — generic WebView view lifecycle (platform-agnostic).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_VIEW_H
#define GEANYWEBVIEW_VIEW_H

#include "plugin.h"

G_BEGIN_DECLS

/* Create a view on a fresh page of `notebook`, load `view_path` from the virtual
 * host, and (if eager) realize it now so it initializes immediately. */
GwvView *gwv_view_new(GwvState *st, GtkNotebook *notebook,
                      const char *label, const char *view_path, gboolean eager);
void     gwv_view_reveal(GwvView *v, GtkNotebook *notebook);
void     gwv_view_free(GwvView *v);

/* Append a message widget (optionally with a WebView2-install link) to a view. */
void     gwv_show_view_message(GwvView *v, const char *text, gboolean install_link);

/* Shared bridge handler: copy the payload string to Geany's own GTK clipboard. */
void     gwv_on_ch_copy(Bridge *bridge, const char *payload, gpointer user);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEW_H */
