/*
 * typography.c — the "Simplify Typography (WVM)" Tools-menu action: replace Unicode
 * symbols typical of LLM output and PDF copy-paste (smart quotes, em dashes,
 * arrows, box drawing, invisible spaces, ...) with the plain ASCII a human
 * would type. Works on the selection — or the whole document without one —
 * as a single undo step.
 *
 * The symbol groups are individually toggleable in the Preferences and never
 * overlap: each codepoint belongs to exactly one group (sole exception: the
 * fullwidth hyphen U+FF0D is claimed by the dashes group and falls back to
 * the fullwidth group — both produce '-'), and the only multi-character rule
 * (the em dash, which swallows the plain spaces/tabs around it) consumes
 * characters no group rewrites — so the result does not depend on any
 * replacement order.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */
#include <string.h>

#include "plugin.h"

/* Per-group metadata: the GKeyFile key, the default state, and the
 * Preferences checkbox texts. */
static const struct {
	const char *key;
	gboolean    def;
	const char *label;
	const char *tip;
} typo_meta[GWV_TYPO_COUNT] = {
	[GWV_TYPO_DASHES]    = { "typography_dashes", TRUE, "Dashes (— – −)",
		"En dashes, minus signs, horizontal bars and non-breaking hyphens "
		"become '-'. An em dash and the spaces around it become ' - '; a "
		"run of em dashes counts as one replacement." },
	[GWV_TYPO_QUOTES]    = { "typography_quotes", TRUE, "Quotes (“ ” ‘ ’ ″)",
		"Curly single/double quotes, guillemets and primes become straight "
		"' and \"." },
	[GWV_TYPO_ELLIPSIS]  = { "typography_ellipsis", TRUE, "Ellipsis (…)",
		"The one-character ellipsis becomes three dots." },
	[GWV_TYPO_SPACES]    = { "typography_spaces", TRUE, "Invisible spaces/marks",
		"No-break, thin and hair spaces become a plain space; zero-width "
		"characters, soft hyphens and text-direction marks are removed." },
	[GWV_TYPO_BULLETS]   = { "typography_bullets", TRUE, "Bullets (• ◦ ·)",
		"List bullets and middle dots become '-'." },
	[GWV_TYPO_ARROWS]    = { "typography_arrows", TRUE, "Arrows (→ ⇒ ▶)",
		"Arrows become ->, =>, <-> and so on; triangle arrowheads become "
		">, <, ^, v." },
	[GWV_TYPO_BOXES]     = { "typography_boxes", TRUE, "Box drawing (─ │ ┌)",
		"Box-drawing lines and corners become -, | and +; diagonals become "
		"/, \\ and X." },
	[GWV_TYPO_MATH]      = { "typography_math", TRUE, "Math (≤ × ½)",
		"≤ ≥ ≠ ≈ × ÷ ± become <=, >=, !=, ~, x, /, +/-; vulgar fractions "
		"become 1/2, 3/4, ..." },
	[GWV_TYPO_LIGATURES] = { "typography_ligatures", TRUE, "Ligatures (ﬁ ﬂ)",
		"Latin f-ligatures (typical PDF copy-paste artifacts) become the "
		"separate letters." },
	/* The rest are off by default — a matter of taste or context. */
	[GWV_TYPO_MARKS]     = { "typography_marks", FALSE, "Checkmarks (✓ ✗ ★)",
		"Checkmarks become [x], crosses become x, stars become *. Off by "
		"default: the ASCII stand-ins are a matter of taste." },
	[GWV_TYPO_LEGAL]     = { "typography_legal", FALSE, "Legal marks (™ © ®)",
		"™ ℠ © ® become (TM), (SM), (c), (R). Off by default: sometimes "
		"meaningful exactly as written." },
	[GWV_TYPO_FULLWIDTH] = { "typography_fullwidth", FALSE, "Fullwidth (！Ａ９)",
		"Fullwidth ASCII variants and the ideographic space become their "
		"plain counterparts. Off by default: wrong for CJK documents." },
	[GWV_TYPO_UPDOWN]    = { "typography_updown_arrows", FALSE, "Up/down arrows (↑ ↓)",
		"↑ ⇑ become ^ and ↓ ⇓ become v. Off by default: reads poorly in "
		"prose, useful in diagrams." },
	[GWV_TYPO_BLOCKS]    = { "typography_blocks", FALSE, "Block shading (█ ▒)",
		"Block-element characters (full/half blocks and shades) become "
		"'#'." },
	[GWV_TYPO_SUPERSCRIPTS] = { "typography_superscripts", FALSE, "Superscripts (² ³)",
		"Superscript digits become plain digits (m² -> m2). Off by "
		"default: can corrupt real math notation." },
};

