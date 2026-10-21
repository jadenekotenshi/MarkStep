/* md_inline.c -- the inline structure of a paragraph or heading: emphasis, code spans, links and images (inline and by
 * reference), autolinks, raw HTML, line breaks, escapes and character references; plus link reference definitions.
 * Follows CommonMark's delimiter-stack algorithm. */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"

#define NOPOS ((size_t)-1)

/* ------------------------------------------------------------------ small scanners */

static int is_ws(int c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

static int is_alpha(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static int is_digit(int c)
{
    return c >= '0' && c <= '9';
}

static int is_xdigit(int c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* Backslash escapes and (optionally) character references, resolved into out. */
void md_unescape_into(MdBuf *out, const char *s, size_t n, int entities)
{
    size_t i = 0;

    while (i < n) {
        if (s[i] == '\\' && i + 1 < n && md_is_ascii_punct((unsigned char)s[i + 1])) {
            md_buf_putc(out, s[i + 1]);
            i += 2;
        } else if (entities && s[i] == '&') {
            size_t j = i + 1;
            unsigned long cp = 0;
            int ok = 0;

            if (j < n && s[j] == '#') {
                int hex = 0, digits = 0;

                j++;
                if (j < n && (s[j] == 'x' || s[j] == 'X')) {
                    hex = 1;
                    j++;
                }
                while (j < n && (hex ? is_xdigit((unsigned char)s[j]) : is_digit((unsigned char)s[j])) && digits < 8) {
                    int dv = is_digit((unsigned char)s[j]) ? s[j] - '0' : ((s[j] | 32) - 'a' + 10);

                    cp = cp * (hex ? 16UL : 10UL) + (unsigned long)dv;
                    digits++;
                    j++;
                }
                if (digits >= 1 && digits <= (hex ? 6 : 7) && j < n && s[j] == ';') {
                    ok = 1;
                    if (cp == 0 || cp > 0x10FFFFUL) {
                        cp = 0xFFFDUL;
                    }
                    j++;
                }
            } else {
                size_t k = j;

                while (k < n && (is_alpha((unsigned char)s[k]) || is_digit((unsigned char)s[k]))) {
                    k++;
                }
                if (k > j && k < n && s[k] == ';') {
                    cp = md_entity_lookup(s + j, k - j);
                    if (cp != 0) {
                        ok = 1;
                        j = k + 1;
                    }
                }
            }
            if (ok) {
                char u[4];
                int ul = md_utf8_encode(cp, u);

                md_buf_put(out, u, (size_t)ul);
                i = j;
            } else {
                md_buf_putc(out, s[i]);
                i++;
            }
        } else {
            md_buf_putc(out, s[i]);
            i++;
        }
    }
}

/* A link destination at s[i]: <...> or a run without spaces and with balanced parentheses. Returns the position after it
 * (NOPOS if there is none); dest and dlen give the text without the angle brackets, unprocessed. */
size_t md_scan_link_dest(const char *s, size_t n, size_t i, const char **dest, size_t *dlen, int *angle)
{
    size_t j;

    *angle = 0;
    if (i < n && s[i] == '<') {
        j = i + 1;
        while (j < n && s[j] != '>') {
            if (s[j] == '\n' || s[j] == '<') {
                return NOPOS;
            }
            if (s[j] == '\\' && j + 1 < n && md_is_ascii_punct((unsigned char)s[j + 1])) {
                j++;
            }
            j++;
        }
        if (j >= n) {
            return NOPOS;
        }
        *angle = 1;
        *dest = s + i + 1;
        *dlen = j - i - 1;
        return j + 1;
    }
    {
        int depth = 0;

        j = i;
        while (j < n) {
            unsigned char c = (unsigned char)s[j];

            if (c == '\\' && j + 1 < n && md_is_ascii_punct((unsigned char)s[j + 1])) {
                j += 2;
                continue;
            }
            if (c == '(') {
                depth++;
                if (depth > 32) {
                    return NOPOS;
                }
            } else if (c == ')') {
                if (depth == 0) {
                    break;
                }
                depth--;
            } else if (c <= 0x20 || c == 0x7F) {
                break;
            }
            j++;
        }
        if (depth != 0) {
            return NOPOS;
        }
        *dest = s + i;
        *dlen = j - i;
        return j;
    }
}

/* A link title at s[i] ("..." or '...' or (...)): the position after it, or NOPOS. */
size_t md_scan_link_title(const char *s, size_t n, size_t i, const char **title, size_t *tlen)
{
    int open, close;
    size_t j;

    if (i >= n) {
        return NOPOS;
    }
    open = s[i];
    if (open == '"' || open == '\'') {
        close = open;
    } else if (open == '(') {
        close = ')';
    } else {
        return NOPOS;
    }
    for (j = i + 1; j < n; j++) {
        if (s[j] == '\\' && j + 1 < n && md_is_ascii_punct((unsigned char)s[j + 1])) {
            j++;
        } else if (s[j] == close) {
            *title = s + i + 1;
            *tlen = j - i - 1;
            return j + 1;
        } else if (open == '(' && s[j] == '(') {
            return NOPOS;
        } else if (s[j] == '\n' && j + 1 < n) {
            size_t k = j + 1;

            while (k < n && (s[k] == ' ')) {
                k++;
            }
            if (k < n && s[k] == '\n') {
                return NOPOS;                                  /* a title cannot hold a blank line */
            }
        }
    }
    return NOPOS;
}

/* ------------------------------------------------------------------ raw HTML */

static size_t scan_tag_name(const char *s, size_t n, size_t i)
{
    if (i >= n || !is_alpha((unsigned char)s[i])) {
        return NOPOS;
    }
    while (i < n && (is_alpha((unsigned char)s[i]) || is_digit((unsigned char)s[i]) || s[i] == '-')) {
        i++;
    }
    return i;
}

/* An open or closing tag starting at s[i] == '<': the position after its '>', or 0. */
size_t md_scan_html_tag(const char *s, size_t n, size_t i)
{
    size_t j;

    if (i >= n || s[i] != '<') {
        return 0;
    }
    if (i + 1 < n && s[i + 1] == '/') {
        j = scan_tag_name(s, n, i + 2);
        if (j == NOPOS) {
            return 0;
        }
        while (j < n && is_ws((unsigned char)s[j])) {
            j++;
        }
        return (j < n && s[j] == '>') ? j + 1 : 0;
    }
    j = scan_tag_name(s, n, i + 1);
    if (j == NOPOS) {
        return 0;
    }
    for (;;) {
        size_t k = j, ws = 0;

        while (k < n && is_ws((unsigned char)s[k])) {
            k++;
            ws++;
        }
        if (k < n && s[k] == '>') {
            return k + 1;
        }
        if (k + 1 < n && s[k] == '/' && s[k + 1] == '>') {
            return k + 2;
        }
        if (ws == 0 || k >= n || !(is_alpha((unsigned char)s[k]) || s[k] == '_' || s[k] == ':')) {
            return 0;
        }
        while (k < n && (is_alpha((unsigned char)s[k]) || is_digit((unsigned char)s[k]) || s[k] == '_' || s[k] == '.' ||
                         s[k] == ':' || s[k] == '-')) {
            k++;
        }
        {
            size_t m = k;

            while (m < n && is_ws((unsigned char)s[m])) {
                m++;
            }
            if (m < n && s[m] == '=') {
                m++;
                while (m < n && is_ws((unsigned char)s[m])) {
                    m++;
                }
                if (m >= n) {
                    return 0;
                }
                if (s[m] == '"' || s[m] == '\'') {
                    int q = s[m++];

                    while (m < n && s[m] != q) {
                        m++;
                    }
                    if (m >= n) {
                        return 0;
                    }
                    m++;
                } else {
                    size_t st = m;

                    while (m < n && !is_ws((unsigned char)s[m]) && s[m] != '"' && s[m] != '\'' && s[m] != '=' && s[m] != '<' &&
                           s[m] != '>' && s[m] != '`') {
                        m++;
                    }
                    if (m == st) {
                        return 0;
                    }
                }
                k = m;
            }
        }
        j = k;
    }
}

/* Any inline raw HTML at s[i]: a tag, a comment, a processing instruction, a declaration or CDATA. Returns its end or 0. */
static size_t scan_inline_html(const char *s, size_t n, size_t i)
{
    size_t j;

    if (i + 1 >= n || s[i] != '<') {
        return 0;
    }
    if (s[i + 1] == '!') {
        if (i + 3 < n && s[i + 2] == '-' && s[i + 3] == '-') {
            if (i + 4 < n && s[i + 4] == '>') {
                return i + 5;
            }
            if (i + 5 < n && s[i + 4] == '-' && s[i + 5] == '>') {
                return i + 6;
            }
            for (j = i + 4; j + 2 < n; j++) {
                if (s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>') {
                    return j + 3;
                }
            }
            return 0;
        }
        if (i + 8 < n && memcmp(s + i, "<![CDATA[", 9) == 0) {
            for (j = i + 9; j + 2 < n; j++) {
                if (s[j] == ']' && s[j + 1] == ']' && s[j + 2] == '>') {
                    return j + 3;
                }
            }
            return 0;
        }
        if (i + 2 < n && is_alpha((unsigned char)s[i + 2])) {
            for (j = i + 3; j < n; j++) {
                if (s[j] == '>') {
                    return j + 1;
                }
            }
        }
        return 0;
    }
    if (s[i + 1] == '?') {
        for (j = i + 2; j + 1 < n; j++) {
            if (s[j] == '?' && s[j + 1] == '>') {
                return j + 2;
            }
        }
        return 0;
    }
    return md_scan_html_tag(s, n, i);
}

/* ------------------------------------------------------------------ link reference definitions */

void md_add_ref(MdDoc *d, const char *label, size_t llen, const char *url, size_t ulen, const char *title, size_t tlen)
{
    MdBuf norm;
    int i;

    md_buf_init(&norm);
    md_normalize_label(label, llen, &norm);
    if (norm.fail || norm.len == 0) {
        md_buf_free(&norm);
        return;
    }
    for (i = 0; i < d->nrefs; i++) {
        if (strcmp(d->refs[i].label, norm.p) == 0) {
            md_buf_free(&norm);
            return;                                            /* the first definition of a label wins */
        }
    }
    if (d->nrefs >= d->refcap) {
        int cap = d->refcap ? d->refcap * 2 : 16;
        MdRef *nr = (MdRef *)realloc(d->refs, (size_t)cap * sizeof(MdRef));

        if (nr == NULL) {
            md_buf_free(&norm);
            return;
        }
        d->refs = nr;
        d->refcap = cap;
    }
    d->refs[d->nrefs].label = md_buf_finish(&norm, NULL);
    d->refs[d->nrefs].url = md_strndup(d, url, ulen);
    d->refs[d->nrefs].title = md_strndup(d, title, tlen);
    d->nrefs++;
}

const MdRef *md_find_ref(const MdDoc *d, const char *label, size_t llen)
{
    MdBuf norm;
    int i;
    const MdRef *found = NULL;

    md_buf_init(&norm);
    md_normalize_label(label, llen, &norm);
    if (!norm.fail && norm.len > 0) {
        for (i = 0; i < d->nrefs; i++) {
            if (strcmp(d->refs[i].label, norm.p) == 0) {
                found = &d->refs[i];
                break;
            }
        }
    }
    md_buf_free(&norm);
    return found;
}

/* A link label [text] at s[i]: its end (after the ']') or NOPOS; l and ll give the text. No unescaped brackets, at most 999 characters. */
static size_t scan_label(const char *s, size_t n, size_t i, const char **l, size_t *ll)
{
    size_t j;

    if (i >= n || s[i] != '[') {
        return NOPOS;
    }
    for (j = i + 1; j < n; j++) {
        if (s[j] == '\\' && j + 1 < n && (s[j + 1] == '[' || s[j + 1] == ']' || s[j + 1] == '\\')) {
            j++;
        } else if (s[j] == '[') {
            return NOPOS;
        } else if (s[j] == ']') {
            *l = s + i + 1;
            *ll = j - i - 1;
            return (j - i - 1 > 999) ? NOPOS : j + 1;
        }
    }
    return NOPOS;
}

static size_t skip_spaces_one_newline(const char *s, size_t n, size_t i)
{
    int nl = 0;

    while (i < n && (s[i] == ' ' || s[i] == '\t' || (s[i] == '\n' && !nl))) {
        if (s[i] == '\n') {
            nl = 1;
        }
        i++;
    }
    return i;
}

size_t md_scan_ref_defs(MdDoc *d, const char *s, size_t len)
{
    size_t pos = 0;

    for (;;) {
        const char *lab, *dest, *title = NULL;
        size_t ll, dl, tl = 0, i, after_label, after_dest, line_end, k;
        int angle;
        MdBuf du, tu;
        int has_title = 0;

        i = pos;
        while (i < len && s[i] == ' ') {
            i++;
        }
        after_label = scan_label(s, len, i, &lab, &ll);
        if (after_label == NOPOS || after_label >= len || s[after_label] != ':') {
            break;
        }
        {
            size_t z;
            int nonblank = 0;

            for (z = 0; z < ll; z++) {
                if (!is_ws((unsigned char)lab[z])) {
                    nonblank = 1;
                }
            }
            if (!nonblank) {
                break;
            }
        }
        i = skip_spaces_one_newline(s, len, after_label + 1);
        after_dest = md_scan_link_dest(s, len, i, &dest, &dl, &angle);
        if (after_dest == NOPOS || (!angle && dl == 0)) {
            break;
        }
        /* a title, if it sits alone at the end of its line */
        k = skip_spaces_one_newline(s, len, after_dest);
        if (k > after_dest) {
            size_t te = md_scan_link_title(s, len, k, &title, &tl);

            if (te != NOPOS) {
                size_t m = te;

                while (m < len && s[m] == ' ') {
                    m++;
                }
                if (m >= len || s[m] == '\n') {
                    has_title = 1;
                    line_end = m < len ? m + 1 : m;
                    goto defined;
                }
            }
        }
        {
            size_t m = after_dest;

            while (m < len && s[m] == ' ') {
                m++;
            }
            if (m < len && s[m] != '\n') {
                break;
            }
            line_end = m < len ? m + 1 : m;
        }
    defined:
        md_buf_init(&du);
        md_buf_init(&tu);
        md_unescape_into(&du, dest, dl, 1);
        if (has_title) {
            md_unescape_into(&tu, title, tl, 1);
        }
        md_add_ref(d, lab, ll, du.p ? du.p : "", du.len, tu.p ? tu.p : "", tu.len);
        md_buf_free(&du);
        md_buf_free(&tu);
        pos = line_end;
    }
    return pos;
}

/* ------------------------------------------------------------------ the inline parser */

typedef struct Delim Delim;
struct Delim {
    Delim *prev, *next;
    MdNode *node;
    int c, len, orig_len, can_open, can_close;
};

typedef struct Bracket Bracket;
struct Bracket {
    Bracket *prev;
    MdNode *node;
    Delim *prev_delim;
    size_t pos;                  /* just after the '[' */
    int image, active;
};

typedef struct {
    MdDoc *doc;
    const char *s;
    size_t n;
    size_t pos;
    MdNode *parent;
    Delim *last_delim;
    Bracket *last_bracket;
} S;

static MdNode *new_text(S *sp, const char *t, size_t n)
{
    MdNode *c = md_new_node(sp->doc, MD_TEXT);

    if (c == NULL) {
        return NULL;
    }
    c->text = md_strndup(sp->doc, t, n);
    c->text_len = n;
    return c;
}

static void add_text(S *sp, const char *t, size_t n)
{
    MdNode *c;

    if (n == 0) {
        return;
    }
    c = new_text(sp, t, n);
    if (c != NULL) {
        md_append_child(sp->parent, c);
    }
}

static void remove_delim(S *sp, Delim *d)
{
    if (d->next) {
        d->next->prev = d->prev;
    }
    if (d->prev) {
        d->prev->next = d->next;
    }
    if (sp->last_delim == d) {
        sp->last_delim = d->prev;
    }
}

static unsigned long char_before(const char *s, size_t i)
{
    size_t j;
    size_t adv;

    if (i == 0) {
        return ' ';
    }
    j = i - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) {
        j--;
    }
    return md_utf8_decode(s + j, i - j, &adv);
}

static unsigned long char_after(const char *s, size_t n, size_t i)
{
    size_t adv;

    if (i >= n) {
        return ' ';
    }
    return md_utf8_decode(s + i, n - i, &adv);
}

static void handle_delim(S *sp, int c)
{
    size_t start = sp->pos, e = sp->pos;
    int len, left, right, pws, pp, nws, np, can_open, can_close;
    unsigned long before, after;
    MdNode *tn;
    Delim *d;

    while (e < sp->n && sp->s[e] == c) {
        e++;
    }
    len = (int)(e - start);
    sp->pos = e;
    tn = new_text(sp, sp->s + start, (size_t)len);
    if (tn == NULL) {
        return;
    }
    md_append_child(sp->parent, tn);
    if (c == '~' && len != 2) {
        return;                                                /* only a pair of tildes means strikethrough */
    }
    before = char_before(sp->s, start);
    after = char_after(sp->s, sp->n, e);
    pws = md_is_unicode_space(before);
    pp = md_is_unicode_punct(before);
    nws = md_is_unicode_space(after);
    np = md_is_unicode_punct(after);
    left = !nws && (!np || pws || pp);
    right = !pws && (!pp || nws || np);
    if (c == '_') {
        can_open = left && (!right || pp);
        can_close = right && (!left || np);
    } else {
        can_open = left;
        can_close = right;
    }
    if (!can_open && !can_close) {
        return;
    }
    d = (Delim *)md_alloc(sp->doc, sizeof(Delim));
    if (d == NULL) {
        return;
    }
    d->node = tn;
    d->c = c;
    d->len = d->orig_len = len;
    d->can_open = can_open;
    d->can_close = can_close;
    d->prev = sp->last_delim;
    d->next = NULL;
    if (sp->last_delim) {
        sp->last_delim->next = d;
    }
    sp->last_delim = d;
}

/* Turns the delimiters above `bottom` into emphasis, strong emphasis and strikethrough where they pair up. */
static void process_emphasis(S *sp, Delim *bottom)
{
    Delim *closer, *opener, *openers_bottom[3][3][2];
    int ci, mi, k;

    for (ci = 0; ci < 3; ci++) {
        for (mi = 0; mi < 3; mi++) {
            for (k = 0; k < 2; k++) {
                openers_bottom[ci][mi][k] = bottom;
            }
        }
    }
    closer = sp->last_delim;
    while (closer != NULL && closer->prev != bottom) {
        closer = closer->prev;
    }
    while (closer != NULL) {
        if (closer->can_close) {
            int found = 0, idx_c = closer->c == '*' ? 0 : closer->c == '_' ? 1 : 2;
            Delim *old_closer = closer;
            Delim **ob;

            ob = &openers_bottom[idx_c][closer->orig_len % 3][closer->can_open ? 1 : 0];
            opener = closer->prev;
            while (opener != NULL && opener != bottom && opener != *ob) {
                int odd_match = (closer->can_open || opener->can_close) && closer->orig_len % 3 != 0 &&
                                (opener->orig_len + closer->orig_len) % 3 == 0;

                if (opener->can_open && opener->c == closer->c && !odd_match) {
                    if (closer->c != '~' || (opener->len == 2 && closer->len == 2)) {
                        found = 1;
                        break;
                    }
                }
                opener = opener->prev;
            }
            if (found) {
                MdNode *emph, *tmp, *next;
                int use = (closer->len >= 2 && opener->len >= 2) ? 2 : 1;
                Delim *d, *dn;

                if (closer->c == '~') {
                    use = 2;
                }
                emph = md_new_node(sp->doc, closer->c == '~' ? MD_STRIKE : (use == 2 ? MD_STRONG : MD_EMPH));
                if (emph == NULL) {
                    return;
                }
                opener->len -= use;
                closer->len -= use;
                opener->node->text_len -= (size_t)use;
                closer->node->text += use;
                closer->node->text_len -= (size_t)use;
                tmp = opener->node->next;
                while (tmp != NULL && tmp != closer->node) {
                    next = tmp->next;
                    md_unlink(tmp);
                    md_append_child(emph, tmp);
                    tmp = next;
                }
                md_insert_after(opener->node, emph);
                for (d = closer->prev; d != NULL && d != opener; d = dn) {      /* delimiters between them are spent */
                    dn = d->prev;
                    remove_delim(sp, d);
                }
                if (opener->len == 0) {
                    md_unlink(opener->node);
                    remove_delim(sp, opener);
                }
                if (closer->len == 0) {
                    Delim *nextc = closer->next;

                    md_unlink(closer->node);
                    remove_delim(sp, closer);
                    closer = nextc;
                }
            } else {
                *ob = old_closer->prev;
                closer = old_closer->next;
                if (!old_closer->can_open) {
                    remove_delim(sp, old_closer);
                }
            }
        } else {
            closer = closer->next;
        }
    }
    while (sp->last_delim != NULL && sp->last_delim != bottom) {                 /* what is left stays text */
        remove_delim(sp, sp->last_delim);
    }
}

/* backticks */
static void handle_backticks(S *sp)
{
    size_t start = sp->pos, e = sp->pos, i;
    size_t run;

    while (e < sp->n && sp->s[e] == '`') {
        e++;
    }
    run = e - start;
    i = e;
    while (i < sp->n) {
        if (sp->s[i] == '`') {
            size_t j = i;

            while (j < sp->n && sp->s[j] == '`') {
                j++;
            }
            if (j - i == run) {
                MdBuf b;
                MdNode *c;
                size_t k;
                int all_space = 1;

                md_buf_init(&b);
                for (k = e; k < i; k++) {
                    char ch = sp->s[k] == '\n' ? ' ' : sp->s[k];

                    if (ch != ' ') {
                        all_space = 0;
                    }
                    md_buf_putc(&b, ch);
                }
                if (b.len >= 2 && b.p[0] == ' ' && b.p[b.len - 1] == ' ' && !all_space) {
                    c = md_new_node(sp->doc, MD_CODE);
                    if (c != NULL) {
                        c->text = md_strndup(sp->doc, b.p + 1, b.len - 2);
                        c->text_len = b.len - 2;
                    }
                } else {
                    c = md_new_node(sp->doc, MD_CODE);
                    if (c != NULL) {
                        c->text = md_strndup(sp->doc, b.p ? b.p : "", b.len);
                        c->text_len = b.len;
                    }
                }
                md_buf_free(&b);
                if (c != NULL) {
                    md_append_child(sp->parent, c);
                }
                sp->pos = j;
                return;
            }
            i = j;
        } else {
            i++;
        }
    }
    add_text(sp, sp->s + start, run);                          /* no closing run: the backticks are just text */
    sp->pos = e;
}

static void handle_entity(S *sp)
{
    size_t i = sp->pos + 1, n = sp->n;
    const char *s = sp->s;
    size_t j;
    MdBuf b;

    /* find where a reference could end */
    j = i;
    if (j < n && s[j] == '#') {
        j++;
        if (j < n && (s[j] == 'x' || s[j] == 'X')) {
            j++;
        }
    }
    while (j < n && (is_alpha((unsigned char)s[j]) || is_digit((unsigned char)s[j])) && j - i < 40) {
        j++;
    }
    if (j < n && s[j] == ';') {
        md_buf_init(&b);
        md_unescape_into(&b, s + sp->pos, j + 1 - sp->pos, 1);
        if (!b.fail && !(b.len == j + 1 - sp->pos && memcmp(b.p, s + sp->pos, b.len) == 0)) {      /* it was a reference */
            add_text(sp, b.p, b.len);
            md_buf_free(&b);
            sp->pos = j + 1;
            return;
        }
        md_buf_free(&b);
    }
    add_text(sp, "&", 1);
    sp->pos++;
}

static int is_email_char(int c)
{
    return is_alpha(c) || is_digit(c) || strchr(".!#$%&'*+/=?^_`{|}~-", c) != NULL;
}

static int try_autolink(S *sp)
{
    const char *s = sp->s;
    size_t n = sp->n, i = sp->pos + 1, j;
    MdNode *link, *t;
    MdBuf url;

    /* URI: scheme ':' then no space, '<' or '>' */
    if (i < n && is_alpha((unsigned char)s[i])) {
        j = i + 1;
        while (j < n && (is_alpha((unsigned char)s[j]) || is_digit((unsigned char)s[j]) || s[j] == '+' || s[j] == '.' || s[j] == '-')) {
            j++;
        }
        if (j - i >= 2 && j - i <= 32 && j < n && s[j] == ':') {
            size_t k = j + 1;

            while (k < n && s[k] != '>' && s[k] != '<' && (unsigned char)s[k] > 0x20 && s[k] != 0x7F) {
                k++;
            }
            if (k < n && s[k] == '>') {
                link = md_new_node(sp->doc, MD_LINK);
                if (link == NULL) {
                    return 0;
                }
                link->url = md_strndup(sp->doc, s + i, k - i);
                t = new_text(sp, s + i, k - i);
                if (t) {
                    md_append_child(link, t);
                }
                md_append_child(sp->parent, link);
                sp->pos = k + 1;
                return 1;
            }
        }
    }
    /* email */
    j = i;
    while (j < n && is_email_char((unsigned char)s[j])) {
        j++;
    }
    if (j > i && j < n && s[j] == '@') {
        size_t k = j + 1, label_start = k;
        int ok = 0, labels = 0;

        while (k < n) {
            if (is_alpha((unsigned char)s[k]) || is_digit((unsigned char)s[k]) || s[k] == '-') {
                k++;
            } else if (s[k] == '.' && k > label_start) {
                labels++;
                k++;
                label_start = k;
            } else {
                break;
            }
        }
        if (k > label_start && k < n && s[k] == '>' && k - (j + 1) >= 1 && s[j + 1] != '-' && s[k - 1] != '-') {
            ok = 1;
        }
        (void)labels;
        if (ok) {
            md_buf_init(&url);
            md_buf_puts(&url, "mailto:");
            md_buf_put(&url, s + i, k - i);
            link = md_new_node(sp->doc, MD_LINK);
            if (link != NULL && !url.fail) {
                link->url = md_strndup(sp->doc, url.p, url.len);
                t = new_text(sp, s + i, k - i);
                if (t) {
                    md_append_child(link, t);
                }
                md_append_child(sp->parent, link);
            }
            md_buf_free(&url);
            sp->pos = k + 1;
            return 1;
        }
    }
    return 0;
}

static void push_bracket(S *sp, int image, size_t textlen)
{
    Bracket *b;
    MdNode *t;

    t = new_text(sp, image ? "![" : "[", textlen);
    if (t == NULL) {
        return;
    }
    md_append_child(sp->parent, t);
    b = (Bracket *)md_alloc(sp->doc, sizeof(Bracket));
    if (b == NULL) {
        return;
    }
    b->prev = sp->last_bracket;
    b->node = t;
    b->prev_delim = sp->last_delim;
    b->pos = sp->pos + textlen;
    b->image = image;
    b->active = 1;
    sp->last_bracket = b;
    sp->pos += textlen;
}

static int has_unescaped_bracket(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
        } else if (s[i] == '[' || s[i] == ']') {
            return 1;
        }
    }
    return 0;
}

