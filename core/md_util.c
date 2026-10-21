/* md_util.c -- arena, tree links, growable strings, UTF-8 and character references for the Markdown parser. */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"

/* ------------------------------------------------------------------ arena */

#define CHUNK_SIZE 16384UL

struct MdChunk {
    MdChunk *next;
    size_t used, cap;
    double align_;                       /* keeps data[] 8-aligned */
};

#define CHUNK_DATA(c) ((char *)(c) + sizeof(MdChunk))

void *md_alloc(MdDoc *d, size_t n)
{
    MdChunk *c = d->chunks;
    void *p;

    n = (n + 7) & ~(size_t)7;
    if (c == NULL || c->cap - c->used < n) {
        size_t cap = n > CHUNK_SIZE ? n : CHUNK_SIZE;

        c = (MdChunk *)malloc(sizeof(MdChunk) + cap);
        if (c == NULL) {
            d->oom = 1;
            return NULL;
        }
        c->next = d->chunks;
        c->used = 0;
        c->cap = cap;
        d->chunks = c;
    }
    p = CHUNK_DATA(c) + c->used;
    c->used += n;
    return p;
}

char *md_strndup(MdDoc *d, const char *s, size_t n)
{
    char *p = (char *)md_alloc(d, n + 1);

    if (p == NULL) {
        return NULL;
    }
    if (n > 0) {
        memcpy(p, s, n);
    }
    p[n] = '\0';
    return p;
}

