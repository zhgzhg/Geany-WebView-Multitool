/*
 * views/terminal.h — the message-window ConPTY terminal view.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_VIEWS_TERMINAL_H
#define GEANYWEBVIEW_VIEWS_TERMINAL_H

#include "plugin.h"

G_BEGIN_DECLS

void gwv_terminal_create(GwvState *st);
void gwv_terminal_destroy(GwvState *st);

/* Push the configured instance count to the pages (adds/removes tabs live). */
void gwv_terminal_sync_instances(GwvState *st);

/* Shared machinery for other terminal placements (views/sideterm.c): a fully
 * wired terminal view (notebook may be NULL for an unattached panel), and the
 * install/remove sync of the keybinding-override snooper. */
GwvView *gwv_terminal_new_view(GwvState *st, GtkNotebook *notebook);
void     gwv_terminal_snooper_sync(GwvState *st);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEWS_TERMINAL_H */