const char *gwv_typography_group_key(int group)
{
	return typo_meta[group].key;
}

const char *gwv_typography_group_label(int group)
{
	return _(typo_meta[group].label);
}

const char *gwv_typography_group_tip(int group)
{
	return _(typo_meta[group].tip);
}

gboolean gwv_typography_group_default(int group)
{
	return typo_meta[group].def;
}

/* One class character per box-drawing codepoint (U+2500..U+257F): '-' for
 * horizontals, '|' for verticals, '+' for corners/tees/crosses, plus the
 * three diagonals. */
static const char box_class[] =
	"--||--||--||"                       /* 2500-0B light/heavy/dashed lines */
	"++++++++++++++++++++++++++++++++"   /* 250C-2B corners and tees         */
	"++++++++++++++++++++++++++++++++"   /* 252C-4B tees and crosses         */
	"--||"                               /* 254C-4F two-dash lines           */
	"-|"                                 /* 2550-51 double lines             */
	"+++++++++++++++++++++++++++++++"    /* 2552-70 double/rounded corners   */
	"/\\X"                               /* 2571-73 diagonals                */
	"-|-|-|-|-|-|";                      /* 2574-7F half lines               */
G_STATIC_ASSERT(sizeof(box_class) == 0x80 + 1);

/* The ASCII a human would have typed for `ch`, "" to remove it, or NULL to
 * leave it alone. The em dash (U+2014) is absent on purpose — it is spacing-
 * aware and handled by the scanner. */