MdNode *md_new_node(MdDoc *d, MdType t)
{
    MdNode *n = (MdNode *)md_alloc(d, sizeof(MdNode));

    if (n == NULL) {
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->type = t;
    n->text = "";
    n->info = "";
    n->url = "";
    n->title = "";
    n->checked = -1;
    return n;
}

void md_node_free_content(MdNode *n)
{
    free(n->content);
    n->content = NULL;
    n->content_len = n->content_cap = 0;
}

void md_node_add_line(MdNode *n, const char *s, size_t len)
{
    if (n->content_len + len + 2 > n->content_cap) {
        size_t cap = n->content_cap ? n->content_cap : 128;
        char *nb;

        while (cap < n->content_len + len + 2) {
            cap *= 2;
        }
        nb = (char *)realloc(n->content, cap);
        if (nb == NULL) {
            return;
        }
        n->content = nb;
        n->content_cap = cap;
    }
    if (len > 0) {
        memcpy(n->content + n->content_len, s, len);
    }
    n->content_len += len;
    n->content[n->content_len++] = '\n';
    n->content[n->content_len] = '\0';
}

void md_unlink(MdNode *n)
{
    if (n->prev) {
        n->prev->next = n->next;
    } else if (n->parent) {
        n->parent->first_child = n->next;
    }
    if (n->next) {
        n->next->prev = n->prev;
    } else if (n->parent) {
        n->parent->last_child = n->prev;
    }
    n->parent = n->prev = n->next = NULL;
}

void md_append_child(MdNode *parent, MdNode *child)
{
    child->parent = parent;
    child->next = NULL;
    child->prev = parent->last_child;
    if (parent->last_child) {
        parent->last_child->next = child;
    } else {
        parent->first_child = child;
    }
    parent->last_child = child;
}

void md_prepend_child(MdNode *parent, MdNode *child)
{
    child->parent = parent;
    child->prev = NULL;
    child->next = parent->first_child;
    if (parent->first_child) {
        parent->first_child->prev = child;
    } else {
        parent->last_child = child;
    }
    parent->first_child = child;
}

void md_insert_after(MdNode *ref, MdNode *n)
{
    n->parent = ref->parent;
    n->prev = ref;
    n->next = ref->next;
    if (ref->next) {
        ref->next->prev = n;
    } else if (ref->parent) {
        ref->parent->last_child = n;
    }
    ref->next = n;
}

void md_insert_before(MdNode *ref, MdNode *n)
{
    n->parent = ref->parent;
    n->next = ref;
    n->prev = ref->prev;
    if (ref->prev) {
        ref->prev->next = n;
    } else if (ref->parent) {
        ref->parent->first_child = n;
    }
    ref->prev = n;
}

static void free_contents(MdNode *n)
{
    MdNode *c;

    for (c = n->first_child; c != NULL; c = c->next) {
        free_contents(c);
    }
    free(n->content);
    n->content = NULL;
}

MdNode *md_root(const MdDoc *doc)
{
    return doc != NULL ? doc->root : NULL;
}

void md_free(MdDoc *doc)
{
    MdChunk *c, *next;
    int i;

    if (doc == NULL) {
        return;
    }
    if (doc->root) {
        free_contents(doc->root);
    }
    for (i = 0; i < doc->nrefs; i++) {
        free(doc->refs[i].label);
    }
    free(doc->refs);
    for (c = doc->chunks; c != NULL; c = next) {
        next = c->next;
        free(c);
    }
    free(doc);
}

/* ------------------------------------------------------------------ growable string */

void md_buf_init(MdBuf *b)
{
    b->p = NULL;
    b->len = b->cap = 0;
    b->fail = 0;
}

void md_buf_put(MdBuf *b, const char *s, size_t n)
{
    if (b->fail || n == 0) {
        return;
    }
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        char *nb;

        while (cap < b->len + n + 1) {
            cap *= 2;
        }
        nb = (char *)realloc(b->p, cap);
        if (nb == NULL) {
            b->fail = 1;
            return;
        }
        b->p = nb;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = '\0';
}

void md_buf_putc(MdBuf *b, int c)
{
    char ch = (char)c;

    md_buf_put(b, &ch, 1);
}

void md_buf_puts(MdBuf *b, const char *s)
{
    md_buf_put(b, s, strlen(s));
}

void md_buf_putint(MdBuf *b, long v)
{
    char tmp[24];
    int i = 0, j;
    unsigned long u;

    if (v < 0) {
        md_buf_putc(b, '-');
        u = (unsigned long)(-v);
    } else {
        u = (unsigned long)v;
    }
    if (u == 0) {
        tmp[i++] = '0';
    }
    while (u > 0) {
        tmp[i++] = (char)('0' + u % 10);
        u /= 10;
    }
    for (j = i - 1; j >= 0; j--) {
        md_buf_putc(b, tmp[j]);
    }
}

char *md_buf_finish(MdBuf *b, size_t *len)
{
    char *p;

    if (b->fail) {
        free(b->p);
        md_buf_init(b);
        return NULL;
    }
    if (b->p == NULL) {
        p = (char *)malloc(1);
        if (p != NULL) {
            p[0] = '\0';
        }
        if (len) {
            *len = 0;
        }
        return p;
    }
    p = b->p;
    if (len) {
        *len = b->len;
    }
    md_buf_init(b);
    return p;
}

void md_buf_free(MdBuf *b)
{
    free(b->p);
    md_buf_init(b);
}

/* ------------------------------------------------------------------ characters */

int md_utf8_encode(unsigned long cp, char out[4])
{
    if (cp == 0 || cp > 0x10FFFFUL || (cp >= 0xD800UL && cp <= 0xDFFFUL)) {
        cp = 0xFFFDUL;
    }
    if (cp < 0x80UL) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800UL) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000UL) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

unsigned long md_utf8_decode(const char *s, size_t n, size_t *adv)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned long cp;
    size_t need, i;

    if (n == 0) {
        *adv = 0;
        return 0;
    }
    if (u[0] < 0x80) {
        *adv = 1;
        return u[0];
    }
    if (u[0] >= 0xC2 && u[0] <= 0xDF) {
        need = 1;
        cp = u[0] & 0x1F;
    } else if (u[0] >= 0xE0 && u[0] <= 0xEF) {
        need = 2;
        cp = u[0] & 0x0F;
    } else if (u[0] >= 0xF0 && u[0] <= 0xF4) {
        need = 3;
        cp = u[0] & 0x07;
    } else {
        *adv = 1;
        return 0xFFFDUL;
    }
    if (n < need + 1) {
        *adv = 1;
        return 0xFFFDUL;
    }
    for (i = 1; i <= need; i++) {
        if ((u[i] & 0xC0) != 0x80) {
            *adv = 1;
            return 0xFFFDUL;
        }
        cp = (cp << 6) | (u[i] & 0x3F);
    }
    if ((need == 2 && cp < 0x800UL) || (need == 3 && (cp < 0x10000UL || cp > 0x10FFFFUL)) ||
        (cp >= 0xD800UL && cp <= 0xDFFFUL)) {
        *adv = 1;
        return 0xFFFDUL;
    }
    *adv = need + 1;
    return cp;
}

