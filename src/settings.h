/*
 * settings.h — plugin settings (GKeyFile) and the Preferences page.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_SETTINGS_H
#define GEANYWEBVIEW_SETTINGS_H

#include "plugin.h"

G_BEGIN_DECLS

/* Load settings; if the file does not exist yet, seed it with the defaults. */
void settings_load(GwvState *st);
void settings_save(GwvState *st);

/* Apply enable/disable changes (create/destroy views), then persist. */
void settings_apply(GwvState *st, gboolean enable_preview, gboolean enable_terminal);

/* Geany Plugin Manager -> Preferences page for this plugin. */
GtkWidget *gwv_configure(GeanyPlugin *plugin, GtkDialog *dialog, gpointer pdata);

G_END_DECLS

#endif /* GEANYWEBVIEW_SETTINGS_H */
