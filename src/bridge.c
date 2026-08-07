/*
 * bridge.c — view <-> native message routing.
 *
 * JSON is handled by the small strict reader below, deliberately NOT by
 * json-glib: the official Windows Geany bundle ships no json-glib DLL, and
 * geany-plugins' lsp.dll embeds a private copy whose GObject types would
 * collide with a second registration in the same process. This plugin must
 * therefore not touch json-glib at all.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include "bridge.h"

#include <string.h>

typedef struct {
	BridgeHandler fn;
	gpointer      user;
} BridgeReg;

struct Bridge {
	WvHost     *host;
	GHashTable *handlers;   /* channel (gchar*) -> BridgeReg* */
	gchar      *token;      /* expected envelope token; NULL = no check */
};

/* ---------------------------- JSON reader ----------------------------- */
/* Envelopes cross a trust boundary (WebView2 exposes the message channel to
 * whatever document is loaded), so parsing is strict: everything looked at
 * is validated, nesting is bounded, trailing garbage rejects the input. */

#define JSON_MAX_DEPTH 64

typedef struct {
	const char *p;     /* cursor */
	const char *end;   /* one past the last byte */
} JsonIter;

static gboolean json_value(JsonIter *it, int depth);

static void json_ws(JsonIter *it)
{
	while (it->p < it->end &&
	       (*it->p == ' ' || *it->p == '\t' || *it->p == '\n' || *it->p == '\r'))
		it->p++;
}

