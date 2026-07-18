/*
 * bridge.c — view <-> native message routing (json-glib).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "bridge.h"

#include <json-glib/json-glib.h>

typedef struct {
	BridgeHandler fn;
	gpointer      user;
} BridgeReg;

struct Bridge {
	WvHost     *host;
	GHashTable *handlers;   /* channel (gchar*) -> BridgeReg* */
};

Bridge *bridge_new(WvHost *host)
{
	Bridge *b = g_new0(Bridge, 1);
	b->host = host;
	b->handlers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	return b;
}

void bridge_free(Bridge *b)
{
	if (b == NULL)
		return;
	g_hash_table_destroy(b->handlers);
	g_free(b);
}

void bridge_on(Bridge *b, const char *channel, BridgeHandler handler, gpointer user)
{
	BridgeReg *r = g_new0(BridgeReg, 1);
	r->fn = handler;
	r->user = user;
	g_hash_table_insert(b->handlers, g_strdup(channel), r);
}

void bridge_handle(Bridge *b, const char *envelope_json)
{
	if (b == NULL || envelope_json == NULL)
		return;

	JsonParser *parser = json_parser_new();
	if (json_parser_load_from_data(parser, envelope_json, -1, NULL)) {
		JsonNode *root = json_parser_get_root(parser);
		if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
			JsonObject *obj = json_node_get_object(root);

			const char *channel = NULL;
			if (json_object_has_member(obj, "ch")) {
				JsonNode *cn = json_object_get_member(obj, "ch");
				if (JSON_NODE_HOLDS_VALUE(cn))
					channel = json_node_get_string(cn);
			}

			if (channel != NULL) {
				BridgeReg *r = g_hash_table_lookup(b->handlers, channel);
				if (r != NULL) {
					gchar *payload = NULL;
					if (json_object_has_member(obj, "p"))
						payload = json_to_string(json_object_get_member(obj, "p"), FALSE);
					r->fn(b, payload != NULL ? payload : "null", r->user);
					g_free(payload);
				}
			}
		}
	}
	g_object_unref(parser);
}

void bridge_post(Bridge *b, const char *channel, const char *payload_json)
{
	if (b == NULL || channel == NULL)
		return;
	/* channel is always a controlled identifier; payload is already JSON. */
	gchar *ch_quoted = g_strdup_printf("\"%s\"", channel);
	gchar *envelope = g_strdup_printf("{\"ch\":%s,\"p\":%s}",
	                                  ch_quoted, payload_json != NULL ? payload_json : "null");
	wv_host_post_message(b->host, envelope);
	g_free(envelope);
	g_free(ch_quoted);
}

gchar *bridge_payload_string(const char *payload_json)
{
	if (payload_json == NULL)
		return NULL;
	JsonParser *parser = json_parser_new();
	gchar *result = NULL;
	if (json_parser_load_from_data(parser, payload_json, -1, NULL)) {
		JsonNode *root = json_parser_get_root(parser);
		if (root != NULL && JSON_NODE_HOLDS_VALUE(root))
			result = g_strdup(json_node_get_string(root));
	}
	g_object_unref(parser);
	return result;
}

gboolean bridge_payload_get_int(const char *payload_json, const char *key, int *out)
{
	if (payload_json == NULL)
		return FALSE;
	JsonParser *parser = json_parser_new();
	gboolean ok = FALSE;
	if (json_parser_load_from_data(parser, payload_json, -1, NULL)) {
		JsonNode *root = json_parser_get_root(parser);
		if (root != NULL && JSON_NODE_HOLDS_OBJECT(root)) {
			JsonObject *obj = json_node_get_object(root);
			if (json_object_has_member(obj, key)) {
				JsonNode *m = json_object_get_member(obj, key);
				if (JSON_NODE_HOLDS_VALUE(m)) {
					*out = (int) json_node_get_int(m);
					ok = TRUE;
				}
			}
		}
	}
	g_object_unref(parser);
	return ok;
}
