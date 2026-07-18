/*
 * bridge.h — view <-> native message routing.
 *
 * The JS shim (assets/bridge.js) sends envelopes {"ch":<channel>,"p":<payload>}.
 * A Bridge parses them and dispatches by channel to registered handlers, and
 * posts channel messages back to the page.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef GEANYWEBVIEW_BRIDGE_H
#define GEANYWEBVIEW_BRIDGE_H

#include <glib.h>

#include "wvhost.h"

G_BEGIN_DECLS

typedef struct Bridge Bridge;

/* Called with the payload as a JSON string ("null" if absent). */
typedef void (*BridgeHandler)(Bridge *bridge, const char *payload_json, gpointer user);

Bridge *bridge_new  (WvHost *host);
void    bridge_free (Bridge *bridge);

/* Register (or replace) the handler for a channel. */
void    bridge_on   (Bridge *bridge, const char *channel,
                     BridgeHandler handler, gpointer user);

/* Parse an incoming envelope (from WvHost on_message) and dispatch it. */
void    bridge_handle(Bridge *bridge, const char *envelope_json);

/* Post payload (already-serialized JSON, or NULL) to the page on `channel`. */
void    bridge_post (Bridge *bridge, const char *channel, const char *payload_json);

G_END_DECLS

#endif /* GEANYWEBVIEW_BRIDGE_H */