int md_is_ascii_punct(int c)
{
    return (c >= 0x21 && c <= 0x2F) || (c >= 0x3A && c <= 0x40) || (c >= 0x5B && c <= 0x60) || (c >= 0x7B && c <= 0x7E);
}

int md_is_unicode_space(unsigned long cp)
{
    return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0x0B || cp == 0x0C || cp == 0xA0UL || cp == 0x1680UL ||
           (cp >= 0x2000UL && cp <= 0x200AUL) || cp == 0x202FUL || cp == 0x205FUL || cp == 0x3000UL;
}

int md_is_unicode_punct(unsigned long cp)
{
    if (cp < 0x80UL) {
        return md_is_ascii_punct((int)cp);
    }
    return (cp >= 0xA1UL && cp <= 0xA9UL) || cp == 0xABUL || cp == 0xACUL || (cp >= 0xAEUL && cp <= 0xB1UL) || cp == 0xB4UL ||
           (cp >= 0xB6UL && cp <= 0xB8UL) || cp == 0xBBUL || cp == 0xBFUL || cp == 0xD7UL || cp == 0xF7UL ||
           (cp >= 0x2010UL && cp <= 0x2027UL) || (cp >= 0x2030UL && cp <= 0x205EUL) || (cp >= 0x20A0UL && cp <= 0x20CFUL) ||
           (cp >= 0x2190UL && cp <= 0x23FFUL) || (cp >= 0x2500UL && cp <= 0x27BFUL) || (cp >= 0x3001UL && cp <= 0x3003UL) ||
           (cp >= 0x3008UL && cp <= 0x3011UL) || (cp >= 0x3014UL && cp <= 0x301FUL) || (cp >= 0xFF01UL && cp <= 0xFF0FUL) ||
           (cp >= 0xFF1AUL && cp <= 0xFF20UL) || (cp >= 0xFE50UL && cp <= 0xFE6BUL);
}