static const char *typo_map(gunichar ch, const gboolean *on)
{
	if (ch >= 0x2500 && ch <= 0x257F) {
		if (!on[GWV_TYPO_BOXES])
			return NULL;
		switch (box_class[ch - 0x2500]) {
		case '-':  return "-";
		case '|':  return "|";
		case '/':  return "/";
		case '\\': return "\\";
		case 'X':  return "X";
		default:   return "+";
		}
	}
	if (ch >= 0x2580 && ch <= 0x259F)        /* block elements */
		return on[GWV_TYPO_BLOCKS] ? "#" : NULL;
	if (ch >= 0x2000 && ch <= 0x200A)        /* en quad .. hair space */
		return on[GWV_TYPO_SPACES] ? " " : NULL;
	if ((ch >= 0x200B && ch <= 0x200F) ||    /* ZWSP, ZWNJ, ZWJ, LRM, RLM */
	    (ch >= 0x202A && ch <= 0x202E) ||    /* directional embeddings    */
	    (ch >= 0x2066 && ch <= 0x2069) ||    /* directional isolates      */
	    ch == 0x00AD || ch == 0x2060 || ch == 0xFEFF)
		return on[GWV_TYPO_SPACES] ? "" : NULL;

	switch (ch) {
	case 0x2011:                             /* non-breaking hyphen    */
	case 0x2012:                             /* figure dash            */
	case 0x2013:                             /* en dash                */
	case 0x2015:                             /* horizontal bar         */
	case 0x2212:                             /* minus sign             */
	case 0xFF0D:                             /* fullwidth hyphen-minus */
		return on[GWV_TYPO_DASHES] ? "-" : NULL;
	case 0x2018: case 0x2019: case 0x201A: case 0x201B:
	case 0x2039: case 0x203A:                /* single guillemets      */
	case 0x02BC:                             /* modifier apostrophe    */
	case 0x2032:                             /* prime (feet, minutes)  */
		return on[GWV_TYPO_QUOTES] ? "'" : NULL;
	case 0x201C: case 0x201D: case 0x201E: case 0x201F:
	case 0x00AB: case 0x00BB:                /* guillemets             */
	case 0x2033:                             /* double prime           */
		return on[GWV_TYPO_QUOTES] ? "\"" : NULL;
	case 0x2026:
		return on[GWV_TYPO_ELLIPSIS] ? "..." : NULL;
	case 0x00A0:                             /* no-break space         */
	case 0x202F:                             /* narrow no-break space  */
	case 0x205F:                             /* medium math space      */
		return on[GWV_TYPO_SPACES] ? " " : NULL;
	case 0x2022: case 0x2023: case 0x2043:   /* bullets                */
	case 0x25E6: case 0x25AA: case 0x25CF:
	case 0x00B7:                             /* middle dot             */
		return on[GWV_TYPO_BULLETS] ? "-" : NULL;
	case 0x2192: case 0x27F6: return on[GWV_TYPO_ARROWS] ? "->"  : NULL;
	case 0x2190: case 0x27F5: return on[GWV_TYPO_ARROWS] ? "<-"  : NULL;
	case 0x2194: case 0x27F7: return on[GWV_TYPO_ARROWS] ? "<->" : NULL;
	case 0x21D2: case 0x27F9: return on[GWV_TYPO_ARROWS] ? "=>"  : NULL;
	case 0x21D0: case 0x27F8: return on[GWV_TYPO_ARROWS] ? "<="  : NULL;
	case 0x21D4: case 0x27FA: return on[GWV_TYPO_ARROWS] ? "<=>" : NULL;
	case 0x25B6: case 0x25B7: case 0x25B8:   /* triangle arrowheads    */
	case 0x25B9: case 0x25BA: case 0x25BB:
		return on[GWV_TYPO_ARROWS] ? ">" : NULL;
	case 0x25C0: case 0x25C1: case 0x25C2:
	case 0x25C3: case 0x25C4: case 0x25C5:
		return on[GWV_TYPO_ARROWS] ? "<" : NULL;
	case 0x25B2: case 0x25B3: case 0x25B4: case 0x25B5:
		return on[GWV_TYPO_ARROWS] ? "^" : NULL;
	case 0x25BC: case 0x25BD: case 0x25BE: case 0x25BF:
		return on[GWV_TYPO_ARROWS] ? "v" : NULL;
	case 0x2264: return on[GWV_TYPO_MATH] ? "<="  : NULL;
	case 0x2265: return on[GWV_TYPO_MATH] ? ">="  : NULL;
	case 0x2260: return on[GWV_TYPO_MATH] ? "!="  : NULL;
	case 0x2248: return on[GWV_TYPO_MATH] ? "~"   : NULL;
	case 0x00D7: return on[GWV_TYPO_MATH] ? "x"   : NULL;
	case 0x00F7: return on[GWV_TYPO_MATH] ? "/"   : NULL;
	case 0x00B1: return on[GWV_TYPO_MATH] ? "+/-" : NULL;
	case 0x2219: case 0x22C5:                /* bullet / dot operator  */
		return on[GWV_TYPO_MATH] ? "*" : NULL;
	case 0x2044: return on[GWV_TYPO_MATH] ? "/"    : NULL;
	case 0x00BC: return on[GWV_TYPO_MATH] ? "1/4"  : NULL;
	case 0x00BD: return on[GWV_TYPO_MATH] ? "1/2"  : NULL;
	case 0x00BE: return on[GWV_TYPO_MATH] ? "3/4"  : NULL;
	case 0x2150: return on[GWV_TYPO_MATH] ? "1/7"  : NULL;
	case 0x2151: return on[GWV_TYPO_MATH] ? "1/9"  : NULL;
	case 0x2152: return on[GWV_TYPO_MATH] ? "1/10" : NULL;
	case 0x2153: return on[GWV_TYPO_MATH] ? "1/3"  : NULL;
	case 0x2154: return on[GWV_TYPO_MATH] ? "2/3"  : NULL;
	case 0x2155: return on[GWV_TYPO_MATH] ? "1/5"  : NULL;
	case 0x2156: return on[GWV_TYPO_MATH] ? "2/5"  : NULL;
	case 0x2157: return on[GWV_TYPO_MATH] ? "3/5"  : NULL;
	case 0x2158: return on[GWV_TYPO_MATH] ? "4/5"  : NULL;
	case 0x2159: return on[GWV_TYPO_MATH] ? "1/6"  : NULL;
	case 0x215A: return on[GWV_TYPO_MATH] ? "5/6"  : NULL;
	case 0x215B: return on[GWV_TYPO_MATH] ? "1/8"  : NULL;
	case 0x215C: return on[GWV_TYPO_MATH] ? "3/8"  : NULL;
	case 0x215D: return on[GWV_TYPO_MATH] ? "5/8"  : NULL;
	case 0x215E: return on[GWV_TYPO_MATH] ? "7/8"  : NULL;
	case 0x2713: case 0x2714: case 0x2705:   /* checkmarks             */
		return on[GWV_TYPO_MARKS] ? "[x]" : NULL;
	case 0x2717: case 0x2718: case 0x274C:   /* crosses                */
		return on[GWV_TYPO_MARKS] ? "x" : NULL;
	case 0x2605: case 0x2606:                /* stars                  */
		return on[GWV_TYPO_MARKS] ? "*" : NULL;
	case 0x2122: return on[GWV_TYPO_LEGAL] ? "(TM)" : NULL;
	case 0x2120: return on[GWV_TYPO_LEGAL] ? "(SM)" : NULL;
	case 0x00A9: return on[GWV_TYPO_LEGAL] ? "(c)"  : NULL;
	case 0x00AE: return on[GWV_TYPO_LEGAL] ? "(R)"  : NULL;
	case 0x3000:                             /* ideographic space      */
		return on[GWV_TYPO_FULLWIDTH] ? " " : NULL;
	case 0x2191: case 0x21D1: return on[GWV_TYPO_UPDOWN] ? "^" : NULL;
	case 0x2193: case 0x21D3: return on[GWV_TYPO_UPDOWN] ? "v" : NULL;
	case 0x00B9: return on[GWV_TYPO_SUPERSCRIPTS] ? "1" : NULL;
	case 0x00B2: return on[GWV_TYPO_SUPERSCRIPTS] ? "2" : NULL;
	case 0x00B3: return on[GWV_TYPO_SUPERSCRIPTS] ? "3" : NULL;
	case 0x2070: return on[GWV_TYPO_SUPERSCRIPTS] ? "0" : NULL;
	case 0x2074: return on[GWV_TYPO_SUPERSCRIPTS] ? "4" : NULL;
	case 0x2075: return on[GWV_TYPO_SUPERSCRIPTS] ? "5" : NULL;
	case 0x2076: return on[GWV_TYPO_SUPERSCRIPTS] ? "6" : NULL;
	case 0x2077: return on[GWV_TYPO_SUPERSCRIPTS] ? "7" : NULL;
	case 0x2078: return on[GWV_TYPO_SUPERSCRIPTS] ? "8" : NULL;
	case 0x2079: return on[GWV_TYPO_SUPERSCRIPTS] ? "9" : NULL;
	case 0xFB00: return on[GWV_TYPO_LIGATURES] ? "ff"  : NULL;
	case 0xFB01: return on[GWV_TYPO_LIGATURES] ? "fi"  : NULL;
	case 0xFB02: return on[GWV_TYPO_LIGATURES] ? "fl"  : NULL;
	case 0xFB03: return on[GWV_TYPO_LIGATURES] ? "ffi" : NULL;
	case 0xFB04: return on[GWV_TYPO_LIGATURES] ? "ffl" : NULL;
	}
	return NULL;
}

