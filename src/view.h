/*
 * view.h — generic WebView view lifecycle (platform-agnostic).
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef GEANYWEBVIEW_VIEW_H
#define GEANYWEBVIEW_VIEW_H

#include "plugin.h"

G_BEGIN_DECLS

/* Create a view on a fresh page of `notebook`, load `view_path` from the virtual
 * host, and (if eager) realize it now so it initializes immediately. */
GwvView *gwv_view_new(GwvState *st, GtkNotebook *notebook,
                      const char *label, const char *view_path, gboolean eager);

/* Full-control variant: caller supplies the host config (e.g. allow_browsing,
 * no inject_js), an absolute start URL, and whether a Bridge is wanted.
 * `notebook` may be NULL: the panel is created unattached and the caller packs
 * it into its own container. */
GwvView *gwv_view_new_full(GwvState *st, GtkNotebook *notebook, const char *label,
                           const WvHostConfig *cfg, const char *url,
                           gboolean eager, gboolean with_bridge);
void     gwv_view_reveal(GwvView *v, GtkNotebook *notebook);
void     gwv_view_free(GwvView *v);

/* Append a message widget (optionally with a WebView2-install link) to a view. */
void     gwv_show_view_message(GwvView *v, const char *text, gboolean install_link);

/* Small flat icon button, shared by the pane toolbars and the find bar. */
GtkWidget *gwv_icon_button(const char *icon, const char *tip,
                           GCallback cb, gpointer user);

/* Shared bridge handler: copy the payload string to Geany's own GTK clipboard. */
void     gwv_on_ch_copy(Bridge *bridge, const char *payload, gpointer user);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEW_H */