int md_strncasecmp(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        int x = (unsigned char)a[i], y = (unsigned char)b[i];

        if (x >= 'A' && x <= 'Z') {
            x += 32;
        }
        if (y >= 'A' && y <= 'Z') {
            y += 32;
        }
        if (x != y) {
            return x < y ? -1 : 1;
        }
        if (x == 0) {
            return 0;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ character references */

static const char *const LATIN1[96] = {
    "nbsp", "iexcl", "cent", "pound", "curren", "yen", "brvbar", "sect", "uml", "copy", "ordf", "laquo", "not", "shy", "reg", "macr",
    "deg", "plusmn", "sup2", "sup3", "acute", "micro", "para", "middot", "cedil", "sup1", "ordm", "raquo", "frac14", "frac12",
    "frac34", "iquest", "Agrave", "Aacute", "Acirc", "Atilde", "Auml", "Aring", "AElig", "Ccedil", "Egrave", "Eacute", "Ecirc",
    "Euml", "Igrave", "Iacute", "Icirc", "Iuml", "ETH", "Ntilde", "Ograve", "Oacute", "Ocirc", "Otilde", "Ouml", "times", "Oslash",
    "Ugrave", "Uacute", "Ucirc", "Uuml", "Yacute", "THORN", "szlig", "agrave", "aacute", "acirc", "atilde", "auml", "aring",
    "aelig", "ccedil", "egrave", "eacute", "ecirc", "euml", "igrave", "iacute", "icirc", "iuml", "eth", "ntilde", "ograve",
    "oacute", "ocirc", "otilde", "ouml", "divide", "oslash", "ugrave", "uacute", "ucirc", "uuml", "yacute", "thorn", "yuml"
};

static const struct { const char *name; unsigned long cp; } OTHER[] = {
    { "quot", 34 }, { "amp", 38 }, { "apos", 39 }, { "lt", 60 }, { "gt", 62 }, { "OElig", 338 }, { "oelig", 339 },
    { "Scaron", 352 }, { "scaron", 353 }, { "Yuml", 376 }, { "fnof", 402 }, { "circ", 710 }, { "tilde", 732 },
    { "ensp", 8194 }, { "emsp", 8195 }, { "thinsp", 8201 }, { "zwnj", 8204 }, { "zwj", 8205 }, { "ndash", 8211 },
    { "mdash", 8212 }, { "lsquo", 8216 }, { "rsquo", 8217 }, { "sbquo", 8218 }, { "ldquo", 8220 }, { "rdquo", 8221 },
    { "bdquo", 8222 }, { "dagger", 8224 }, { "Dagger", 8225 }, { "bull", 8226 }, { "hellip", 8230 }, { "permil", 8240 },
    { "prime", 8242 }, { "Prime", 8243 }, { "lsaquo", 8249 }, { "rsaquo", 8250 }, { "euro", 8364 }, { "trade", 8482 },
    { "larr", 8592 }, { "uarr", 8593 }, { "rarr", 8594 }, { "darr", 8595 }, { "harr", 8596 }, { "rArr", 8658 },
    { "lArr", 8656 }, { "hArr", 8660 }, { "minus", 8722 }, { "ne", 8800 }, { "le", 8804 }, { "ge", 8805 }, { "infin", 8734 },
    { "asymp", 8776 }, { "radic", 8730 }, { "sum", 8721 }, { "hearts", 9829 }, { "spades", 9824 }, { "clubs", 9827 },
    { "diams", 9830 }, { "check", 10003 }, { "star", 9734 }, { "starf", 9733 }, { "Alpha", 913 }, { "Beta", 914 },
    { "alpha", 945 }, { "beta", 946 }, { "gamma", 947 }, { "delta", 948 }, { "lambda", 955 }, { "mu", 956 }, { "pi", 960 },
    { "sigma", 963 }, { "omega", 969 }, { "Omega", 937 }, { "Delta", 916 }, { "Sigma", 931 }, { "Pi", 928 }
};

unsigned long md_entity_lookup(const char *name, size_t len)
{
    size_t i;

    for (i = 0; i < 96; i++) {
        if (strlen(LATIN1[i]) == len && memcmp(LATIN1[i], name, len) == 0) {
            return 160UL + i;
        }
    }
    for (i = 0; i < sizeof(OTHER) / sizeof(OTHER[0]); i++) {
        if (strlen(OTHER[i].name) == len && memcmp(OTHER[i].name, name, len) == 0) {
            return OTHER[i].cp;
        }
    }
    return 0;
}

/* Case folds one code point into out (ASCII, Latin-1 and the sharp s), so that [Foo] and [FOO] and [ẞ]/[SS] name one label. */
static void fold_cp(unsigned long cp, MdBuf *out)
{
    char u[4];
    int n;

    if (cp >= 'A' && cp <= 'Z') {
        cp += 32;
    } else if ((cp >= 0xC0UL && cp <= 0xDEUL && cp != 0xD7UL)) {
        cp += 32;
    } else if (cp == 0xDFUL || cp == 0x1E9EUL) {
        md_buf_puts(out, "ss");
        return;
    } else if (cp >= 0x391UL && cp <= 0x3A9UL) {
        cp += 32;
    } else if (cp >= 0x410UL && cp <= 0x42FUL) {
        cp += 32;
    }
    n = md_utf8_encode(cp, u);
    md_buf_put(out, u, (size_t)n);
}

void md_normalize_label(const char *s, size_t n, MdBuf *out)
{
    size_t i = 0, adv;
    int pending_space = 0, any = 0;

    while (i < n) {
        unsigned long cp = md_utf8_decode(s + i, n - i, &adv);

        if (md_is_unicode_space(cp)) {
            if (any) {
                pending_space = 1;
            }
        } else {
            if (pending_space) {
                md_buf_putc(out, ' ');
                pending_space = 0;
            }
            fold_cp(cp, out);
            any = 1;
        }
        i += adv;
    }
}
