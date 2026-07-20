/*
 * views/sideterm.h — terminal pane right of the editor area.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#ifndef GEANYWEBVIEW_VIEWS_SIDETERM_H
#define GEANYWEBVIEW_VIEWS_SIDETERM_H

#include "plugin.h"

G_BEGIN_DECLS

void gwv_sideterm_create(GwvState *st);
void gwv_sideterm_destroy(GwvState *st);

G_END_DECLS

#endif /* GEANYWEBVIEW_VIEWS_SIDETERM_H */
