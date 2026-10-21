/* md_block.c -- the block structure of a Markdown document: containers (block quotes, lists), leaves (paragraphs, headings,
 * code, HTML, tables), one line at a time, following CommonMark's own algorithm: see which open blocks the line continues,
 * see which new blocks it starts, then add what is left to the right block. */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"

typedef struct {
    MdDoc *doc;
    MdNode *root;
    MdNode *tip;                 /* the deepest open block */
    int lineno;
    const char *l;               /* the current line, tabs expanded, no line ending */
    size_t len;
    size_t offset;               /* how much of it the open containers have used */
    size_t fns;                  /* the first non-space at or after offset */
    int indent;                  /* fns - offset */
    int blank;
    int closed_by_fence;         /* the line was a closing code fence: nothing more to do with it */
} P;

static int accepts_lines(MdType t)
{
    return t == MD_PARAGRAPH || t == MD_CODE_BLOCK || t == MD_HTML_BLOCK;
}

static int can_contain(MdType parent, MdType child)
{
    switch (parent) {
    case MD_DOCUMENT:
    case MD_BLOCKQUOTE:
    case MD_ITEM:
        return child != MD_ITEM;
    case MD_LIST:
        return child == MD_ITEM;
    case MD_TABLE:
        return child == MD_TABLE_ROW;
    case MD_TABLE_ROW:
        return child == MD_TABLE_CELL;
    default:
        return 0;
    }
}

static void find_first_nonspace(P *p)
{
    p->fns = p->offset;
    while (p->fns < p->len && p->l[p->fns] == ' ') {
        p->fns++;
    }
    p->indent = (int)(p->fns - p->offset);
    p->blank = p->fns >= p->len;
}

