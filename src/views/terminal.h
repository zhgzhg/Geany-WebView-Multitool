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

/* Push the configured instance count to the page (adds/removes tabs live). */
void gwv_terminal_sync_instances(GwvState *st);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEWS_TERMINAL_H */