static void handle_close_bracket(S *sp)
{
    Bracket *opener = sp->last_bracket;
    const char *s = sp->s;
    size_t n = sp->n, after = sp->pos + 1, i;
    int found = 0;
    const char *url = "", *title = "";
    size_t ulen = 0, tlen = 0;
    MdBuf ub, tb;
    const MdRef *ref = NULL;

    if (opener == NULL) {
        add_text(sp, "]", 1);
        sp->pos++;
        return;
    }
    if (!opener->active) {
        sp->last_bracket = opener->prev;
        add_text(sp, "]", 1);
        sp->pos++;
        return;
    }
    md_buf_init(&ub);
    md_buf_init(&tb);
    /* an inline link or image: (destination "title") */
    if (after < n && s[after] == '(') {
        const char *dest, *ttl;
        size_t dl, tl, e;
        int angle;

        i = after + 1;
        while (i < n && is_ws((unsigned char)s[i])) {
            i++;
        }
        e = md_scan_link_dest(s, n, i, &dest, &dl, &angle);
        if (e != NOPOS) {
            size_t k = e;

            while (k < n && is_ws((unsigned char)s[k])) {
                k++;
            }
            if (k < n && s[k] == ')') {
                found = 1;
                after = k + 1;
                md_unescape_into(&ub, dest, dl, 1);
            } else if (k > e) {
                size_t te = md_scan_link_title(s, n, k, &ttl, &tl);

                if (te != NOPOS) {
                    size_t m = te;

                    while (m < n && is_ws((unsigned char)s[m])) {
                        m++;
                    }
                    if (m < n && s[m] == ')') {
                        found = 1;
                        after = m + 1;
                        md_unescape_into(&ub, dest, dl, 1);
                        md_unescape_into(&tb, ttl, tl, 1);
                    }
                }
            }
        }
    }
    /* a reference: [text][label], [text][] or [text] */
    if (!found) {
        const char *lab = NULL;
        size_t ll = 0, e;
        const char *rawl = s + opener->pos;
        size_t rawn = sp->pos - opener->pos;

        e = (after < n && s[after] == '[') ? scan_label(s, n, after, &lab, &ll) : NOPOS;
        if (e != NOPOS && ll > 0) {
            ref = md_find_ref(sp->doc, lab, ll);
            if (ref != NULL) {
                after = e;
            }
        } else if (e != NOPOS && ll == 0) {
            if (!has_unescaped_bracket(rawl, rawn)) {
                ref = md_find_ref(sp->doc, rawl, rawn);
                if (ref != NULL) {
                    after = e;
                }
            }
        } else if (!has_unescaped_bracket(rawl, rawn) && !(after < n && s[after] == '[' && e == NOPOS && 0)) {
            ref = md_find_ref(sp->doc, rawl, rawn);
        }
        if (ref != NULL) {
            found = 1;
            url = ref->url;
            ulen = strlen(ref->url);
            title = ref->title;
            tlen = strlen(ref->title);
        }
    }
    if (found) {
        MdNode *link = md_new_node(sp->doc, opener->image ? MD_IMAGE : MD_LINK);
        MdNode *tmp, *next;
        Bracket *bk;

        if (link == NULL) {
            md_buf_free(&ub);
            md_buf_free(&tb);
            return;
        }
        if (ref != NULL) {
            link->url = md_strndup(sp->doc, url, ulen);
            link->title = md_strndup(sp->doc, title, tlen);
        } else {
            link->url = md_strndup(sp->doc, ub.p ? ub.p : "", ub.len);
            link->title = md_strndup(sp->doc, tb.p ? tb.p : "", tb.len);
        }
        tmp = opener->node->next;
        while (tmp != NULL) {
            next = tmp->next;
            md_unlink(tmp);
            md_append_child(link, tmp);
            tmp = next;
        }
        md_append_child(sp->parent, link);
        md_unlink(opener->node);
        process_emphasis(sp, opener->prev_delim);
        sp->last_bracket = opener->prev;
        if (!opener->image) {
            for (bk = sp->last_bracket; bk != NULL; bk = bk->prev) {
                if (!bk->image) {
                    bk->active = 0;                              /* links may not hold links */
                }
            }
        }
        sp->pos = after;
    } else {
        sp->last_bracket = opener->prev;
        add_text(sp, "]", 1);
        sp->pos++;
    }
    md_buf_free(&ub);
    md_buf_free(&tb);
}