static int is_blank_text(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] != ' ' && s[i] != '\n' && s[i] != '\r') {
            return 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ finalizing */

static int ends_with_blank_line(MdNode *n)
{
    while (n != NULL) {
        if (n->last_line_blank) {
            return 1;
        }
        n = (n->type == MD_LIST || n->type == MD_ITEM) ? n->last_child : NULL;
    }
    return 0;
}

static void finalize_list(MdNode *list)
{
    MdNode *item, *sub;

    list->tight = 1;
    for (item = list->first_child; item != NULL; item = item->next) {
        if (ends_with_blank_line(item) && item->next) {
            list->tight = 0;
            return;
        }
        for (sub = item->first_child; sub != NULL; sub = sub->next) {
            if (ends_with_blank_line(sub) && (item->next || sub->next)) {
                list->tight = 0;
                return;
            }
        }
    }
}

static MdNode *finalize(P *p, MdNode *n)
{
    MdNode *parent = n->parent;
    MdDoc *d = p->doc;

    if (!n->open) {
        return parent;
    }
    n->open = 0;
    switch (n->type) {
    case MD_PARAGRAPH: {
        size_t used = md_scan_ref_defs(d, n->content ? n->content : "", n->content_len);

        if (used > 0) {
            memmove(n->content, n->content + used, n->content_len - used + 1);
            n->content_len -= used;
        }
        if (n->content_len == 0 || is_blank_text(n->content, n->content_len)) {
            md_node_free_content(n);
            md_unlink(n);
        }
        break;
    }
    case MD_CODE_BLOCK:
        if (!n->fenced && n->content != NULL) {                       /* an indented block loses its trailing blank lines */
            size_t e = n->content_len;

            while (e > 0) {
                size_t ls = e - 1, k;
                int blank_line = 1;

                while (ls > 0 && n->content[ls - 1] != '\n') {
                    ls--;
                }
                for (k = ls; k + 1 < e; k++) {
                    if (n->content[k] != ' ') {
                        blank_line = 0;
                        break;
                    }
                }
                if (!blank_line) {
                    break;
                }
                e = ls;
            }
            n->content_len = e;
            if (n->content != NULL) {
                n->content[e] = '\0';
            }
        }
        n->text = md_strndup(d, n->content ? n->content : "", n->content_len);
        n->text_len = n->content_len;
        md_node_free_content(n);
        break;
    case MD_HTML_BLOCK:
        n->text = md_strndup(d, n->content ? n->content : "", n->content_len);
        n->text_len = n->content_len;
        md_node_free_content(n);
        break;
    case MD_LIST:
        finalize_list(n);
        break;
    default:
        break;
    }
    return parent;
}

#define MAX_CONTAINER_DEPTH 64           /* block quotes, lists and items nested deeper than this are not made */

static int depth_of(const MdNode *n)
{
    int d = 0;

    for (; n != NULL; n = n->parent) {
        d++;
    }
    return d;
}

static MdNode *add_child(P *p, MdNode *parent, MdType type)
{
    MdNode *c;

    while (!can_contain(parent->type, type)) {
        parent = finalize(p, parent);
    }
    c = md_new_node(p->doc, type);
    if (c == NULL) {
        return parent;
    }
    c->open = 1;
    c->start_line = p->lineno;
    md_append_child(parent, c);
    return c;
}

/* ------------------------------------------------------------------ scanners */

static int is_thematic_break(const char *s, size_t n, size_t i)
{
    int ch = 0, count = 0;

    for (; i < n; i++) {
        if (s[i] == '*' || s[i] == '-' || s[i] == '_') {
            if (ch == 0) {
                ch = s[i];
            } else if (s[i] != ch) {
                return 0;
            }
            count++;
        } else if (s[i] != ' ') {
            return 0;
        }
    }
    return count >= 3;
}

static int is_setext_underline(const char *s, size_t n, size_t i)      /* returns '=' or '-' or 0 */
{
    int ch;
    size_t j;

    if (i >= n || (s[i] != '=' && s[i] != '-')) {
        return 0;
    }
    ch = s[i];
    for (j = i; j < n && s[j] == ch; j++) {
        ;
    }
    for (; j < n; j++) {
        if (s[j] != ' ') {
            return 0;
        }
    }
    return ch;
}

/* ATX heading: returns the level (1..6) and sets cstart and cend to the content, or 0. */
static int scan_atx(const char *s, size_t n, size_t i, size_t *cstart, size_t *cend)
{
    int level = 0;
    size_t e;

    while (i < n && s[i] == '#' && level < 7) {
        level++;
        i++;
    }
    if (level < 1 || level > 6) {
        return 0;
    }
    if (i < n && s[i] != ' ') {
        return 0;
    }
    while (i < n && s[i] == ' ') {
        i++;
    }
    e = n;
    while (e > i && s[e - 1] == ' ') {
        e--;
    }
    {
        size_t k = e;

        while (k > i && s[k - 1] == '#') {
            k--;
        }
        if (k < e) {                                      /* a closing sequence: preceded by a space, or the whole content */
            if (k == i) {
                e = i;
            } else if (s[k - 1] == ' ') {
                e = k;
                while (e > i && s[e - 1] == ' ') {
                    e--;
                }
            }
        }
    }
    *cstart = i;
    *cend = e;
    return level;
}

/* A code fence: returns its length (>= 3) and sets *ch, or 0. A backtick fence's info string may not contain a backtick. */
static int scan_fence(const char *s, size_t n, size_t i, int *ch)
{
    size_t j = i;
    int c;

    if (i >= n || (s[i] != '`' && s[i] != '~')) {
        return 0;
    }
    c = s[i];
    while (j < n && s[j] == c) {
        j++;
    }
    if (j - i < 3) {
        return 0;
    }
    if (c == '`') {
        size_t k;

        for (k = j; k < n; k++) {
            if (s[k] == '`') {
                return 0;
            }
        }
    }
    *ch = c;
    return (int)(j - i);
}

static int is_closing_fence(const char *s, size_t n, size_t i, int ch, int minlen)
{
    size_t j = i;

    while (j < n && s[j] == ch) {
        j++;
    }
    if ((int)(j - i) < minlen) {
        return 0;
    }
    for (; j < n; j++) {
        if (s[j] != ' ') {
            return 0;
        }
    }
    return 1;
}

static const char *const BLOCK_TAGS[] = {
    "address", "article", "aside", "base", "basefont", "blockquote", "body", "caption", "center", "col", "colgroup", "dd",
    "details", "dialog", "dir", "div", "dl", "dt", "fieldset", "figcaption", "figure", "footer", "form", "frame", "frameset",
    "h1", "h2", "h3", "h4", "h5", "h6", "head", "header", "hr", "html", "iframe", "legend", "li", "link", "main", "menu",
    "menuitem", "nav", "noframes", "ol", "optgroup", "option", "p", "param", "section", "source", "summary", "table", "tbody",
    "td", "tfoot", "th", "thead", "title", "tr", "track", "ul", NULL
};

static int tag_name_is(const char *s, size_t n, size_t i, const char *name, size_t *after)
{
    size_t l = strlen(name);

    if (i + l > n || md_strncasecmp(s + i, name, l) != 0) {
        return 0;
    }
    *after = i + l;
    return 1;
}

/* The HTML block start condition (1..7) the line meets at position i, or 0. Condition 7 may not interrupt a paragraph. */
static int html_block_start(const char *s, size_t n, size_t i, int in_paragraph)
{
    size_t a, k;
    static const char *const raw[] = { "script", "pre", "style", "textarea", NULL };

    if (i >= n || s[i] != '<') {
        return 0;
    }
    for (k = 0; raw[k] != NULL; k++) {
        if (tag_name_is(s, n, i + 1, raw[k], &a) && (a >= n || s[a] == ' ' || s[a] == '>')) {
            return 1;
        }
    }
    if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
        return 2;
    }
    if (i + 1 < n && s[i + 1] == '?') {
        return 3;
    }
    if (i + 2 < n && s[i + 1] == '!' && s[i + 2] >= 'A' && s[i + 2] <= 'Z') {
        return 4;
    }
    if (i + 8 < n && memcmp(s + i, "<![CDATA[", 9) == 0) {
        return 5;
    }
    {
        size_t b = i + 1;

        if (b < n && s[b] == '/') {
            b++;
        }
        for (k = 0; BLOCK_TAGS[k] != NULL; k++) {
            if (tag_name_is(s, n, b, BLOCK_TAGS[k], &a) && (a >= n || s[a] == ' ' || s[a] == '>' || (s[a] == '/' && a + 1 < n && s[a + 1] == '>'))) {
                return 6;
            }
        }
    }
    if (!in_paragraph) {
        size_t e = md_scan_html_tag(s, n, i);

        if (e > 0 && is_blank_text(s + e, n - e)) {
            return 7;
        }
    }
    return 0;
}