/* A pending replacement: byte offsets into the scanned range. */
typedef struct {
	gint     start, end;
	GString *repl;
} TypoSpan;

/* Record a replacement, merging with an adjacent predecessor so runs (box
 * diagrams, arrows) become one Scintilla edit instead of hundreds. */
static void typo_span_add(GArray *spans, gint start, gint end, const char *repl)
{
	if (spans->len > 0) {
		TypoSpan *last = &g_array_index(spans, TypoSpan, spans->len - 1);
		if (last->end == start) {
			g_string_append(last->repl, repl);
			last->end = end;
			return;
		}
	}
	TypoSpan s = { start, end, g_string_new(repl) };
	g_array_append_val(spans, s);
}

/* Scan UTF-8 `text` and append the pending replacements to `spans`; returns
 * the replacement count. `prev_ch`/`next_ch` are the bytes just outside the
 * scanned range ('\n' when the range starts/ends the document), consulted
 * only when an em dash touches a range edge. */
static gint typo_scan(const char *text, const gboolean *on,
                          int prev_ch, int next_ch, GArray *spans)
{
	gint count = 0;
	const char *p = text;
	while (*p != '\0') {
		gunichar ch = g_utf8_get_char_validated(p, -1);
		if (ch == (gunichar) -1 || ch == (gunichar) -2) {
			p++;                             /* not UTF-8; skip the byte */
			continue;
		}
		const char *next = g_utf8_next_char(p);
		gint pos = (gint) (p - text);
		if (ch == 0x2014 && on[GWV_TYPO_DASHES]) {
			/* Em dash: the dash plus any plain spaces/tabs and further em
			 * dashes around it become one hyphen, spaced on each side that
			 * does not touch a line (or document/selection) boundary. */
			gint last_end = spans->len > 0 ?
				g_array_index(spans, TypoSpan, spans->len - 1).end : 0;
			gint s = pos;
			while (s > last_end && (text[s - 1] == ' ' || text[s - 1] == '\t'))
				s--;
			const char *q = next;
			for (;;) {
				if (*q == ' ' || *q == '\t')
					q++;
				else if (strncmp(q, "\xe2\x80\x94", 3) == 0)
					q += 3;
				else
					break;
			}
			int b = s > 0 ? (unsigned char) text[s - 1] : prev_ch;
			int a = *q != '\0' ? (unsigned char) *q : next_ch;
			gboolean lead  = b != '\n' && b != '\r' && b != ' ' && b != '\t';
			gboolean trail = a != '\n' && a != '\r' && a != ' ' && a != '\t';
			typo_span_add(spans, s, (gint) (q - text),
			             lead ? (trail ? " - " : " -") : (trail ? "- " : "-"));
			count++;
			p = q;
			continue;
		}
		const char *repl = typo_map(ch, on);
		if (repl != NULL) {
			typo_span_add(spans, pos, (gint) (next - text), repl);
			count++;
		} else if (ch >= 0xFF01 && ch <= 0xFF5E && on[GWV_TYPO_FULLWIDTH]) {
			/* Fullwidth ASCII variants shift down to plain ASCII; checked
			 * after the map so the dashes group claims U+FF0D first. */
			char fw[2] = { (char) (ch - 0xFEE0), '\0' };
			typo_span_add(spans, pos, (gint) (next - text), fw);
			count++;
		}
		p = next;
	}
	return count;
}

