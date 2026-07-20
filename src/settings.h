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

/* Preview-mode value (0 auto, 1 markdown, 2 html) <-> config/bridge name. */
const char *settings_preview_mode_name (int mode);
int         settings_preview_mode_value(const char *name);

/* Apply Preferences-dialog changes (create/destroy views, mode, shell), then
 * persist. `term_shell` may be NULL/empty for platform auto-detection. */
void settings_apply(GwvState *st, gboolean enable_preview, gboolean enable_terminal,
                    gboolean term_primary, gboolean tools_copy_path,
                    int term_instances, int preview_mode, const char *term_shell);

/* Geany Plugin Manager -> Preferences page for this plugin. */
GtkWidget *gwv_configure(GeanyPlugin *plugin, GtkDialog *dialog, gpointer pdata);

G_END_DECLS

#endif /* GEANYWEBVIEW_SETTINGS_H */