static int html_block_ends(int type, const char *s, size_t n)
{
    size_t i;

    switch (type) {
    case 1:
        for (i = 0; i + 1 < n; i++) {
            if (s[i] == '<' && s[i + 1] == '/') {
                size_t a;

                if (tag_name_is(s, n, i + 2, "script", &a) || tag_name_is(s, n, i + 2, "pre", &a) ||
                    tag_name_is(s, n, i + 2, "style", &a) || tag_name_is(s, n, i + 2, "textarea", &a)) {
                    if (a < n && s[a] == '>') {
                        return 1;
                    }
                }
            }
        }
        return 0;
    case 2:
        for (i = 0; i + 2 < n; i++) {
            if (s[i] == '-' && s[i + 1] == '-' && s[i + 2] == '>') {
                return 1;
            }
        }
        return 0;
    case 3:
        for (i = 0; i + 1 < n; i++) {
            if (s[i] == '?' && s[i + 1] == '>') {
                return 1;
            }
        }
        return 0;
    case 4:
        for (i = 0; i < n; i++) {
            if (s[i] == '>') {
                return 1;
            }
        }
        return 0;
    case 5:
        for (i = 0; i + 2 < n; i++) {
            if (s[i] == ']' && s[i + 1] == ']' && s[i + 2] == '>') {
                return 1;
            }
        }
        return 0;
    default:
        return 0;
    }
}