static gboolean json_hex4(JsonIter *it, gunichar *out)
{
	gunichar v = 0;
	int i;

	if (it->end - it->p < 4)
		return FALSE;
	for (i = 0; i < 4; i++) {
		char c = *it->p++;
		v <<= 4;
		if (c >= '0' && c <= '9')
			v |= (gunichar) (c - '0');
		else if (c >= 'a' && c <= 'f')
			v |= (gunichar) (c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v |= (gunichar) (c - 'A' + 10);
		else
			return FALSE;
	}
	*out = v;
	return TRUE;
}

/* Scan a JSON string starting at its opening quote; leaves the cursor after
 * the closing quote. When `out` is non-NULL the unescaped UTF-8 content is
 * appended to it. */
static gboolean json_string(JsonIter *it, GString *out)
{
	if (it->p >= it->end || *it->p != '"')
		return FALSE;
	it->p++;
	while (it->p < it->end) {
		guchar c = (guchar) *it->p;
		if (c == '"') {
			it->p++;
			return TRUE;
		}
		if (c == '\\') {
			char e;
			it->p++;
			if (it->p >= it->end)
				return FALSE;
			e = *it->p++;
			switch (e) {
			case '"': case '\\': case '/':
				if (out) g_string_append_c(out, e);
				break;
			case 'b': if (out) g_string_append_c(out, '\b'); break;
			case 'f': if (out) g_string_append_c(out, '\f'); break;
			case 'n': if (out) g_string_append_c(out, '\n'); break;
			case 'r': if (out) g_string_append_c(out, '\r'); break;
			case 't': if (out) g_string_append_c(out, '\t'); break;
			case 'u': {
				gunichar u;
				if (!json_hex4(it, &u))
					return FALSE;
				if (u >= 0xD800 && u <= 0xDBFF) {
					gunichar lo;
					if (it->end - it->p < 2 || it->p[0] != '\\' || it->p[1] != 'u')
						return FALSE;
					it->p += 2;
					if (!json_hex4(it, &lo) || lo < 0xDC00 || lo > 0xDFFF)
						return FALSE;
					u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
				} else if (u >= 0xDC00 && u <= 0xDFFF) {
					return FALSE;   /* lone low surrogate */
				}
				if (u == 0)
					return FALSE;   /* no embedded NUL in C strings */
				if (out)
					g_string_append_unichar(out, u);
				break;
			}
			default:
				return FALSE;
			}
		} else if (c < 0x20) {
			return FALSE;   /* raw control characters are not valid JSON */
		} else {
			if (out)
				g_string_append_c(out, (char) c);
			it->p++;
		}
	}
	return FALSE;   /* unterminated */
}

/* Loose number scan (full grammar strictness is not needed for routing);
 * numeric extraction re-parses with g_ascii_strtod which rejects leftovers. */
static gboolean json_number(JsonIter *it)
{
	if (it->p < it->end && *it->p == '-')
		it->p++;
	if (it->p >= it->end || *it->p < '0' || *it->p > '9')
		return FALSE;
	while (it->p < it->end && strchr("0123456789.eE+-", *it->p) != NULL)
		it->p++;
	return TRUE;
}

static gboolean json_lit(JsonIter *it, const char *lit)
{
	gsize n = strlen(lit);

	if ((gsize) (it->end - it->p) < n || strncmp(it->p, lit, n) != 0)
		return FALSE;
	it->p += n;
	return TRUE;
}

static gboolean json_container(JsonIter *it, int depth, char close)
{
	gboolean object = (close == '}');

	it->p++;   /* consume the opener */
	json_ws(it);
	if (it->p < it->end && *it->p == close) {
		it->p++;
		return TRUE;
	}
	for (;;) {
		if (object) {
			json_ws(it);
			if (!json_string(it, NULL))
				return FALSE;
			json_ws(it);
			if (it->p >= it->end || *it->p != ':')
				return FALSE;
			it->p++;
		}
		if (!json_value(it, depth + 1))
			return FALSE;
		json_ws(it);
		if (it->p >= it->end)
			return FALSE;
		if (*it->p == ',') {
			it->p++;
			continue;
		}
		if (*it->p == close) {
			it->p++;
			return TRUE;
		}
		return FALSE;
	}
}

/* Validate and step over one JSON value. */
static gboolean json_value(JsonIter *it, int depth)
{
	json_ws(it);
	if (it->p >= it->end || depth > JSON_MAX_DEPTH)
		return FALSE;
	switch (*it->p) {
	case '"': return json_string(it, NULL);
	case '{': return json_container(it, depth, '}');
	case '[': return json_container(it, depth, ']');
	case 't': return json_lit(it, "true");
	case 'f': return json_lit(it, "false");
	case 'n': return json_lit(it, "null");
	default:  return json_number(it);
	}
}

/* Parse `text` as one complete JSON object (nothing but whitespace may
 * follow) and report the raw span of the top-level member `key`. Returns
 * FALSE when text is not a valid object; a missing member leaves *val_start
 * NULL and returns TRUE. On duplicates the last occurrence wins. */
static gboolean json_object_find(const char *text, const char *key,
                                 const char **val_start, gsize *val_len)
{
	JsonIter it = { text, text + strlen(text) };
	GString *k = g_string_new(NULL);
	gboolean ok = FALSE;

	*val_start = NULL;
	*val_len = 0;

	json_ws(&it);
	if (it.p < it.end && *it.p == '{') {
		it.p++;
		json_ws(&it);
		if (it.p < it.end && *it.p == '}') {
			it.p++;
			ok = TRUE;
		} else {
			for (;;) {
				const char *vs;

				json_ws(&it);
				g_string_truncate(k, 0);
				if (!json_string(&it, k))
					break;
				json_ws(&it);
				if (it.p >= it.end || *it.p != ':')
					break;
				it.p++;
				json_ws(&it);
				vs = it.p;
				if (!json_value(&it, 1))
					break;
				if (strcmp(k->str, key) == 0) {
					*val_start = vs;
					*val_len = (gsize) (it.p - vs);
				}
				json_ws(&it);
				if (it.p < it.end && *it.p == ',') {
					it.p++;
					continue;
				}
				if (it.p < it.end && *it.p == '}') {
					it.p++;
					ok = TRUE;
				}
				break;
			}
		}
	}
	if (ok) {
		json_ws(&it);
		ok = (it.p == it.end);   /* no trailing garbage */
	}
	g_string_free(k, TRUE);
	if (!ok) {
		*val_start = NULL;
		*val_len = 0;
	}
	return ok;
}

/* Unescape a string-value span into a newly allocated UTF-8 C string;
 * NULL when the span is not a JSON string or not valid UTF-8. */
static gchar *json_span_to_string(const char *start, gsize len)
{
	JsonIter it = { start, start + len };
	GString *out;
	gboolean ok;

	if (start == NULL || len < 2 || *start != '"')
		return NULL;
	out = g_string_new(NULL);
	ok = json_string(&it, out) && it.p == it.end &&
	     g_utf8_validate(out->str, -1, NULL);
	return g_string_free(out, !ok);
}

/* Convert a number-value span to int (doubles truncate, huge values clamp);
 * non-numbers — unlike json-glib's coercion — report FALSE. */
static gboolean json_span_to_int(const char *start, gsize len, int *out)
{
	char buf[32];
	gchar *endp = NULL;
	gdouble d;

	if (start == NULL || len == 0 || len >= sizeof buf)
		return FALSE;
	memcpy(buf, start, len);
	buf[len] = '\0';
	d = g_ascii_strtod(buf, &endp);   /* locale-independent on purpose */
	if (endp != buf + len)
		return FALSE;
	if (d >= (gdouble) G_MAXINT)
		*out = G_MAXINT;
	else if (d <= (gdouble) G_MININT)
		*out = G_MININT;
	else
		*out = (int) d;
	return TRUE;
}

void bridge_json_append_quoted(GString *s, const char *text)
{
	const guchar *p;

	g_string_append_c(s, '"');
	for (p = (const guchar *) text; *p != '\0'; p++) {
		switch (*p) {
		case '"':  g_string_append(s, "\\\""); break;
		case '\\': g_string_append(s, "\\\\"); break;
		case '\b': g_string_append(s, "\\b");  break;
		case '\f': g_string_append(s, "\\f");  break;
		case '\n': g_string_append(s, "\\n");  break;
		case '\r': g_string_append(s, "\\r");  break;
		case '\t': g_string_append(s, "\\t");  break;
		default:
			if (*p < 0x20)
				g_string_append_printf(s, "\\u%04x", *p);
			else
				g_string_append_c(s, (char) *p);
		}
	}
	g_string_append_c(s, '"');
}

/* ------------------------------ bridge -------------------------------- */

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
	g_free(b->token);
	g_free(b);
}