static void handle_newline(S *sp)
{
    MdNode *last = sp->parent->last_child;
    int hard = 0;

    if (last != NULL && last->type == MD_TEXT) {
        size_t k = last->text_len, spaces = 0;

        while (k > 0 && last->text[k - 1] == ' ') {
            k--;
            spaces++;
        }
        last->text_len = k;
        hard = spaces >= 2;
        if (k == 0) {
            md_unlink(last);
        }
    }
    {
        MdNode *br = md_new_node(sp->doc, hard ? MD_LINEBREAK : MD_SOFTBREAK);

        if (br != NULL) {
            md_append_child(sp->parent, br);
        }
    }
    sp->pos++;
    while (sp->pos < sp->n && sp->s[sp->pos] == ' ') {
        sp->pos++;
    }
}

static int is_special(int c)
{
    return c == '\n' || c == '`' || c == '\\' || c == '&' || c == '<' || c == '*' || c == '_' || c == '~' || c == '[' || c == ']' ||
           c == '!';
}

static void parse_inline_text(MdDoc *d, MdNode *parent, const char *s, size_t n)
{
    S sp;

    memset(&sp, 0, sizeof(sp));
    sp.doc = d;
    sp.s = s;
    sp.n = n;
    sp.parent = parent;
    while (sp.pos < sp.n) {
        int c = (unsigned char)s[sp.pos];

        switch (c) {
        case '\n':
            handle_newline(&sp);
            break;
        case '`':
            handle_backticks(&sp);
            break;
        case '\\':
            if (sp.pos + 1 < sp.n && s[sp.pos + 1] == '\n') {
                MdNode *br = md_new_node(d, MD_LINEBREAK);

                if (br != NULL) {
                    md_append_child(parent, br);
                }
                sp.pos += 2;
                while (sp.pos < sp.n && s[sp.pos] == ' ') {
                    sp.pos++;
                }
            } else if (sp.pos + 1 < sp.n && md_is_ascii_punct((unsigned char)s[sp.pos + 1])) {
                add_text(&sp, s + sp.pos + 1, 1);
                sp.pos += 2;
            } else {
                add_text(&sp, "\\", 1);
                sp.pos++;
            }
            break;
        case '&':
            handle_entity(&sp);
            break;
        case '<':
            if (!try_autolink(&sp)) {
                size_t e = scan_inline_html(s, n, sp.pos);

                if (e > 0) {
                    MdNode *h = md_new_node(d, MD_HTML_INLINE);

                    if (h != NULL) {
                        h->text = md_strndup(d, s + sp.pos, e - sp.pos);
                        h->text_len = e - sp.pos;
                        md_append_child(parent, h);
                    }
                    sp.pos = e;
                } else {
                    add_text(&sp, "<", 1);
                    sp.pos++;
                }
            }
            break;
        case '*':
        case '_':
        case '~':
            handle_delim(&sp, c);
            break;
        case '[':
            push_bracket(&sp, 0, 1);
            break;
        case '!':
            if (sp.pos + 1 < sp.n && s[sp.pos + 1] == '[') {
                push_bracket(&sp, 1, 2);
            } else {
                add_text(&sp, "!", 1);
                sp.pos++;
            }
            break;
        case ']':
            handle_close_bracket(&sp);
            break;
        default: {
            size_t e = sp.pos + 1;

            while (e < sp.n && !is_special((unsigned char)s[e])) {
                e++;
            }
            add_text(&sp, s + sp.pos, e - sp.pos);
            sp.pos = e;
            break;
        }
        }
    }
    process_emphasis(&sp, NULL);
    /* neighbouring pieces of text become one */
    {
        MdNode *c = parent->first_child;

        while (c != NULL) {
            MdNode *next = c->next;

            if (c->type == MD_TEXT && next != NULL && next->type == MD_TEXT) {
                MdBuf b;

                md_buf_init(&b);
                md_buf_put(&b, c->text, c->text_len);
                while (next != NULL && next->type == MD_TEXT) {
                    MdNode *after = next->next;

                    md_buf_put(&b, next->text, next->text_len);
                    md_unlink(next);
                    next = after;
                }
                c->text = md_strndup(d, b.p ? b.p : "", b.len);
                c->text_len = b.len;
                md_buf_free(&b);
                c = next;
            } else {
                c = next;
            }
        }
    }
}

static void parse_node(MdDoc *d, MdNode *n)
{
    MdNode *c;

    if ((n->type == MD_PARAGRAPH || n->type == MD_HEADING || n->type == MD_TABLE_CELL) && n->content != NULL) {
        size_t len = n->content_len;

        while (len > 0 && (n->content[len - 1] == '\n' || n->content[len - 1] == ' ')) {
            len--;
        }
        parse_inline_text(d, n, n->content, len);
        md_node_free_content(n);
        return;
    }
    for (c = n->first_child; c != NULL; c = c->next) {
        parse_node(d, c);
    }
}

void md_parse_inlines(MdDoc *d)
{
    parse_node(d, d->root);
}