/* A list marker at p->fns. Returns the marker's width (0 if none) and fills the rest. */
static int scan_list_marker(P *p, int interrupts_paragraph, int *ordered, int *start, char *delim, int *padding)
{
    const char *s = p->l;
    size_t n = p->len, i = p->fns, j;
    int marker_len, spaces;

    if (i >= n) {
        return 0;
    }
    if (s[i] == '*' || s[i] == '-' || s[i] == '+') {
        *ordered = 0;
        *start = 1;
        *delim = s[i];
        j = i + 1;
    } else if (s[i] >= '0' && s[i] <= '9') {
        long v = 0;
        int digits = 0;

        j = i;
        while (j < n && s[j] >= '0' && s[j] <= '9' && digits < 10) {
            v = v * 10 + (s[j] - '0');
            digits++;
            j++;
        }
        if (digits > 9 || j >= n || (s[j] != '.' && s[j] != ')')) {
            return 0;
        }
        *ordered = 1;
        *start = (int)v;
        *delim = s[j];
        j++;
    } else {
        return 0;
    }
    if (j < n && s[j] != ' ') {
        return 0;
    }
    if (interrupts_paragraph) {
        size_t k = j;

        while (k < n && s[k] == ' ') {
            k++;
        }
        if (k >= n) {
            return 0;                                          /* an empty item cannot interrupt a paragraph */
        }
        if (*ordered && *start != 1) {
            return 0;
        }
    }
    marker_len = (int)(j - i);
    spaces = 0;
    while (j + (size_t)spaces < n && s[j + (size_t)spaces] == ' ') {
        spaces++;
    }
    if (j + (size_t)spaces >= n || spaces >= 5) {
        *padding = marker_len + 1;
    } else {
        *padding = marker_len + spaces;
    }
    return marker_len;
}

static int lists_match(const MdNode *list, int ordered, char delim)
{
    return list->ordered == ordered && list->delim == delim;
}

/* ------------------------------------------------------------------ tables */

/* Splits a table row into trimmed cells; returns how many, and fills starts/ends (up to max). */
static int split_row(const char *s, size_t n, size_t starts[], size_t ends[], int max)
{
    size_t i = 0, cs;
    int count = 0;

    while (i < n && s[i] == ' ') {
        i++;
    }
    while (n > i && s[n - 1] == ' ') {
        n--;
    }
    if (i < n && s[i] == '|') {
        i++;
    }
    cs = i;
    while (i <= n) {
        if (i == n || (s[i] == '|' && (i == 0 || s[i - 1] != '\\'))) {
            if (i == n && cs == n && count > 0) {
                break;                                          /* nothing after the final pipe */
            }
            if (count < max) {
                size_t a = cs, b = i;

                while (a < b && s[a] == ' ') {
                    a++;
                }
                while (b > a && s[b - 1] == ' ') {
                    b--;
                }
                starts[count] = a;
                ends[count] = b;
            }
            count++;
            cs = i + 1;
        }
        i++;
    }
    return count;
}

#define MAX_COLS 64

/* Is the line a table delimiter row (| --- | :-: |)? Fills the alignments and returns the number of cells (0 if not). */
static int scan_delimiter_row(const char *s, size_t n, size_t i, int aligns[])
{
    size_t st[MAX_COLS], en[MAX_COLS];
    int cells, k, any_pipe = 0;
    size_t j;

    for (j = i; j < n; j++) {
        if (s[j] == '|') {
            any_pipe = 1;
        } else if (s[j] != ' ' && s[j] != '-' && s[j] != ':') {
            return 0;
        }
    }
    if (!any_pipe) {
        return 0;
    }
    cells = split_row(s + i, n - i, st, en, MAX_COLS);
    if (cells < 1 || cells > MAX_COLS) {
        return 0;
    }
    for (k = 0; k < cells; k++) {
        size_t a = st[k], b = en[k];
        int left = 0, right = 0;

        if (a < b && s[i + a] == ':') {
            left = 1;
            a++;
        }
        if (b > a && s[i + b - 1] == ':') {
            right = 1;
            b--;
        }
        if (b <= a) {
            return 0;
        }
        for (j = a; j < b; j++) {
            if (s[i + j] != '-') {
                return 0;
            }
        }
        aligns[k] = left && right ? 2 : left ? 1 : right ? 3 : 0;
    }
    return cells;
}

