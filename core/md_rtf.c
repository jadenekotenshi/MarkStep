/* md_rtf.c -- the Markdown tree as RTF, for viewers and editors that read plain RTF (OpenWrite, Edit.app, WordPad ...).
 *
 * Plain RTF 1 only: a font table, a colour table, \pard paragraphs with indents and spacing, \b \i \ul \strike, groups,
 * \line, \tab. Nothing that needs OLE or fields. Characters beyond ASCII are written as \'hh in the chosen 8-bit
 * charset when it has them, otherwise as \uN with a look-alike ASCII fallback. */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"
#include "nsenc.h"

#define TW_QUOTE   360           /* indent a block quote adds */
#define TW_CODE    240           /* extra indent of code blocks */

typedef struct {
    MdBuf b;
    MdRtfOptions o;
    int fs_body;                 /* half-points */
    int fs_mono;
} R;

void md_rtf_defaults(MdRtfOptions *o)
{
    o->body_font = "Helvetica";
    o->mono_font = "Courier";
    o->body_pt = 12;
    o->mono_pt = 10;
    o->links = MD_LINKS_BOTH;
    o->html = MD_HTML_DROP;
    o->charset = MD_CHARSET_CP1252;
}

/* ------------------------------------------------------------------ characters */

static const unsigned short CP1252_HIGH[32] = {
    0x20AC, 0, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0, 0x017D, 0,
    0, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0, 0x017E, 0x0178
};

static int cp1252_byte(unsigned long cp)
{
    int i;

    if (cp >= 0xA0UL && cp <= 0xFFUL) {
        return (int)cp;
    }
    for (i = 0; i < 32; i++) {
        if (CP1252_HIGH[i] != 0 && CP1252_HIGH[i] == cp) {
            return 0x80 + i;
        }
    }
    return -1;
}

/* One ASCII character that stands in for cp where nothing better can be shown. */
static char ascii_fallback(unsigned long cp)
{
    if (cp >= 0x2010UL && cp <= 0x2015UL) {
        return '-';
    }
    if (cp == 0x2018UL || cp == 0x2019UL || cp == 0x201BUL || cp == 0x2032UL) {
        return '\'';
    }
    if (cp == 0x201CUL || cp == 0x201DUL || cp == 0x201FUL || cp == 0x2033UL) {
        return '"';
    }
    if (cp == 0x2022UL || cp == 0x25CFUL || cp == 0x25E6UL || cp == 0x2023UL) {
        return '*';
    }
    if (cp == 0x2026UL) {
        return '.';
    }
    if (cp == 0x2190UL || cp == 0x25C0UL) {
        return '<';
    }
    if (cp == 0x2192UL || cp == 0x25B6UL) {
        return '>';
    }
    if (cp == 0x2191UL) {
        return '^';
    }
    if (cp == 0x2193UL) {
        return 'v';
    }
    if (cp == 0x2713UL || cp == 0x2714UL) {
        return 'v';
    }
    if (cp == 0x2717UL || cp == 0x2718UL || cp == 0x00D7UL) {
        return 'x';
    }
    if (cp == 0x00A0UL || cp == 0x2002UL || cp == 0x2003UL || cp == 0x2009UL) {
        return ' ';
    }
    return '?';
}

static void put_hex_byte(R *r, int v)
{
    static const char hex[] = "0123456789abcdef";

    md_buf_puts(&r->b, "\\'");
    md_buf_putc(&r->b, hex[(v >> 4) & 15]);
    md_buf_putc(&r->b, hex[v & 15]);
}

static void put_u(R *r, unsigned long unit, char fallback)
{
    long v = (long)unit;

    if (v > 32767L) {
        v -= 65536L;
    }
    md_buf_puts(&r->b, "\\u");
    md_buf_putint(&r->b, v);
    md_buf_putc(&r->b, fallback);
}