static void on_typography_activate(GtkMenuItem *item, gpointer user)
{
	(void) item;
	GwvState *st = user;
	GeanyDocument *doc = document_get_current();
	if (doc == NULL)
		return;
	if (doc->readonly) {
		ui_set_statusbar(TRUE, "%s", _("Simplify Typography (WVM): the document is read-only."));
		return;
	}
	ScintillaObject *sci = doc->editor->sci;
	gint doc_len = sci_get_length(sci);

	gint rstart = sci_get_selection_start(sci);
	gint rend   = sci_get_selection_end(sci);
	if (rstart == rend) {                    /* no selection: whole document */
		rstart = 0;
		rend   = doc_len;
	}
	if (rend <= rstart)
		return;
	gchar *text = sci_get_contents_range(sci, rstart, rend);
	if (text == NULL)
		return;

	int prev_ch = rstart > 0 ?
		(int) scintilla_send_message(sci, SCI_GETCHARAT, (uptr_t) (rstart - 1), 0) : '\n';
	int next_ch = rend < doc_len ?
		(int) scintilla_send_message(sci, SCI_GETCHARAT, (uptr_t) rend, 0) : '\n';
	GArray *spans = g_array_new(FALSE, FALSE, sizeof(TypoSpan));
	gint count = typo_scan(text, st->typography_groups, prev_ch, next_ch, spans);

	/* Apply back to front so earlier offsets stay valid; one undo step. */
	if (count > 0) {
		sci_start_undo_action(sci);
		for (guint i = spans->len; i > 0; i--) {
			TypoSpan *sp = &g_array_index(spans, TypoSpan, i - 1);
			scintilla_send_message(sci, SCI_SETTARGETSTART,
			                       (uptr_t) (rstart + sp->start), 0);
			scintilla_send_message(sci, SCI_SETTARGETEND,
			                       (uptr_t) (rstart + sp->end), 0);
			scintilla_send_message(sci, SCI_REPLACETARGET,
			                       (uptr_t) sp->repl->len, (sptr_t) sp->repl->str);
		}
		sci_end_undo_action(sci);
		ui_set_statusbar(TRUE, _("Simplify Typography (WVM): %d replacement(s)."), count);
	} else {
		ui_set_statusbar(TRUE, "%s", _("Simplify Typography (WVM): nothing to replace."));
	}

	for (guint i = 0; i < spans->len; i++)
		g_string_free(g_array_index(spans, TypoSpan, i).repl, TRUE);
	g_array_free(spans, TRUE);
	g_free(text);
}