static void unescape_pipes(MdBuf *out, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == '|') {
            continue;
        }
        md_buf_putc(out, s[i]);
    }
}

static void add_table_row(P *p, MdNode *table, const char *s, size_t n, int header, const int aligns[], int ncols)
{
    size_t st[MAX_COLS], en[MAX_COLS];
    int cells = split_row(s, n, st, en, MAX_COLS), k;
    MdNode *row = md_new_node(p->doc, MD_TABLE_ROW);

    if (row == NULL) {
        return;
    }
    row->align = header;
    md_append_child(table, row);
    for (k = 0; k < ncols; k++) {
        MdNode *cell = md_new_node(p->doc, MD_TABLE_CELL);
        MdBuf b;

        if (cell == NULL) {
            return;
        }
        cell->align = aligns[k];
        cell->level = header;
        md_append_child(row, cell);
        if (k < cells && k < MAX_COLS) {
            md_buf_init(&b);
            unescape_pipes(&b, s + st[k], en[k] - st[k]);
            if (!b.fail && b.len > 0) {
                md_node_add_line(cell, b.p, b.len);
            }
            md_buf_free(&b);
        }
    }
}

/* ------------------------------------------------------------------ one line */

/* A setext underline turns the paragraph in front of it into a heading, unless the paragraph is nothing but link definitions. */
static int can_setext(P *p, MdNode *para)
{
    size_t used;

    if (!is_setext_underline(p->l, p->len, p->fns) || para->content == NULL) {
        return 0;
    }
    used = md_scan_ref_defs(p->doc, para->content, para->content_len);
    return used < para->content_len && !is_blank_text(para->content + used, para->content_len - used);
}

static void advance_to_end(P *p)
{
    p->offset = p->len;
}