static void put_cp(R *r, unsigned long cp)
{
    int byte = -1;

    if (cp == '\\' || cp == '{' || cp == '}') {
        md_buf_putc(&r->b, '\\');
        md_buf_putc(&r->b, (int)cp);
        return;
    }
    if (cp == '\t') {
        md_buf_puts(&r->b, "\\tab ");
        return;
    }
    if (cp == '\n') {
        md_buf_puts(&r->b, "\\line ");
        return;
    }
    if (cp < 0x20) {
        return;
    }
    if (cp < 0x7F) {
        md_buf_putc(&r->b, (int)cp);
        return;
    }
    if (cp == 0x7F) {
        return;
    }
    if (cp == 0xA0UL) {
        md_buf_puts(&r->b, "\\~");
        return;
    }
    if (r->o.charset == MD_CHARSET_CP1252) {
        byte = cp1252_byte(cp);
    } else if (r->o.charset == MD_CHARSET_NEXTSTEP && cp <= 0xFFFFUL) {
        byte = nsenc_encode((unsigned short)cp);
    }
    if (byte >= 0x80) {
        put_hex_byte(r, byte);
    } else if (cp > 0xFFFFUL) {
        unsigned long v = cp - 0x10000UL;

        put_u(r, 0xD800UL + (v >> 10), '?');
        put_u(r, 0xDC00UL + (v & 0x3FFUL), '?');
    } else {
        put_u(r, cp, ascii_fallback(cp));
    }
}

static void put_text(R *r, const char *s, size_t n)
{
    size_t i = 0, adv;

    while (i < n) {
        unsigned long cp = md_utf8_decode(s + i, n - i, &adv);

        if (adv == 0) {
            break;
        }
        put_cp(r, cp);
        i += adv;
    }
}

static void put_str(R *r, const char *s)
{
    put_text(r, s, strlen(s));
}

/* ------------------------------------------------------------------ plain text of a subtree (table cells, comparisons, alt text) */

static void plain(MdBuf *b, const MdNode *n)
{
    const MdNode *c;

    for (c = n->first_child; c != NULL; c = c->next) {
        switch (c->type) {
        case MD_TEXT:
        case MD_CODE:
            md_buf_put(b, c->text, c->text_len);
            break;
        case MD_SOFTBREAK:
        case MD_LINEBREAK:
            md_buf_putc(b, ' ');
            break;
        case MD_HTML_INLINE:
            break;
        default:
            plain(b, c);
            break;
        }
    }
}

/* ------------------------------------------------------------------ inlines */

static void inlines(R *r, const MdNode *parent);

static int is_br_tag(const char *s, size_t n)
{
    size_t i;

    if (n < 4 || s[0] != '<' || (s[1] != 'b' && s[1] != 'B') || (s[2] != 'r' && s[2] != 'R')) {
        return 0;
    }
    i = 3;
    while (i < n && (s[i] == ' ' || s[i] == '/')) {
        i++;
    }
    return i == n - 1 && s[i] == '>';
}

static void link_tail(R *r, const char *url, const MdNode *n)
{
    MdBuf pt;
    int same;
    const char *u = url;

    md_buf_init(&pt);
    plain(&pt, n);
    if (strncmp(u, "mailto:", 7) == 0) {
        u += 7;
    }
    same = pt.p != NULL && strcmp(pt.p, u) == 0;
    md_buf_free(&pt);
    if (!same && url[0] != '\0') {
        md_buf_puts(&r->b, "{\\cf3  (");
        put_str(r, url);
        md_buf_puts(&r->b, ")}");
    }
}

static void mono_open(R *r)
{
    md_buf_puts(&r->b, "{\\f1\\fs");
    md_buf_putint(&r->b, r->fs_mono);
    md_buf_putc(&r->b, ' ');
}