/* Grey the item out when there's nothing writable to edit. Refreshed each
 * time the Tools menu opens; connect_object ties the handler's life to the
 * item (the same pattern as the copy-path action). */
static void on_tools_menu_show(GtkWidget *menu, gpointer item)
{
	(void) menu;
	GeanyDocument *doc = document_get_current();
	gtk_widget_set_sensitive(GTK_WIDGET(item), doc != NULL && !doc->readonly);
}

void gwv_typography_create(GwvState *st)
{
	if (st->menu_typography != NULL)
		return;
	GtkWidget *tools_menu = st->plugin->geany_data->main_widgets->tools_menu;
	st->menu_typography = gtk_menu_item_new_with_mnemonic(_("_Simplify Typography (WVM)"));
	gtk_widget_set_tooltip_text(st->menu_typography,
		_("Replace symbols typical of LLM output (smart quotes, em dashes, "
		  "arrows, box drawing, invisible spaces) with plain ASCII — in the "
		  "selection, or the whole document when nothing is selected. One "
		  "undo step; pick the symbol groups in the plugin preferences."));
	g_signal_connect(st->menu_typography, "activate",
	                 G_CALLBACK(on_typography_activate), st);
	g_signal_connect_object(tools_menu, "show",
	                        G_CALLBACK(on_tools_menu_show), st->menu_typography, 0);
	gtk_widget_show(st->menu_typography);
	gtk_container_add(GTK_CONTAINER(tools_menu), st->menu_typography);
}

void gwv_typography_destroy(GwvState *st)
{
	if (st->menu_typography == NULL)
		return;
	gtk_widget_destroy(st->menu_typography);   /* also drops the menu 'show' handler */
	st->menu_typography = NULL;
}