void bridge_set_token(Bridge *b, const char *token)
{
	if (b == NULL)
		return;
	g_free(b->token);
	b->token = g_strdup(token);
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
	const char *chs, *ps;
	gsize chl, pl;
	gchar *channel;
	BridgeReg *r;

	if (b == NULL || envelope_json == NULL)
		return;

	if (!json_object_find(envelope_json, "ch", &chs, &chl))
		return;

	/* The shim echoes the per-view token; envelopes without it come from
	 * content that reached the raw message channel some other way (e.g. the
	 * sandboxed HTML-preview iframe on WebKitGTK, where the handler is
	 * exposed to every frame) — drop them. */
	if (b->token != NULL) {
		const char *ts;
		gsize tl;
		gchar *tok;
		json_object_find(envelope_json, "t", &ts, &tl);   /* validated above */
		tok = json_span_to_string(ts, tl);
		if (tok == NULL || strcmp(tok, b->token) != 0) {
			g_debug("GWV: bridge envelope dropped (missing/invalid token)");
			g_free(tok);
			return;
		}
		g_free(tok);
	}

	channel = json_span_to_string(chs, chl);
	if (channel == NULL)
		return;

	r = g_hash_table_lookup(b->handlers, channel);
	if (r != NULL) {
		gchar *payload = NULL;
		json_object_find(envelope_json, "p", &ps, &pl);   /* validated above */
		if (ps != NULL)
			payload = g_strndup(ps, pl);
		r->fn(b, payload != NULL ? payload : "null", r->user);
		g_free(payload);
	}
	g_free(channel);
}

void bridge_post(Bridge *b, const char *channel, const char *payload_json)
{
	GString *envelope;

	if (b == NULL || channel == NULL)
		return;
	envelope = g_string_new("{\"ch\":");
	bridge_json_append_quoted(envelope, channel);
	g_string_append(envelope, ",\"p\":");
	g_string_append(envelope, payload_json != NULL ? payload_json : "null");
	g_string_append_c(envelope, '}');
	wv_host_post_message(b->host, envelope->str);
	g_string_free(envelope, TRUE);
}

void bridge_post_text(Bridge *b, const char *channel, const char *key, const char *value)
{
	GString *payload;

	if (b == NULL || channel == NULL || key == NULL)
		return;
	payload = g_string_new("{");
	bridge_json_append_quoted(payload, key);
	g_string_append_c(payload, ':');
	bridge_json_append_quoted(payload, value != NULL ? value : "");
	g_string_append_c(payload, '}');
	bridge_post(b, channel, payload->str);
	g_string_free(payload, TRUE);
}

gchar *bridge_payload_string(const char *payload_json)
{
	JsonIter it;
	GString *out;
	gboolean ok;

	if (payload_json == NULL)
		return NULL;
	it.p = payload_json;
	it.end = payload_json + strlen(payload_json);
	json_ws(&it);
	if (it.p >= it.end || *it.p != '"')
		return NULL;
	out = g_string_new(NULL);
	ok = json_string(&it, out);
	if (ok) {
		json_ws(&it);
		ok = (it.p == it.end) && g_utf8_validate(out->str, -1, NULL);
	}
	return g_string_free(out, !ok);
}

gchar *bridge_payload_get_string(const char *payload_json, const char *key)
{
	const char *vs;
	gsize vl;

	if (payload_json == NULL)
		return NULL;
	if (!json_object_find(payload_json, key, &vs, &vl))
		return NULL;
	return json_span_to_string(vs, vl);
}

gboolean bridge_payload_get_int(const char *payload_json, const char *key, int *out)
{
	const char *vs;
	gsize vl;

	if (payload_json == NULL || out == NULL)
		return FALSE;
	if (!json_object_find(payload_json, key, &vs, &vl))
		return FALSE;
	return json_span_to_int(vs, vl, out);
}