static void inline_node(R *r, const MdNode *n)
{
    switch (n->type) {
    case MD_TEXT:
        put_text(r, n->text, n->text_len);
        break;
    case MD_SOFTBREAK:
        md_buf_putc(&r->b, ' ');
        break;
    case MD_LINEBREAK:
        md_buf_puts(&r->b, "\\line ");
        break;
    case MD_CODE:
        mono_open(r);
        put_text(r, n->text, n->text_len);
        md_buf_putc(&r->b, '}');
        break;
    case MD_HTML_INLINE:
        if (r->o.html == MD_HTML_TEXT) {
            mono_open(r);
            put_text(r, n->text, n->text_len);
            md_buf_putc(&r->b, '}');
        } else if (is_br_tag(n->text, n->text_len)) {
            md_buf_puts(&r->b, "\\line ");
        }
        break;
    case MD_EMPH:
        md_buf_puts(&r->b, "{\\i ");
        inlines(r, n);
        md_buf_putc(&r->b, '}');
        break;
    case MD_STRONG:
        md_buf_puts(&r->b, "{\\b ");
        inlines(r, n);
        md_buf_putc(&r->b, '}');
        break;
    case MD_STRIKE:
        md_buf_puts(&r->b, "{\\strike ");
        inlines(r, n);
        md_buf_putc(&r->b, '}');
        break;
    case MD_LINK:
        md_buf_puts(&r->b, "{\\ul\\cf2 ");
        if (r->o.links == MD_LINKS_URL && n->url[0] != '\0') {
            put_str(r, n->url);
        } else {
            inlines(r, n);
        }
        md_buf_putc(&r->b, '}');
        if (r->o.links == MD_LINKS_BOTH) {
            link_tail(r, n->url, n);
        }
        break;
    case MD_IMAGE: {
        MdBuf alt;

        md_buf_init(&alt);
        plain(&alt, n);
        md_buf_puts(&r->b, "{\\i [image");
        if (alt.len > 0) {
            md_buf_puts(&r->b, ": ");
            put_text(r, alt.p, alt.len);
        }
        md_buf_puts(&r->b, "]}");
        md_buf_free(&alt);
        if (r->o.links != MD_LINKS_TEXT && n->url[0] != '\0') {
            md_buf_puts(&r->b, "{\\cf3  (");
            put_str(r, n->url);
            md_buf_puts(&r->b, ")}");
        }
        break;
    }
    default:
        inlines(r, n);
        break;
    }
}

static void inlines(R *r, const MdNode *parent)
{
    const MdNode *c;

    for (c = parent->first_child; c != NULL; c = c->next) {
        inline_node(r, c);
    }
}

/* ------------------------------------------------------------------ blocks */

typedef struct {
    int indent;                  /* twips from the left margin */
    int quote;                   /* inside a block quote: set in grey */
    int depth;                   /* how many lists deep */
    int tight;                   /* the paragraph is a direct child of a tight list's item */
    int last_in_top_list;
} Ctx;

static void para_open(R *r, int li, int fi, int sb, int sa, int quote, int mono, int fs)
{
    md_buf_puts(&r->b, "\\pard\\plain\\f");
    md_buf_putc(&r->b, mono ? '1' : '0');
    md_buf_puts(&r->b, "\\fs");
    md_buf_putint(&r->b, fs);
    md_buf_puts(&r->b, "\\li");
    md_buf_putint(&r->b, li);
    md_buf_puts(&r->b, "\\fi");
    md_buf_putint(&r->b, fi);
    md_buf_puts(&r->b, "\\sb");
    md_buf_putint(&r->b, sb);
    md_buf_puts(&r->b, "\\sa");
    md_buf_putint(&r->b, sa);
    if (quote) {
        md_buf_puts(&r->b, "\\cf3");
    }
    md_buf_putc(&r->b, ' ');
}

static void para_close(R *r)
{
    md_buf_puts(&r->b, "\\par\n");
}

static void block(R *r, const MdNode *n, const Ctx *cx);

static void blocks(R *r, const MdNode *parent, const Ctx *cx)
{
    const MdNode *c;

    for (c = parent->first_child; c != NULL; c = c->next) {
        block(r, c, cx);
    }
}

static void code_lines(R *r, const char *text, size_t len, const Ctx *cx)
{
    size_t i, start = 0;
    int first = 1;

    while (len > 0 && text[len - 1] == '\n') {
        len--;
    }
    para_open(r, cx->indent + TW_CODE, 0, 0, 120, cx->quote, 1, r->fs_mono);
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            if (!first) {
                md_buf_puts(&r->b, "\\line ");
            }
            put_text(r, text + start, i - start);
            first = 0;
            start = i + 1;
        }
    }
    para_close(r);
}

static int ucount(const char *s, size_t n)
{
    size_t i;
    int c = 0;

    for (i = 0; i < n; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) {
            c++;
        }
    }
    return c;
}

#define MAXC 64