static void process_line(P *p, const char *line, size_t len)
{
    MdNode *container, *last_matched;
    int all_matched = 1;

    p->l = line;
    p->len = len;
    p->offset = 0;
    p->lineno++;
    p->closed_by_fence = 0;

    /* 1. which open blocks does the line continue? */
    container = p->root;
    last_matched = p->root;
    while (container->last_child && container->last_child->open) {
        MdNode *c = container->last_child;
        int matched = 0;

        container = c;
        find_first_nonspace(p);
        switch (c->type) {
        case MD_BLOCKQUOTE:
            if (p->indent <= 3 && p->fns < p->len && p->l[p->fns] == '>') {
                p->offset = p->fns + 1;
                if (p->offset < p->len && p->l[p->offset] == ' ') {
                    p->offset++;
                }
                matched = 1;
            }
            break;
        case MD_LIST:
            matched = 1;                                       /* a list as such always continues; its items decide */
            break;
        case MD_ITEM:
            if (p->indent >= c->marker_offset + c->padding) {
                p->offset += (size_t)(c->marker_offset + c->padding);
                matched = 1;
            } else if (p->blank && c->first_child != NULL) {
                p->offset = p->fns;
                matched = 1;
            }
            break;
        case MD_CODE_BLOCK:
            if (c->fenced) {
                if (p->indent <= 3 && is_closing_fence(p->l, p->len, p->fns, c->fence_char, c->fence_len)) {
                    container = finalize(p, c);
                    p->tip = container;
                    p->closed_by_fence = 1;
                    return;
                }
                {
                    int skip = c->fence_offset;

                    while (skip > 0 && p->offset < p->len && p->l[p->offset] == ' ') {
                        p->offset++;
                        skip--;
                    }
                }
                matched = 1;
            } else if (p->indent >= 4) {
                p->offset += 4;
                matched = 1;
            } else if (p->blank) {
                p->offset = p->fns;
                matched = 1;
            }
            break;
        case MD_HTML_BLOCK:
            matched = !(p->blank && (c->level == 6 || c->level == 7));
            break;
        case MD_PARAGRAPH:
            matched = !p->blank;
            break;
        case MD_TABLE:
            matched = !p->blank;
            break;
        default:
            matched = 0;                                       /* headings and thematic breaks take one line only */
            break;
        }
        if (!matched) {
            container = c->parent;
            all_matched = 0;
            break;
        }
        last_matched = container;
    }
    if (!all_matched) {
        container = last_matched;
    } else {
        last_matched = container;
    }

    /* 2. which new blocks does it start? */
    find_first_nonspace(p);
    while (container->type != MD_CODE_BLOCK && container->type != MD_HTML_BLOCK) {
        int indented;
        int ordered, start, padding, mlen, fch, flen, hb;
        char delim;
        size_t cs, ce;

        find_first_nonspace(p);
        indented = p->indent >= 4;

        if (depth_of(container) > MAX_CONTAINER_DEPTH && p->fns < p->len && (p->l[p->fns] == '>' || p->l[p->fns] == '-' ||
                                                                              p->l[p->fns] == '*' || p->l[p->fns] == '+' ||
                                                                              (p->l[p->fns] >= '0' && p->l[p->fns] <= '9'))) {
            break;                                              /* too deep already: what follows is text */
        } else if (!indented && p->fns < p->len && p->l[p->fns] == '>') {
            container = add_child(p, container, MD_BLOCKQUOTE);
            p->offset = p->fns + 1;
            if (p->offset < p->len && p->l[p->offset] == ' ') {
                p->offset++;
            }
        } else if (!indented && (hb = scan_atx(p->l, p->len, p->fns, &cs, &ce)) > 0) {
            container = add_child(p, container, MD_HEADING);
            container->level = hb;
            if (ce > cs) {
                md_node_add_line(container, p->l + cs, ce - cs);
            }
            advance_to_end(p);
        } else if (!indented && (flen = scan_fence(p->l, p->len, p->fns, &fch)) > 0) {
            size_t is = p->fns + (size_t)flen, ie = p->len;
            MdBuf ib;

            container = add_child(p, container, MD_CODE_BLOCK);
            container->fenced = 1;
            container->fence_char = fch;
            container->fence_len = flen;
            container->fence_offset = p->indent;
            while (is < ie && p->l[is] == ' ') {
                is++;
            }
            while (ie > is && p->l[ie - 1] == ' ') {
                ie--;
            }
            md_buf_init(&ib);
            md_unescape_into(&ib, p->l + is, ie - is, 1);
            container->info = md_strndup(p->doc, ib.p ? ib.p : "", ib.len);
            md_buf_free(&ib);
            advance_to_end(p);
        } else if (!indented && (hb = html_block_start(p->l, p->len, p->fns, container->type == MD_PARAGRAPH)) > 0) {
            container = add_child(p, container, MD_HTML_BLOCK);
            container->level = hb;                              /* (kept as the HTML block type) */
            /* the line itself is added with the rest below: leave offset where the containers left it */
        } else if (!indented && container->type == MD_PARAGRAPH && can_setext(p, container)) {
            int ch = is_setext_underline(p->l, p->len, p->fns);
            size_t used = md_scan_ref_defs(p->doc, container->content, container->content_len);

            if (used > 0) {
                memmove(container->content, container->content + used, container->content_len - used + 1);
                container->content_len -= used;
            }
            container->type = MD_HEADING;
            container->level = ch == '=' ? 1 : 2;
            while (container->content_len > 0 && (container->content[container->content_len - 1] == '\n' ||
                                                  container->content[container->content_len - 1] == ' ')) {
                container->content_len--;
            }
            container->content[container->content_len++] = '\n';
            container->content[container->content_len] = '\0';
            advance_to_end(p);
            p->tip = container;
        } else if (!indented && is_thematic_break(p->l, p->len, p->fns)) {
            container = add_child(p, container, MD_THEMATIC_BREAK);
            advance_to_end(p);
        } else if ((!indented || container->type == MD_LIST) && p->indent < 4 &&
                   (mlen = scan_list_marker(p, container->type == MD_PARAGRAPH, &ordered, &start, &delim, &padding)) > 0) {
            MdNode *item;
            int spaces_after = padding - mlen;

            if (container->type != MD_LIST || !lists_match(container, ordered, delim)) {
                container = add_child(p, container, MD_LIST);
                container->ordered = ordered;
                container->start = start;
                container->delim = delim;
            }
            item = add_child(p, container, MD_ITEM);
            item->marker_offset = p->indent;
            item->padding = padding;
            container = item;
            p->offset = p->fns + (size_t)mlen + (size_t)(spaces_after >= 5 ? 1 : (spaces_after > 0 ? spaces_after : 0));
            if (p->offset > p->len) {
                p->offset = p->len;
            }
        } else if (indented && p->tip->type != MD_PARAGRAPH && !p->blank) {
            p->offset += 4;
            container = add_child(p, container, MD_CODE_BLOCK);
        } else {
            break;
        }
        if (accepts_lines(container->type)) {
            break;
        }
    }

    /* the first line of a table: a paragraph of one line followed by a delimiter row turns into a table */
    if (!p->closed_by_fence && all_matched && container->type == MD_PARAGRAPH && container == p->tip) {
        int aligns[MAX_COLS], ncols;

        find_first_nonspace(p);
        ncols = (p->indent <= 3) ? scan_delimiter_row(p->l, p->len, p->fns, aligns) : 0;
        if (ncols > 0 && container->content_len > 0 &&
            memchr(container->content, '\n', container->content_len) == container->content + container->content_len - 1 &&
            memchr(container->content, '|', container->content_len) != NULL) {
            size_t st[MAX_COLS], en[MAX_COLS];
            int hc = split_row(container->content, container->content_len - 1, st, en, MAX_COLS);

            if (hc == ncols) {
                char *saved = (char *)malloc(container->content_len);

                if (saved != NULL) {
                    size_t slen = container->content_len - 1;

                    memcpy(saved, container->content, slen);
                    md_node_free_content(container);
                    container->type = MD_TABLE;
                    container->open = 1;
                    add_table_row(p, container, saved, slen, 1, aligns, ncols);
                    /* remember the alignments on the table's first row cells: later rows copy them from there */
                    free(saved);
                    advance_to_end(p);
                    p->tip = container;
                    return;
                }
            }
        }
    }

    /* 3. what is left of the line belongs to a block */
    find_first_nonspace(p);
    if (p->blank && container->last_child) {
        container->last_child->last_line_blank = 1;
    }
    {
        int llb = p->blank && container->type != MD_BLOCKQUOTE && container->type != MD_HEADING &&
                  container->type != MD_THEMATIC_BREAK && !(container->type == MD_CODE_BLOCK && container->fenced) &&
                  !(container->type == MD_ITEM && container->first_child == NULL && container->start_line == p->lineno);
        MdNode *t;

        container->last_line_blank = llb;
        for (t = container; t->parent; t = t->parent) {
            t->parent->last_line_blank = 0;
        }
    }

    if (p->tip != last_matched && container == last_matched && !p->blank && p->tip->type == MD_PARAGRAPH) {
        /* a lazy continuation line of a paragraph */
        md_node_add_line(p->tip, p->l + p->fns, p->len - p->fns);
        return;
    }
    while (p->tip != last_matched) {
        p->tip = finalize(p, p->tip);
    }
    if (container->type == MD_CODE_BLOCK) {
        if (!(container->fenced && container->start_line == p->lineno)) {        /* (not the fence line itself) */
            md_node_add_line(container, p->l + p->offset, p->len - p->offset);
        }
    } else if (container->type == MD_HTML_BLOCK) {
        md_node_add_line(container, p->l + p->offset, p->len - p->offset);
        if (container->level >= 1 && container->level <= 5 && html_block_ends(container->level, p->l + p->offset, p->len - p->offset)) {
            container = finalize(p, container);
        }
    } else if (container->type == MD_TABLE) {
        if (!p->blank) {
            const MdNode *hdr = container->first_child ? container->first_child->first_child : NULL;
            int aligns[MAX_COLS], ncols = 0;
            const MdNode *c;

            for (c = hdr; c != NULL && ncols < MAX_COLS; c = c->next) {
                aligns[ncols++] = c->align;
            }
            add_table_row(p, container, p->l + p->fns, p->len - p->fns, 0, aligns, ncols);
        }
    } else if (p->blank) {
        /* nothing to add */
    } else if (accepts_lines(container->type) || container->type == MD_HEADING) {
        md_node_add_line(container, p->l + p->fns, p->len - p->fns);
    } else {
        container = add_child(p, container, MD_PARAGRAPH);
        md_node_add_line(container, p->l + p->fns, p->len - p->fns);
    }
    p->tip = container;
}