static void table_block(R *r, const MdNode *t, const Ctx *cx)
{
    const MdNode *row, *cell;
    int ncols = 0, nrows = 0, widths[MAXC], aligns[MAXC], i, j;
    MdBuf *cells;

    for (row = t->first_child; row != NULL; row = row->next) {
        nrows++;
    }
    if (nrows == 0 || t->first_child->first_child == NULL) {
        return;
    }
    for (cell = t->first_child->first_child; cell != NULL && ncols < MAXC; cell = cell->next) {
        aligns[ncols] = cell->align;
        widths[ncols] = 3;
        ncols++;
    }
    cells = (MdBuf *)malloc((size_t)nrows * (size_t)ncols * sizeof(MdBuf));
    if (cells == NULL) {
        return;
    }
    i = 0;
    for (row = t->first_child; row != NULL; row = row->next, i++) {
        cell = row->first_child;
        for (j = 0; j < ncols; j++) {
            MdBuf *b = &cells[i * ncols + j];

            md_buf_init(b);
            if (cell != NULL) {
                int w;

                plain(b, cell);
                w = ucount(b->p ? b->p : "", b->len);
                if (w > widths[j]) {
                    widths[j] = w;
                }
                cell = cell->next;
            }
        }
    }
    para_open(r, cx->indent + TW_CODE, 0, 0, 120, cx->quote, 1, r->fs_mono);
    for (i = 0; i < nrows; i++) {
        if (i > 0) {
            md_buf_puts(&r->b, "\\line ");
        }
        md_buf_putc(&r->b, '|');
        for (j = 0; j < ncols; j++) {
            MdBuf *b = &cells[i * ncols + j];
            int w = ucount(b->p ? b->p : "", b->len), pad = widths[j] - w, left = 0, right = pad, k;

            if (aligns[j] == 3) {
                left = pad;
                right = 0;
            } else if (aligns[j] == 2) {
                left = pad / 2;
                right = pad - left;
            }
            md_buf_putc(&r->b, ' ');
            for (k = 0; k < left; k++) {
                md_buf_putc(&r->b, ' ');
            }
            put_text(r, b->p ? b->p : "", b->len);
            for (k = 0; k < right; k++) {
                md_buf_putc(&r->b, ' ');
            }
            md_buf_puts(&r->b, " |");
        }
        if (i == 0 && nrows > 1) {                                          /* the rule under the header row */
            md_buf_puts(&r->b, "\\line |");
            for (j = 0; j < ncols; j++) {
                int k;

                for (k = 0; k < widths[j] + 2; k++) {
                    md_buf_putc(&r->b, '-');
                }
                md_buf_putc(&r->b, '|');
            }
        }
    }
    para_close(r);
    for (i = 0; i < nrows * ncols; i++) {
        md_buf_free(&cells[i]);
    }
    free(cells);
}

static void put_marker(R *r, const MdNode *list, const MdNode *item, int number, int depth)
{
    MdBuf mk;

    md_buf_init(&mk);
    if (item->checked >= 0) {
        md_buf_puts(&mk, item->checked ? "[x]" : "[ ]");
    } else if (list->ordered) {
        md_buf_putint(&mk, number);
        md_buf_putc(&mk, list->delim == ')' ? ')' : '.');
    }
    if (mk.len > 0) {
        put_text(r, mk.p, mk.len);
    } else {
        put_cp(r, depth % 2 == 0 ? 0x2022UL : 0x2013UL);
    }
    md_buf_free(&mk);
    md_buf_puts(&r->b, "\\tab ");
}

static void item_block(R *r, const MdNode *list, const MdNode *item, int number, int is_last, const Ctx *cx)
{
    int mw = list->ordered ? 480 : 360;
    int at_end = cx->depth == 0 ? is_last : (cx->last_in_top_list && is_last);    /* nothing follows the top-level list after this item */
    Ctx in;
    const MdNode *c = item->first_child;

    in = *cx;
    in.indent = cx->indent + mw;
    in.depth = cx->depth + 1;
    in.tight = list->tight;
    if (c != NULL && c->type == MD_PARAGRAPH) {
        int sa = (list->tight && !(at_end && c->next == NULL)) ? 40 : 120;

        para_open(r, in.indent, -mw, 0, sa, cx->quote, 0, r->fs_body);
        md_buf_puts(&r->b, "\\tx");
        md_buf_putint(&r->b, in.indent);
        md_buf_putc(&r->b, ' ');
        put_marker(r, list, item, number, cx->depth);
        inlines(r, c);
        para_close(r);
        c = c->next;
    } else {
        para_open(r, in.indent, -mw, 0, 40, cx->quote, 0, r->fs_body);
        md_buf_puts(&r->b, "\\tx");
        md_buf_putint(&r->b, in.indent);
        md_buf_putc(&r->b, ' ');
        put_marker(r, list, item, number, cx->depth);
        para_close(r);
    }
    for (; c != NULL; c = c->next) {
        in.last_in_top_list = at_end && c->next == NULL;
        block(r, c, &in);
    }
}

static void block(R *r, const MdNode *n, const Ctx *cx)
{
    switch (n->type) {
    case MD_PARAGRAPH:
        para_open(r, cx->indent, 0, 0, (cx->tight && !cx->last_in_top_list) ? 40 : 120, cx->quote, 0, r->fs_body);
        inlines(r, n);
        para_close(r);
        break;
    case MD_HEADING: {
        static const int scale[7] = { 0, 180, 150, 125, 110, 100, 90 };      /* percent of the body size */
        int fs = (r->fs_body * scale[n->level] / 100) / 2 * 2;                  /* whole points */

        para_open(r, cx->indent, 0, 200, 100, cx->quote, 0, fs);
        md_buf_puts(&r->b, "\\b ");
        inlines(r, n);
        para_close(r);
        break;
    }
    case MD_BLOCKQUOTE: {
        Ctx in;

        in = *cx;
        in.indent += TW_QUOTE;
        in.quote = 1;
        in.tight = 0;
        blocks(r, n, &in);
        break;
    }
    case MD_LIST: {
        const MdNode *item;
        int num = n->start, count = 0, idx = 0;
        Ctx top;

        top = *cx;
        top.tight = 0;
        for (item = n->first_child; item != NULL; item = item->next) {
            count++;
        }
        for (item = n->first_child; item != NULL; item = item->next, num++) {
            idx++;
            item_block(r, n, item, num, idx == count, &top);
        }
        break;
    }
    case MD_CODE_BLOCK:
        code_lines(r, n->text, n->text_len, cx);
        break;
    case MD_HTML_BLOCK:
        if (r->o.html == MD_HTML_TEXT) {
            code_lines(r, n->text, n->text_len, cx);
        }
        break;
    case MD_THEMATIC_BREAK: {
        int k;

        para_open(r, cx->indent, 0, 60, 120, 1, 0, r->fs_body);
        md_buf_puts(&r->b, "\\qc ");
        for (k = 0; k < 24; k++) {
            put_cp(r, 0x2014UL);
        }
        para_close(r);
        break;
    }
    case MD_TABLE:
        table_block(r, n, cx);
        break;
    default:
        blocks(r, n, cx);
        break;
    }
}

char *md_to_rtf(const MdDoc *doc, const MdRtfOptions *opt, size_t *len)
{
    R r;
    Ctx cx;
    MdRtfOptions def;

    if (opt == NULL) {
        md_rtf_defaults(&def);
        opt = &def;
    }
    memset(&r, 0, sizeof(r));
    md_buf_init(&r.b);
    r.o = *opt;
    if (r.o.body_font == NULL) {
        r.o.body_font = "Helvetica";
    }
    if (r.o.mono_font == NULL) {
        r.o.mono_font = "Courier";
    }
    if (r.o.body_pt < 6) {
        r.o.body_pt = 12;
    }
    if (r.o.mono_pt < 6) {
        r.o.mono_pt = 10;
    }
    r.fs_body = r.o.body_pt * 2;
    r.fs_mono = r.o.mono_pt * 2;
    memset(&cx, 0, sizeof(cx));
    md_buf_puts(&r.b, "{\\rtf1\\ansi\\deff0\\deftab720\n{\\fonttbl{\\f0\\fswiss ");
    md_buf_puts(&r.b, r.o.body_font);
    md_buf_puts(&r.b, ";}{\\f1\\fmodern ");
    md_buf_puts(&r.b, r.o.mono_font);
    md_buf_puts(&r.b, ";}}\n{\\colortbl;\\red0\\green0\\blue0;\\red0\\green0\\blue200;\\red100\\green100\\blue100;}\n");
    blocks(&r, doc->root, &cx);
    md_buf_puts(&r.b, "}\n");
    return md_buf_finish(&r.b, len);
}