/* ------------------------------------------------------------------ driving it, and the task-list pass */

static void mark_tasks(MdNode *n)
{
    MdNode *c;

    if (n->type == MD_ITEM && n->first_child != NULL && n->first_child->type == MD_PARAGRAPH) {
        MdNode *para = n->first_child;

        if (para->content != NULL && para->content_len >= 3 && para->content[0] == '[' &&
            (para->content[1] == ' ' || para->content[1] == 'x' || para->content[1] == 'X') && para->content[2] == ']' &&
            (para->content_len == 3 || para->content[3] == ' ' || para->content[3] == '\n')) {
            size_t skip = 3;

            n->checked = para->content[1] == ' ' ? 0 : 1;
            while (skip < para->content_len && para->content[skip] == ' ') {
                skip++;
            }
            memmove(para->content, para->content + skip, para->content_len - skip + 1);
            para->content_len -= skip;
        }
    }
    for (c = n->first_child; c != NULL; c = c->next) {
        mark_tasks(c);
    }
}

/* One line at a time out of src: LF, CRLF and CR all end a line; tabs become spaces to the next multiple of 4; a NUL becomes U+FFFD. */
void md_parse_blocks(MdDoc *d, const char *src, size_t len)
{
    P p;
    MdBuf line;
    size_t i = 0;

    memset(&p, 0, sizeof(p));
    p.doc = d;
    p.root = d->root;
    p.tip = d->root;
    d->root->open = 1;
    md_buf_init(&line);
    while (i < len) {
        size_t col = 0;

        line.len = 0;
        while (i < len && src[i] != '\n' && src[i] != '\r') {
            if (src[i] == '\t') {
                do {
                    md_buf_putc(&line, ' ');
                    col++;
                } while (col % 4 != 0);
            } else if (src[i] == '\0') {
                md_buf_puts(&line, "\xEF\xBF\xBD");
                col++;
            } else {
                md_buf_putc(&line, src[i]);
                if (((unsigned char)src[i] & 0xC0) != 0x80) {
                    col++;
                }
            }
            i++;
        }
        if (i < len && src[i] == '\r' && i + 1 < len && src[i + 1] == '\n') {
            i++;
        }
        i++;                                                    /* past the line ending */
        process_line(&p, line.p ? line.p : "", line.len);
        if (line.fail) {
            d->oom = 1;
            break;
        }
    }
    md_buf_free(&line);
    while (p.tip != NULL && p.tip != p.root) {
        p.tip = finalize(&p, p.tip);
    }
    finalize(&p, p.root);
    mark_tasks(p.root);
}
