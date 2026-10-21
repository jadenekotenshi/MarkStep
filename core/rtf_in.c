/* rtf_in.c -- RTF to Markdown (see rtf_in.h).
 *
 * Two stages. The reader walks the RTF once, keeping a stack of group states, and collects paragraphs made of runs (text with
 * its bold/italic/underline/strike/monospace/link/size/colour) plus each paragraph's indents, style and list marker. The
 * writer then decides what each paragraph is -- heading, list item, code, quote, rule, table row, plain -- and prints Markdown. */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"
#include "nsenc.h"
#include "rtf_in.h"

#define MAXDEPTH   128
#define MAXFONTS   256
#define MAXSTYLES  256
#define MAXCOLORS  256

void rtf_md_defaults(RtfMdOptions *o)
{
    o->charset = RTF_CHARSET_AUTO;
    o->quotes = 1;
}

/* ------------------------------------------------------------------ the reader's data */

typedef struct {
    char name[48];
    int mono, symbol;
} Font;

typedef struct {
    int bold, italic, ul, strike, hidden, mono_hint;
    int font, fs, color, link;                 /* link: 1 + index into the link table, 0 for none */
} Attr;

typedef struct {
    int li, fi, align, style, intbl, ilvl, ls, sb, sa;
} PProps;

typedef struct {
    MdBuf text;
    Attr a;
} Run;

typedef struct {
    Run *runs;
    int n, cap;
    PProps pp;
    MdBuf marker;                              /* the text of a \listtext / \pntext group */
    int marker_font;
    int cell_end, row_end;
} Para;

enum { D_NORMAL = 0, D_SKIP, D_FONTTBL, D_COLORTBL, D_STYLESHEET, D_FLDINST, D_FLDRSLT, D_MARKER };

typedef struct {
    Attr a;
    PProps pp;
    int dest;
    int uc;
    int first;                                 /* nothing but this group's opening brace has been seen: a keyword may name a destination */
} GState;

typedef struct {
    Font fonts[MAXFONTS];
    char styles[MAXSTYLES][48];
    int gray[MAXCOLORS];
    int deff;
    char **links;
    int nlinks;
    Para *paras;
    int npara, parcap;
    int open;                                  /* the last paragraph is still being filled */
    GState st[MAXDEPTH];
    int depth;
    int charset;                               /* RTF_CHARSET_CP1252 or _NEXTSTEP once decided */
    int auto_charset, seen_rtf;
    /* destination scratch */
    int cur_font, cur_style, ncolors;
    int cr, cg, cb;
    MdBuf name;                                /* a font or style name being read */
    MdBuf fldinst;
    int field_url;                             /* 1 + link index of the field being read, 0 for none */
    int skip;                                  /* characters still to skip after \uN */
    unsigned long hi_surrogate;
    int oom;
    int strip_bold;                            /* writing a heading: its bold is the heading, not emphasis */
} RP;

static const unsigned short CP1252_HIGH[32] = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
    0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178
};

static unsigned long decode_byte(const RP *p, int b)
{
    if (b < 0x80) {
        return (unsigned long)b;
    }
    if (p->charset == RTF_CHARSET_NEXTSTEP) {
        return nsenc_decode((unsigned char)b);
    }
    return b < 0xA0 ? CP1252_HIGH[b - 0x80] : (unsigned long)b;
}

static int attr_eq(const Attr *a, const Attr *b)
{
    return a->bold == b->bold && a->italic == b->italic && a->ul == b->ul && a->strike == b->strike && a->hidden == b->hidden &&
           a->font == b->font && a->fs == b->fs && a->color == b->color && a->link == b->link;
}

static void reset_char(RP *p, Attr *a)
{
    a->bold = a->italic = a->ul = a->strike = a->hidden = 0;
    a->font = p->deff;
    a->fs = 24;
    a->color = 0;
}

static void reset_para(PProps *pp)
{
    pp->li = pp->fi = pp->align = pp->style = pp->intbl = pp->sb = pp->sa = 0;
    pp->ilvl = pp->ls = -1;
}

static int add_link(RP *p, const char *url)
{
    char **nl = (char **)realloc(p->links, (size_t)(p->nlinks + 1) * sizeof(char *));

    if (nl == NULL) {
        p->oom = 1;
        return 0;
    }
    p->links = nl;
    p->links[p->nlinks] = (char *)malloc(strlen(url) + 1);
    if (p->links[p->nlinks] == NULL) {
        p->oom = 1;
        return 0;
    }
    strcpy(p->links[p->nlinks], url);
    p->nlinks++;
    return p->nlinks;
}

static Para *cur_para(RP *p)
{
    if (!p->open) {
        Para *pa;

        if (p->npara >= p->parcap) {
            int cap = p->parcap ? p->parcap * 2 : 64;
            Para *np = (Para *)realloc(p->paras, (size_t)cap * sizeof(Para));

            if (np == NULL) {
                p->oom = 1;
                return NULL;
            }
            p->paras = np;
            p->parcap = cap;
        }
        pa = &p->paras[p->npara++];
        memset(pa, 0, sizeof(*pa));
        md_buf_init(&pa->marker);
        pa->pp = p->st[p->depth].pp;
        p->open = 1;
    }
    return &p->paras[p->npara - 1];
}

static void end_para(RP *p)
{
    cur_para(p);
    p->open = 0;
}

static MdBuf *cur_text(RP *p)
{
    Para *pa = cur_para(p);
    Attr *a = &p->st[p->depth].a;

    if (pa == NULL) {
        return NULL;
    }
    if (pa->n == 0 || !attr_eq(&pa->runs[pa->n - 1].a, a)) {
        Run *r;

        if (pa->n >= pa->cap) {
            int cap = pa->cap ? pa->cap * 2 : 8;
            Run *nr = (Run *)realloc(pa->runs, (size_t)cap * sizeof(Run));

            if (nr == NULL) {
                p->oom = 1;
                return NULL;
            }
            pa->runs = nr;
            pa->cap = cap;
        }
        r = &pa->runs[pa->n++];
        md_buf_init(&r->text);
        r->a = *a;
    }
    return &pa->runs[pa->n - 1].text;
}

static void finish_name(RP *p, int what)
{
    char buf[48];
    size_t n = p->name.len, i, s = 0;

    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    if (p->name.p != NULL) {
        memcpy(buf, p->name.p, n);
    }
    buf[n] = '\0';
    while (s < n && buf[s] == ' ') {
        s++;
    }
    for (i = n; i > s && buf[i - 1] == ' '; i--) {
        ;
    }
    buf[i] = '\0';
    if (what == D_FONTTBL && p->cur_font >= 0 && p->cur_font < MAXFONTS) {
        Font *f = &p->fonts[p->cur_font];
        static const char *const mono[] = { "courier", "mono", "consolas", "menlo", "fixed", "lucida console", "terminal",
                                            "typewriter", "andale", "letter gothic", "ocr", "code", "cousine", NULL };
        int k, j, ln = (int)strlen(buf + s);
        char low[48];

        strcpy(f->name, buf + s);
        for (j = 0; j < ln; j++) {
            low[j] = (f->name[j] >= 'A' && f->name[j] <= 'Z') ? (char)(f->name[j] + 32) : f->name[j];
        }
        low[ln] = '\0';
        for (k = 0; mono[k] != NULL; k++) {
            if (strstr(low, mono[k]) != NULL) {
                f->mono = 1;
            }
        }
        if (strstr(low, "symbol") != NULL || strstr(low, "wingdings") != NULL || strstr(low, "dingbats") != NULL) {
            f->symbol = 1;
        }
    } else if (what == D_STYLESHEET && p->cur_style >= 0 && p->cur_style < MAXSTYLES) {
        strcpy(p->styles[p->cur_style], buf + s);
    }
    md_buf_free(&p->name);
}

static void put_cp(RP *p, unsigned long cp)
{
    GState *g = &p->st[p->depth];
    char u[4];
    int ul;

    if (p->skip > 0) {
        p->skip--;
        return;
    }
    switch (g->dest) {
    case D_NORMAL:
    case D_FLDRSLT:
        if (!g->a.hidden) {
            MdBuf *b = cur_text(p);

            if (b != NULL) {
                ul = md_utf8_encode(cp, u);
                md_buf_put(b, u, (size_t)ul);
            }
        }
        break;
    case D_MARKER: {
        Para *pa = cur_para(p);

        if (pa != NULL) {
            ul = md_utf8_encode(cp, u);
            md_buf_put(&pa->marker, u, (size_t)ul);
            pa->marker_font = g->a.font;
        }
        break;
    }
    case D_FLDINST:
        if (cp < 0x80) {
            md_buf_putc(&p->fldinst, (int)cp);
        }
        break;
    case D_FONTTBL:
    case D_STYLESHEET:
        if (cp == ';') {
            finish_name(p, g->dest);
        } else if (cp < 0x80) {
            md_buf_putc(&p->name, (int)cp);
        }
        break;
    case D_COLORTBL:
        if (cp == ';') {
            if (p->ncolors < MAXCOLORS) {
                int mx = p->cr > p->cg ? p->cr : p->cg, mn = p->cr < p->cg ? p->cr : p->cg;

                if (p->cb > mx) {
                    mx = p->cb;
                }
                if (p->cb < mn) {
                    mn = p->cb;
                }
                p->gray[p->ncolors] = (mx - mn <= 24 && mx >= 60 && mx <= 170) ? 1 : 0;
                p->ncolors++;
            }
            p->cr = p->cg = p->cb = 0;
        }
        break;
    default:
        break;
    }
}

static void put_byte(RP *p, int b)
{
    put_cp(p, decode_byte(p, b));
}

static void put_unit(RP *p, long param)
{
    unsigned long u = (unsigned long)(param < 0 ? param + 65536L : param);

    if (u >= 0xD800UL && u <= 0xDBFFUL) {
        p->hi_surrogate = u;
        p->skip = p->st[p->depth].uc;
        return;
    }
    if (u >= 0xDC00UL && u <= 0xDFFFUL && p->hi_surrogate != 0) {
        u = 0x10000UL + ((p->hi_surrogate - 0xD800UL) << 10) + (u - 0xDC00UL);
        p->hi_surrogate = 0;
    }
    {
        int saved = p->skip;

        p->skip = 0;
        put_cp(p, u);
        p->skip = saved;
    }
    p->skip = p->st[p->depth].uc;
}

static void parse_fldinst(RP *p)
{
    const char *s = p->fldinst.p;
    const char *h;

    if (s == NULL) {
        return;
    }
    h = strstr(s, "HYPERLINK");
    if (h == NULL) {
        h = strstr(s, "hyperlink");
    }
    if (h != NULL) {
        const char *q = strchr(h, '"');

        if (q != NULL && strstr(h, "\\l ") == NULL) {                /* \l is a link within the document */
            const char *e = strchr(q + 1, '"');

            if (e != NULL && e > q + 1) {
                MdBuf url;

                md_buf_init(&url);
                md_buf_put(&url, q + 1, (size_t)(e - q - 1));
                if (!url.fail && url.p != NULL) {
                    p->field_url = add_link(p, url.p);
                }
                md_buf_free(&url);
            }
        }
    }
    md_buf_free(&p->fldinst);
}

static int is_skip_dest(const char *w)
{
    static const char *const skip[] = {
        "info", "pict", "header", "headerl", "headerr", "headerf", "footer", "footerl", "footerr", "footerf", "footnote",
        "listtable", "listoverridetable", "rsidtbl", "generator", "xmlnstbl", "themedata", "colorschememapping", "latentstyles",
        "datastore", "userprops", "revtbl", "fchars", "lchars", "pgptbl", "filetbl", "mmath", "mmathPr", "bkmkstart", "bkmkend",
        "shpinst", "object", "objdata", "private", "stylerestrictions", "background", "defchp", "defpap", "wgrffmtfilter",
        "falt", "panose", "fname", "pnseclvl", "nonshppict", "shprslt", "annotation", "atnid", "atnauthor", "txe", "nesttableprops",
        "nonesttables", "template", "docvar", "windowcaption", "comment", NULL
    };
    int i;

    for (i = 0; skip[i] != NULL; i++) {
        if (strcmp(w, skip[i]) == 0) {
            return 1;
        }
    }
    return 0;
}

static void control_word(RP *p, const char *w, int has, long v)
{
    GState *g = &p->st[p->depth];
    int first = g->first;

    g->first = 0;
    if (first || g->dest == D_NORMAL) {
        if (strcmp(w, "fonttbl") == 0) { g->dest = D_FONTTBL; return; }
        if (strcmp(w, "colortbl") == 0) { g->dest = D_COLORTBL; p->ncolors = 0; p->cr = p->cg = p->cb = 0; return; }
        if (strcmp(w, "stylesheet") == 0) { g->dest = D_STYLESHEET; p->cur_style = -1; return; }
        if (strcmp(w, "fldinst") == 0) { g->dest = D_FLDINST; md_buf_free(&p->fldinst); return; }
        if (strcmp(w, "fldrslt") == 0) { g->dest = D_FLDRSLT; g->a.link = p->field_url; return; }
        if (strcmp(w, "listtext") == 0 || strcmp(w, "pntext") == 0) { g->dest = D_MARKER; return; }
        if (first && is_skip_dest(w)) { g->dest = D_SKIP; return; }
    }
    if (g->dest == D_SKIP) {
        return;
    }
    /* the destinations that only collect names or numbers */
    if (g->dest == D_FONTTBL) {
        if (strcmp(w, "f") == 0 && has) {
            p->cur_font = (int)v;
            md_buf_free(&p->name);
        } else if (strcmp(w, "fmodern") == 0 && p->cur_font >= 0 && p->cur_font < MAXFONTS) {
            p->fonts[p->cur_font].mono = 1;
        }
        return;
    }
    if (g->dest == D_STYLESHEET) {
        if ((strcmp(w, "s") == 0) && has) {
            p->cur_style = (int)v;
            md_buf_free(&p->name);
        }
        return;
    }
    if (g->dest == D_COLORTBL) {
        if (strcmp(w, "red") == 0) p->cr = (int)v;
        else if (strcmp(w, "green") == 0) p->cg = (int)v;
        else if (strcmp(w, "blue") == 0) p->cb = (int)v;
        return;
    }
    /* document-level */
    if (strcmp(w, "rtf") == 0) {
        if (p->auto_charset && !p->seen_rtf) {
            p->charset = (has && v == 0) ? RTF_CHARSET_NEXTSTEP : RTF_CHARSET_CP1252;
        }
        p->seen_rtf = 1;
    } else if (strcmp(w, "deff") == 0 && has) {
        p->deff = (int)v;
        g->a.font = p->deff;
    } else if (strcmp(w, "uc") == 0 && has) {
        g->uc = (int)v;
    } else if (strcmp(w, "u") == 0 && has) {
        put_unit(p, v);
    } else if (strcmp(w, "field") == 0) {
        p->field_url = 0;
    } else if (strcmp(w, "plain") == 0) {
        reset_char(p, &g->a);
        g->a.link = 0;
    } else if (strcmp(w, "pard") == 0) {
        reset_para(&g->pp);
    } else if (strcmp(w, "b") == 0) {
        g->a.bold = !has || v != 0;
    } else if (strcmp(w, "i") == 0) {
        g->a.italic = !has || v != 0;
    } else if (strcmp(w, "ul") == 0 || strcmp(w, "uld") == 0 || strcmp(w, "uldb") == 0 || strcmp(w, "ulw") == 0 ||
               strcmp(w, "ulth") == 0 || strcmp(w, "uldash") == 0) {
        g->a.ul = !has || v != 0;
    } else if (strcmp(w, "ulnone") == 0) {
        g->a.ul = 0;
    } else if (strcmp(w, "strike") == 0 || strcmp(w, "striked") == 0) {
        g->a.strike = !has || v != 0;
    } else if (strcmp(w, "v") == 0) {
        g->a.hidden = !has || v != 0;
    } else if (strcmp(w, "f") == 0 && has) {
        g->a.font = (int)v;
    } else if (strcmp(w, "fs") == 0 && has) {
        g->a.fs = (int)v;
    } else if (strcmp(w, "cf") == 0 && has) {
        g->a.color = (int)v;
    } else if (strcmp(w, "li") == 0 && has) {
        g->pp.li = (int)v;
    } else if (strcmp(w, "fi") == 0 && has) {
        g->pp.fi = (int)v;
    } else if (strcmp(w, "sb") == 0 && has) {
        g->pp.sb = (int)v;
    } else if (strcmp(w, "sa") == 0 && has) {
        g->pp.sa = (int)v;
    } else if (strcmp(w, "ql") == 0) {
        g->pp.align = 0;
    } else if (strcmp(w, "qc") == 0) {
        g->pp.align = 2;
    } else if (strcmp(w, "qr") == 0) {
        g->pp.align = 3;
    } else if (strcmp(w, "qj") == 0) {
        g->pp.align = 0;
    } else if (strcmp(w, "s") == 0 && has) {
        g->pp.style = (int)v;
    } else if (strcmp(w, "intbl") == 0) {
        g->pp.intbl = 1;
    } else if (strcmp(w, "ilvl") == 0 && has) {
        g->pp.ilvl = (int)v;
    } else if (strcmp(w, "ls") == 0 && has) {
        g->pp.ls = (int)v;
    } else if (strcmp(w, "par") == 0 || strcmp(w, "sect") == 0 || strcmp(w, "page") == 0) {
        end_para(p);
    } else if (strcmp(w, "line") == 0) {
        put_cp(p, '\n');
    } else if (strcmp(w, "tab") == 0) {
        put_cp(p, '\t');
    } else if (strcmp(w, "cell") == 0 || strcmp(w, "nestcell") == 0) {
        Para *pa = cur_para(p);

        if (pa != NULL) {
            pa->cell_end = 1;
            pa->pp.intbl = 1;
        }
        p->open = 0;
    } else if (strcmp(w, "row") == 0 || strcmp(w, "nestrow") == 0) {
        Para *pa;

        p->open = 0;
        pa = cur_para(p);
        if (pa != NULL) {
            pa->row_end = 1;
            pa->pp.intbl = 1;
        }
        p->open = 0;
    } else if (strcmp(w, "emdash") == 0) {
        put_cp(p, 0x2014UL);
    } else if (strcmp(w, "endash") == 0) {
        put_cp(p, 0x2013UL);
    } else if (strcmp(w, "bullet") == 0) {
        put_cp(p, 0x2022UL);
    } else if (strcmp(w, "lquote") == 0) {
        put_cp(p, 0x2018UL);
    } else if (strcmp(w, "rquote") == 0) {
        put_cp(p, 0x2019UL);
    } else if (strcmp(w, "ldblquote") == 0) {
        put_cp(p, 0x201CUL);
    } else if (strcmp(w, "rdblquote") == 0) {
        put_cp(p, 0x201DUL);
    } else if (strcmp(w, "emspace") == 0 || strcmp(w, "enspace") == 0 || strcmp(w, "qmspace") == 0) {
        put_cp(p, ' ');
    }
}

static void group_end(RP *p, GState *old)
{
    if (old->dest == D_FLDINST) {
        parse_fldinst(p);
    } else if ((old->dest == D_FONTTBL || old->dest == D_STYLESHEET) && p->name.len > 0) {
        finish_name(p, old->dest);
    }
}

static void read_rtf(RP *p, const char *s, size_t n)
{
    size_t i = 0;

    while (i < n && !p->oom) {
        int c = (unsigned char)s[i];

        if (c == '{') {
            if (p->depth + 1 < MAXDEPTH) {
                p->st[p->depth + 1] = p->st[p->depth];
                p->depth++;
                p->st[p->depth].first = 1;
            } else {
                p->depth++;                                    /* too deep: skip it; the matching brace brings us back */
                if (p->depth >= MAXDEPTH) {
                    p->depth = MAXDEPTH - 1;
                    p->st[p->depth].dest = D_SKIP;
                    p->st[p->depth].first = 0;
                }
            }
            i++;
        } else if (c == '}') {
            GState old = p->st[p->depth];

            group_end(p, &old);
            if (p->depth > 0) {
                p->depth--;
            }
            i++;
        } else if (c == '\\') {
            int nx = i + 1 < n ? (unsigned char)s[i + 1] : 0;

            if ((nx >= 'a' && nx <= 'z') || (nx >= 'A' && nx <= 'Z')) {
                char w[32];
                size_t wl = 0;
                long v = 0;
                int has = 0, neg = 0;

                i++;
                while (i < n && ((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= 'A' && s[i] <= 'Z'))) {
                    if (wl < sizeof(w) - 1) {
                        w[wl++] = s[i];
                    }
                    i++;
                }
                w[wl] = '\0';
                if (i < n && s[i] == '-') {
                    neg = 1;
                    i++;
                }
                while (i < n && s[i] >= '0' && s[i] <= '9') {
                    v = v * 10 + (s[i] - '0');
                    if (v > 1000000L) {
                        v = 1000000L;
                    }
                    has = 1;
                    i++;
                }
                if (neg) {
                    v = -v;
                    has = has || 0;
                }
                if (i < n && s[i] == ' ') {
                    i++;
                }
                control_word(p, w, has, v);
            } else if (nx == '\'') {
                int h1 = 0, h2 = 0;
                int hv = 0;

                if (i + 3 < n + 0 && i + 3 <= n) {
                    int a = (unsigned char)s[i + 2], b = (unsigned char)s[i + 3];

                    h1 = (a >= '0' && a <= '9') ? a - '0' : (a >= 'a' && a <= 'f') ? a - 'a' + 10 : (a >= 'A' && a <= 'F') ? a - 'A' + 10 : -1;
                    h2 = (b >= '0' && b <= '9') ? b - '0' : (b >= 'a' && b <= 'f') ? b - 'a' + 10 : (b >= 'A' && b <= 'F') ? b - 'A' + 10 : -1;
                    hv = (h1 >= 0 && h2 >= 0) ? h1 * 16 + h2 : -1;
                } else {
                    hv = -1;
                }
                p->st[p->depth].first = 0;
                if (hv >= 0) {
                    put_byte(p, hv);
                    i += 4;
                } else {
                    i += 2;
                }
            } else if (nx == '\\' || nx == '{' || nx == '}') {
                p->st[p->depth].first = 0;
                put_cp(p, (unsigned long)nx);
                i += 2;
            } else if (nx == '~') {
                p->st[p->depth].first = 0;
                put_cp(p, ' ');
                i += 2;
            } else if (nx == '_') {
                p->st[p->depth].first = 0;
                put_cp(p, '-');
                i += 2;
            } else if (nx == '*') {
                /* \*\keyword: a destination this reader does not know is skipped; the known ones are taken by name */
                size_t j = i + 2;

                if (j < n && s[j] == '\\' && j + 1 < n && s[j + 1] >= 'a' && s[j + 1] <= 'z') {
                    char w[32];
                    size_t wl = 0;

                    j++;
                    while (j < n && s[j] >= 'a' && s[j] <= 'z' && wl < sizeof(w) - 1) {
                        w[wl++] = s[j++];
                    }
                    w[wl] = '\0';
                    if (strcmp(w, "fldinst") != 0 && strcmp(w, "listtext") != 0 && strcmp(w, "pntext") != 0) {
                        p->st[p->depth].dest = D_SKIP;
                        i += 2;
                        continue;
                    }
                }
                i += 2;
            } else if (nx == '\n' || nx == '\r') {
                end_para(p);
                i += 2;
            } else {
                i += 2;
            }
        } else if (c == '\r' || c == '\n') {
            i++;
        } else {
            p->st[p->depth].first = 0;
            if (c == '\t') {
                put_cp(p, '\t');
            } else {
                put_byte(p, c);
            }
            i++;
        }
    }
}

/* ------------------------------------------------------------------ the writer */

typedef struct {
    const char *t;
    size_t n;
    Attr a;
} Seg;

static int is_space_char(int c)
{
    return c == ' ' || c == '\t' || c == '\n';
}

static int para_has_text(const Para *pa)
{
    int i;
    size_t k;

    for (i = 0; i < pa->n; i++) {
        for (k = 0; k < pa->runs[i].text.len; k++) {
            if (!is_space_char((unsigned char)pa->runs[i].text.p[k])) {
                return 1;
            }
        }
    }
    return 0;
}

static void para_plain(const Para *pa, MdBuf *b)
{
    int i;

    for (i = 0; i < pa->n; i++) {
        md_buf_put(b, pa->runs[i].text.p, pa->runs[i].text.len);
    }
}

/* Markdown-escaped text; line_start says whether the next character begins a line (where #, >, -, + and 1. mean something). */
static void put_md(MdBuf *out, const char *s, size_t n, int *line_start, int cell)
{
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == ' ' || c == '\t') {
            if (*line_start || (out->len > 0 && out->p[out->len - 1] == ' ')) {
                continue;                                      /* no leading blanks (four would make code), and no doubled ones */
            }
            md_buf_putc(out, ' ');
            continue;
        }
        if (c == '\\' || c == '`' || c == '*' || c == '[' || c == ']' || c == '<') {
            md_buf_putc(out, '\\');
        } else if (c == '_') {
            int pa = i > 0 && ((s[i - 1] >= '0' && s[i - 1] <= '9') || (s[i - 1] >= 'a' && s[i - 1] <= 'z') || (s[i - 1] >= 'A' && s[i - 1] <= 'Z'));
            int na = i + 1 < n && ((s[i + 1] >= '0' && s[i + 1] <= '9') || (s[i + 1] >= 'a' && s[i + 1] <= 'z') || (s[i + 1] >= 'A' && s[i + 1] <= 'Z'));

            if (!(pa && na)) {
                md_buf_putc(out, '\\');
            }
        } else if (c == '&') {
            size_t k = i + 1;

            while (k < n && k - i < 12 && ((s[k] >= '0' && s[k] <= '9') || (s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || s[k] == '#')) {
                k++;
            }
            if (k > i + 1 && k < n && s[k] == ';') {
                md_buf_putc(out, '\\');
            }
        } else if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            md_buf_putc(out, '\\');
        } else if (c == '|' && cell) {
            md_buf_putc(out, '\\');
        } else if (*line_start && (c == '#' || c == '>' || ((c == '-' || c == '+' || c == '=') && (i + 1 >= n || s[i + 1] == ' ' || s[i + 1] == c)))) {
            md_buf_putc(out, '\\');
        } else if (*line_start && c >= '0' && c <= '9') {
            size_t k = i;

            while (k < n && s[k] >= '0' && s[k] <= '9') {
                k++;
            }
            if (k < n && (s[k] == '.' || s[k] == ')')) {
                md_buf_put(out, s + i, k - i);
                md_buf_putc(out, '\\');
                md_buf_putc(out, s[k]);
                i = k;
                *line_start = 0;
                continue;
            }
        }
        md_buf_putc(out, (char)c);
        *line_start = 0;
    }
}

static void put_close(MdBuf *out, const char *marker)
{
    size_t sp = 0;

    while (out->len > sp && out->p[out->len - 1 - sp] == ' ') {
        sp++;
    }
    out->len -= sp;
    md_buf_puts(out, marker);
    while (sp-- > 0) {
        md_buf_putc(out, ' ');
    }
    if (out->p) {
        out->p[out->len] = '\0';
    }
}

static void put_code_span(MdBuf *out, const char *s, size_t n)
{
    size_t i, run = 0, best = 0;
    int fence, k;
    int pad;

    for (i = 0; i < n; i++) {
        if (s[i] == '`') {
            run++;
            if (run > best) {
                best = run;
            }
        } else {
            run = 0;
        }
    }
    fence = (int)best + 1;
    pad = n > 0 && (s[0] == '`' || s[n - 1] == '`' || (s[0] == ' ' && s[n - 1] == ' '));
    for (k = 0; k < fence; k++) {
        md_buf_putc(out, '`');
    }
    if (pad) {
        md_buf_putc(out, ' ');
    }
    md_buf_put(out, s, n);
    if (pad) {
        md_buf_putc(out, ' ');
    }
    for (k = 0; k < fence; k++) {
        md_buf_putc(out, '`');
    }
}

typedef struct {
    int bold, italic, strike;
} Open;

/* Segments [from, to) as inline Markdown with their emphasis. */
static void emit_segs(RP *p, MdBuf *out, const Seg *seg, int from, int to, int *line_start, int cell)
{
    Open on;
    int i;

    memset(&on, 0, sizeof(on));
    for (i = from; i < to; i++) {
        const Seg *sg = &seg[i];
        int mono = p->fonts[sg->a.font >= 0 && sg->a.font < MAXFONTS ? sg->a.font : 0].mono;
        size_t a = 0, b = sg->n, pos;
        int want_b = sg->a.bold, want_i = sg->a.italic, want_s = sg->a.strike;

        if (mono) {
            MdBuf code;
            size_t k;

            md_buf_init(&code);
            for (k = 0; k < sg->n; k++) {
                md_buf_putc(&code, sg->t[k] == '\n' ? ' ' : (sg->t[k] == '\t' ? ' ' : sg->t[k]));
            }
            if (code.len > 0) {
                if (on.strike) { put_close(out, "~~"); on.strike = 0; }
                if (on.italic) { put_close(out, "*"); on.italic = 0; }
                if (on.bold) { put_close(out, "**"); on.bold = 0; }
                put_code_span(out, code.p, code.len);
                *line_start = 0;
            }
            md_buf_free(&code);
            continue;
        }
        while (a < b && (sg->t[a] == ' ' || sg->t[a] == '\t')) {
            a++;
        }
        while (b > a && (sg->t[b - 1] == ' ' || sg->t[b - 1] == '\t')) {
            b--;
        }
        if (a == b) {                                          /* only blanks (or nothing) */
            for (pos = 0; pos < sg->n; pos++) {
                if (sg->t[pos] == '\n') {
                    md_buf_puts(out, cell ? " " : "\\\n");
                    *line_start = 1;
                } else if (!*line_start && !(out->len > 0 && out->p[out->len - 1] == ' ')) {
                    md_buf_putc(out, ' ');
                }
            }
            continue;
        }
        /* leading blanks go before any marker we open */
        if (a > 0 && !*line_start && !(out->len > 0 && out->p[out->len - 1] == ' ')) {
            md_buf_putc(out, ' ');
        }
        if ((on.strike && !want_s) || (on.italic && !want_i) || (on.bold && !want_b)) {
            if (on.strike && !want_s) { put_close(out, "~~"); on.strike = 0; }
            if (on.italic && (!want_i || 0)) { put_close(out, "*"); on.italic = 0; }
            if (on.bold && !want_b) {
                if (on.italic) { put_close(out, "*"); on.italic = 0; }
                put_close(out, "**");
                on.bold = 0;
            }
        }
        if (want_b && !on.bold) {
            md_buf_puts(out, "**");
            on.bold = 1;
        }
        if (want_i && !on.italic) {
            md_buf_puts(out, "*");
            on.italic = 1;
        }
        if (want_s && !on.strike) {
            md_buf_puts(out, "~~");
            on.strike = 1;
        }
        /* the text itself, with hard breaks */
        {
            size_t st = a;

            for (pos = a; pos <= b; pos++) {
                if (pos == b || sg->t[pos] == '\n') {
                    put_md(out, sg->t + st, pos - st, line_start, cell);
                    if (pos < b) {
                        if (on.strike) { put_close(out, "~~"); on.strike = 0; want_s = 1; }
                        if (on.italic) { put_close(out, "*"); on.italic = 0; }
                        if (on.bold) { put_close(out, "**"); on.bold = 0; }
                        md_buf_puts(out, cell ? " " : "\\\n");
                        *line_start = 1;
                        if (want_b) { md_buf_puts(out, "**"); on.bold = 1; }
                        if (want_i) { md_buf_puts(out, "*"); on.italic = 1; }
                        if (want_s) { md_buf_puts(out, "~~"); on.strike = 1; }
                    }
                    st = pos + 1;
                }
            }
        }
        if (b < sg->n && sg->t[b] != '\n' && !(out->len > 0 && out->p[out->len - 1] == ' ')) {
            md_buf_putc(out, ' ');
        }
    }
    if (on.strike) { put_close(out, "~~"); }
    if (on.italic) { put_close(out, "*"); }
    if (on.bold) { put_close(out, "**"); }
}

static int bare_url_seg(const Seg *s)
{
    size_t k;

    if (!s->a.ul || s->n <= 7 || !(memcmp(s->t, "http://", 7) == 0 || memcmp(s->t, "https://", 8) == 0 || memcmp(s->t, "mailto:", 7) == 0)) {
        return 0;
    }
    for (k = 0; k < s->n; k++) {
        if (s->t[k] == ' ' || s->t[k] == '\n') {
            return 0;
        }
    }
    return 1;
}

static int looks_like_url_tail(const Seg *s, size_t *ustart, size_t *ulen, int strict)
{
    size_t a = 0, b = s->n, k;

    while (a < b && s->t[a] == ' ') {
        a++;
    }
    while (b > a && s->t[b - 1] == ' ') {
        b--;
    }
    if (b - a < 3 || s->t[a] != '(' || s->t[b - 1] != ')') {
        return 0;
    }
    *ustart = a + 1;
    *ulen = b - a - 2;
    for (k = *ustart; k < *ustart + *ulen; k++) {
        if (s->t[k] == ' ' || s->t[k] == '\n') {
            return 0;
        }
    }
    if (strict && strstr("", "") == NULL) {
        MdBuf t;
        int ok;

        md_buf_init(&t);
        md_buf_put(&t, s->t + *ustart, *ulen);
        ok = t.p != NULL && (strstr(t.p, "://") != NULL || strncmp(t.p, "mailto:", 7) == 0 || strncmp(t.p, "www.", 4) == 0);
        md_buf_free(&t);
        return ok;
    }
    return 1;
}

/* A paragraph's runs, from byte offset skip, as one line of Markdown (links, images and emphasis resolved). */
static void emit_para(RP *p, const Para *pa, size_t skip, MdBuf *out, int cell)
{
    Seg *seg = (Seg *)malloc((size_t)(pa->n + 1) * sizeof(Seg));
    int ns = 0, i, line_start = 1;
    size_t n0 = out->len;

    if (seg == NULL) {
        p->oom = 1;
        return;
    }
    for (i = 0; i < pa->n; i++) {
        const Run *r = &pa->runs[i];
        size_t off = 0;

        if (skip > 0) {
            if (skip >= r->text.len) {
                skip -= r->text.len;
                continue;
            }
            off = skip;
            skip = 0;
        }
        if (r->text.len - off == 0) {
            continue;
        }
        seg[ns].t = r->text.p + off;
        seg[ns].n = r->text.len - off;
        seg[ns].a = r->a;
        if (p->strip_bold) {
            seg[ns].a.bold = 0;
        }
        ns++;
    }
    i = 0;
    while (i < ns) {
        int j = i;
        size_t us, ul;

        if (seg[i].a.link > 0 && seg[i].a.link <= p->nlinks) {
            MdBuf inner;
            int ls2 = 0;

            while (j < ns && seg[j].a.link == seg[i].a.link) {
                j++;
            }
            md_buf_init(&inner);
            emit_segs(p, &inner, seg, i, j, &ls2, cell);
            md_buf_putc(out, '[');
            if (inner.p) {
                md_buf_put(out, inner.p, inner.len);
            }
            md_buf_puts(out, "](");
            md_buf_puts(out, p->links[seg[i].a.link - 1]);
            md_buf_putc(out, ')');
            md_buf_free(&inner);
            line_start = 0;
            i = j;
            continue;
        }
        if (seg[i].a.italic && seg[i].n >= 6 && memcmp(seg[i].t, "[image", 6) == 0 && seg[i].t[seg[i].n - 1] == ']') {
            size_t as = 6, ae = seg[i].n - 1;
            MdBuf alt;
            int ls2 = 0;
            int have_tail = i + 1 < ns && looks_like_url_tail(&seg[i + 1], &us, &ul, 0);

            if (as < ae && seg[i].t[as] == ':') {
                as++;
                while (as < ae && seg[i].t[as] == ' ') {
                    as++;
                }
            }
            md_buf_init(&alt);
            put_md(&alt, seg[i].t + as, ae - as, &ls2, cell);
            md_buf_puts(out, "![");
            if (alt.p) {
                md_buf_put(out, alt.p, alt.len);
            }
            md_buf_puts(out, "](");
            if (have_tail) {
                md_buf_put(out, seg[i + 1].t + us, ul);
            }
            md_buf_putc(out, ')');
            md_buf_free(&alt);
            line_start = 0;
            i += have_tail ? 2 : 1;
            continue;
        }
        if (seg[i].a.ul && i + 1 < ns && looks_like_url_tail(&seg[i + 1], &us, &ul, 1)) {
            MdBuf inner;
            int ls2 = 0;

            md_buf_init(&inner);
            emit_segs(p, &inner, seg, i, i + 1, &ls2, cell);
            md_buf_putc(out, '[');
            if (inner.p) {
                md_buf_put(out, inner.p, inner.len);
            }
            md_buf_puts(out, "](");
            md_buf_put(out, seg[i + 1].t + us, ul);
            md_buf_putc(out, ')');
            md_buf_free(&inner);
            line_start = 0;
            i += 2;
            continue;
        }
        if (seg[i].a.ul && (seg[i].n > 7 && (memcmp(seg[i].t, "http://", 7) == 0 || memcmp(seg[i].t, "https://", 8) == 0 ||
                                              memcmp(seg[i].t, "mailto:", 7) == 0))) {
            size_t k;
            int bare = 1;

            for (k = 0; k < seg[i].n; k++) {
                if (seg[i].t[k] == ' ' || seg[i].t[k] == '\n') {
                    bare = 0;
                }
            }
            if (bare) {
                md_buf_putc(out, '<');
                md_buf_put(out, seg[i].t, seg[i].n);
                md_buf_putc(out, '>');
                line_start = 0;
                i++;
                continue;
            }
        }
        /* ordinary segments up to the next special one */
        while (j < ns && !(seg[j].a.link > 0 && seg[j].a.link <= p->nlinks) && !(j > i && seg[j].a.ul && j + 1 < ns && looks_like_url_tail(&seg[j + 1], &us, &ul, 1)) &&
               !(j > i && seg[j].a.italic && seg[j].n >= 6 && memcmp(seg[j].t, "[image", 6) == 0) && !(j > i && bare_url_seg(&seg[j]))) {
            j++;
        }
        if (j == i) {
            j = i + 1;
        }
        emit_segs(p, out, seg, i, j, &line_start, cell);
        i = j;
    }
    free(seg);
    /* nothing left dangling at the end */
    for (;;) {
        size_t m = out->len;

        if (m > n0 + 1 && out->p[m - 1] == '\n' && out->p[m - 2] == '\\') {
            out->len -= 2;
        } else if (m > n0 && (out->p[m - 1] == ' ' || out->p[m - 1] == '\n')) {
            out->len -= 1;
        } else {
            break;
        }
    }
    if (out->p) {
        out->p[out->len] = '\0';
    }
}

/* ---- classification ---- */

enum { K_EMPTY = 0, K_PARA, K_HEAD, K_LIST, K_CODE, K_QUOTE, K_RULE, K_TABLE };

typedef struct {
    int kind;
    int level;                   /* heading level, list nesting level, quote depth */
    int ordered;
    long number;
    int task;                    /* 0 an ordinary item, 1 an open task [ ], 2 a done task [x] */
    size_t skip;                 /* bytes of the paragraph's text taken up by a list marker */
} Cls;

static int is_bullet_cp(unsigned long cp)
{
    return cp == 0x2022UL || cp == 0x25E6UL || cp == 0x25AAUL || cp == 0x25ABUL || cp == 0x25A0UL || cp == 0x25A1UL || cp == 0x25CFUL ||
           cp == 0x25CBUL || cp == 0xB7UL || cp == 0x2219UL || cp == 0xF0B7UL || cp == 0x2013UL || cp == 0x2014UL || cp == 0xF0A7UL ||
           cp == 0x2023UL || cp == 0x2043UL;
}

/* A list marker at the start of s (n bytes): sets ordered/number, returns the bytes it and its spacing take, or 0. */
static size_t detect_marker(const char *s, size_t n, int *ordered, long *number, int marker_is_symbol, int *task)
{
    size_t i = 0, adv, j;
    unsigned long cp;

    while (i < n && (s[i] == ' ')) {
        i++;
    }
    if (i >= n) {
        return 0;
    }
    *task = 0;
    if (i + 2 < n && s[i] == '[' && (s[i + 1] == ' ' || s[i + 1] == 'x' || s[i + 1] == 'X') && s[i + 2] == ']') {
        size_t k = i + 3;

        if (k < n && (s[k] == '\t' || s[k] == ' ')) {
            while (k < n && (s[k] == '\t' || s[k] == ' ')) {
                k++;
            }
            if (k < n) {
                *ordered = 0;
                *number = 0;
                *task = s[i + 1] == ' ' ? 1 : 2;
                return k;
            }
        }
    }
    cp = md_utf8_decode(s + i, n - i, &adv);
    if (is_bullet_cp(cp) || (marker_is_symbol && cp >= 0x20 && cp != ' ') || cp == '-' || cp == '*' || cp == '+' || cp == 'o') {
        size_t k = i + adv;
        int tab = 0, spaces = 0;

        while (k < n && (s[k] == '\t' || s[k] == ' ')) {
            if (s[k] == '\t') {
                tab = 1;
            } else {
                spaces++;
            }
            k++;
        }
        if (k == i + adv || k >= n) {
            return 0;
        }
        if (cp == 'o' && !tab) {
            return 0;
        }
        if ((cp == '-' || cp == '*' || cp == '+') && !tab && spaces < 1) {
            return 0;
        }
        if (cp == 0x2013UL || cp == 0x2014UL) {
            if (!tab && spaces < 2) {
                return 0;
            }
        }
        *ordered = 0;
        *number = 0;
        return k;
    }
    if (cp >= '0' && cp <= '9') {
        long v = 0;
        int digits = 0;

        j = i;
        while (j < n && s[j] >= '0' && s[j] <= '9' && digits < 4) {
            v = v * 10 + (s[j] - '0');
            digits++;
            j++;
        }
        if (j < n && (s[j] == '.' || s[j] == ')') && digits <= 3) {
            size_t k = j + 1;
            int any = 0;

            while (k < n && (s[k] == '\t' || s[k] == ' ')) {
                any = 1;
                k++;
            }
            if (any && k < n) {
                *ordered = 1;
                *number = v;
                return k;
            }
        }
        return 0;
    }
    if (((cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z')) && i + 1 < n && (s[i + 1] == '.' || s[i + 1] == ')')) {
        size_t k = i + 2;
        int tab = 0;

        while (k < n && (s[k] == '\t' || s[k] == ' ')) {
            if (s[k] == '\t') {
                tab = 1;
            }
            k++;
        }
        if (tab && k < n) {
            *ordered = 1;
            *number = (cp >= 'a' ? cp - 'a' : cp - 'A') + 1;
            return k;
        }
    }
    return 0;
}

static int style_is(const RP *p, int style, const char *prefix)
{
    const char *name;
    size_t l = strlen(prefix), i;

    if (style < 0 || style >= MAXSTYLES) {
        return 0;
    }
    name = p->styles[style];
    if (strlen(name) < l) {
        return 0;
    }
    for (i = 0; i < l; i++) {
        char c = name[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c + 32);
        }
        if (c != prefix[i]) {
            return 0;
        }
    }
    return 1;
}

static int rule_text(const char *s, size_t n)
{
    size_t i, cnt = 0;
    unsigned long first = 0;

    for (i = 0; i < n;) {
        size_t adv;
        unsigned long cp = md_utf8_decode(s + i, n - i, &adv);

        i += adv;
        if (cp == ' ' || cp == '\t') {
            continue;
        }
        if (!(cp == 0x2014UL || cp == 0x2013UL || cp == '-' || cp == '_' || cp == '=' || cp == 0x2500UL || cp == 0x2501UL || cp == '*')) {
            return 0;
        }
        if (first == 0) {
            first = cp;
        } else if (cp != first) {
            return 0;
        }
        cnt++;
    }
    return cnt >= 6;
}

static int para_all_mono(const RP *p, const Para *pa)
{
    int i, any = 0;
    size_t k;

    for (i = 0; i < pa->n; i++) {
        int has = 0;

        for (k = 0; k < pa->runs[i].text.len; k++) {
            if (!is_space_char((unsigned char)pa->runs[i].text.p[k])) {
                has = 1;
                break;
            }
        }
        if (has) {
            any = 1;
            if (!p->fonts[pa->runs[i].a.font >= 0 && pa->runs[i].a.font < MAXFONTS ? pa->runs[i].a.font : 0].mono) {
                return 0;
            }
        }
    }
    return any;
}

static int para_all_bold(const Para *pa, int *maxfs)
{
    int i, any = 0;
    size_t k;

    *maxfs = 0;
    for (i = 0; i < pa->n; i++) {
        int has = 0;

        for (k = 0; k < pa->runs[i].text.len; k++) {
            if (!is_space_char((unsigned char)pa->runs[i].text.p[k])) {
                has = 1;
                break;
            }
        }
        if (has) {
            any = 1;
            if (!pa->runs[i].a.bold) {
                return 0;
            }
            if (pa->runs[i].a.fs > *maxfs) {
                *maxfs = pa->runs[i].a.fs;
            }
        }
    }
    return any;
}

static int para_gray_or_italic(const RP *p, const Para *pa)
{
    int i, total = 0, hit = 0;

    for (i = 0; i < pa->n; i++) {
        size_t len = pa->runs[i].text.len;

        total += (int)len;
        if (pa->runs[i].a.italic || (pa->runs[i].a.color > 0 && pa->runs[i].a.color < MAXCOLORS && p->gray[pa->runs[i].a.color])) {
            hit += (int)len;
        }
    }
    return total > 0 && hit * 2 >= total;
}

/* How many twips apart two indents may be and still count as one level. */
#define LEVEL_SLOP 120

static int level_for(const int *vals, int nvals, int li)
{
    int i, rank = 0;

    for (i = 0; i < nvals; i++) {
        if (vals[i] < li - LEVEL_SLOP) {
            rank++;
        }
    }
    return rank;
}

static void collect_levels(int *vals, int *nvals, int li)
{
    int i;

    for (i = 0; i < *nvals; i++) {
        if (vals[i] >= li - LEVEL_SLOP && vals[i] <= li + LEVEL_SLOP) {
            return;
        }
    }
    if (*nvals < 32) {
        vals[(*nvals)++] = li;
    }
}

/* distinct values, ranked: a level is the number of clearly smaller distinct values */
static int distinct_ranks(const int *raw, int nraw, int *out)
{
    int n = 0, i;

    for (i = 0; i < nraw; i++) {
        collect_levels(out, &n, raw[i]);
    }
    return n;
}

static void emit_list_prefix(MdBuf *out, int level, int ordered, long number, int task)
{
    int k;

    for (k = 0; k < level * 4; k++) {
        md_buf_putc(out, ' ');
    }
    if (ordered) {
        md_buf_putint(out, number > 0 ? number : 1);
        md_buf_puts(out, ". ");
    } else {
        md_buf_puts(out, "- ");
    }
    if (task) {
        md_buf_puts(out, task == 2 ? "[x] " : "[ ] ");
    }
}

static void emit_quote_lines(MdBuf *out, const char *text, size_t len, int depth)
{
    size_t i = 0, st = 0;
    int k;

    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            for (k = 0; k < depth; k++) {
                md_buf_puts(out, "> ");
            }
            md_buf_put(out, text + st, i - st);
            if (i == st) {
                /* a blank line inside the quote */
                if (out->len > 0 && out->p[out->len - 1] == ' ') {
                    out->len--;
                }
            }
            md_buf_putc(out, '\n');
            st = i + 1;
        }
    }
}

static int looks_like_md_table(const char *s, size_t n)
{
    size_t i = 0;
    int line = 0;

    while (i < n) {
        size_t e = i;

        while (e < n && s[e] != '\n') {
            e++;
        }
        if (e == i || s[i] != '|' || s[e - 1] != '|') {
            return 0;
        }
        if (line == 1) {
            size_t k;

            for (k = i; k < e; k++) {
                if (s[k] != '|' && s[k] != '-' && s[k] != ':' && s[k] != ' ') {
                    return 0;
                }
            }
        }
        line++;
        i = e + 1;
    }
    return line >= 2;
}

static void emit_code(MdBuf *out, const char *text, size_t len)
{
    size_t i, run = 0, best = 0;
    int fence, k;
    size_t st = 0;

    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == ' ')) {
        len--;
    }
    while (len > 0 && text[0] == '\n') {
        text++;
        len--;
    }
    if (len == 0) {
        return;
    }
    if (looks_like_md_table(text, len)) {                      /* a table this project wrote as aligned text: back to a table */
        md_buf_put(out, text, len);
        md_buf_putc(out, '\n');
        return;
    }
    for (i = 0; i < len; i++) {
        if (text[i] == '`') {
            run++;
            if (run > best) {
                best = run;
            }
        } else {
            run = 0;
        }
    }
    fence = best >= 3 ? (int)best + 1 : 3;
    for (k = 0; k < fence; k++) {
        md_buf_putc(out, '`');
    }
    md_buf_putc(out, '\n');
    for (i = 0; i <= len; i++) {
        if (i == len || text[i] == '\n') {
            size_t e = i;

            while (e > st && text[e - 1] == ' ') {
                e--;
            }
            md_buf_put(out, text + st, e - st);
            md_buf_putc(out, '\n');
            st = i + 1;
        }
    }
    for (k = 0; k < fence; k++) {
        md_buf_putc(out, '`');
    }
    md_buf_putc(out, '\n');
}

static void blank_line(MdBuf *out)
{
    if (out->len == 0) {
        return;
    }
    if (out->len >= 2 && out->p[out->len - 1] == '\n' && out->p[out->len - 2] == '\n') {
        return;
    }
    if (out->p[out->len - 1] != '\n') {
        md_buf_putc(out, '\n');
    }
    md_buf_putc(out, '\n');
}

typedef struct {
    MdBuf text;
    int align;
} Cell;

static void emit_table(RP *p, int first, int *next, MdBuf *out)
{
    int i = first, nrows = 0, rowcap = 8;
    Cell **rows = (Cell **)malloc((size_t)rowcap * sizeof(Cell *));
    int *rowlen = (int *)malloc((size_t)rowcap * sizeof(int));
    int cur_cap = 8, cur_n = 0, r, c, ncols = 0;
    Cell *cur = (Cell *)malloc((size_t)cur_cap * sizeof(Cell));
    MdBuf celltext;
    int cell_align = 0, cell_has = 0;

    md_buf_init(&celltext);
    if (rows == NULL || rowlen == NULL || cur == NULL) {
        free(rows);
        free(rowlen);
        free(cur);
        *next = first + 1;
        return;
    }
    while (i < p->npara && p->paras[i].pp.intbl) {
        const Para *pa = &p->paras[i];

        if (pa->row_end) {
            if (cur_n > 0) {
                if (nrows >= rowcap) {
                    rowcap *= 2;
                    rows = (Cell **)realloc(rows, (size_t)rowcap * sizeof(Cell *));
                    rowlen = (int *)realloc(rowlen, (size_t)rowcap * sizeof(int));
                }
                rows[nrows] = cur;
                rowlen[nrows] = cur_n;
                nrows++;
                if (cur_n > ncols) {
                    ncols = cur_n;
                }
                cur = (Cell *)malloc((size_t)cur_cap * sizeof(Cell));
                cur_n = 0;
            }
            i++;
            continue;
        }
        if (para_has_text(pa)) {
            if (celltext.len > 0) {
                md_buf_putc(&celltext, ' ');
            }
            emit_para(p, pa, 0, &celltext, 1);
        }
        if (!cell_has) {
            cell_align = pa->pp.align;
            cell_has = 1;
        }
        if (pa->cell_end) {
            if (cur_n >= cur_cap) {
                cur_cap *= 2;
                cur = (Cell *)realloc(cur, (size_t)cur_cap * sizeof(Cell));
            }
            cur[cur_n].text = celltext;
            cur[cur_n].align = cell_align;
            cur_n++;
            md_buf_init(&celltext);
            cell_has = 0;
        }
        i++;
    }
    if (celltext.len > 0 || cell_has) {                         /* a last cell with no \cell */
        if (cur_n >= cur_cap) {
            cur_cap *= 2;
            cur = (Cell *)realloc(cur, (size_t)cur_cap * sizeof(Cell));
        }
        cur[cur_n].text = celltext;
        cur[cur_n].align = cell_align;
        cur_n++;
    } else {
        md_buf_free(&celltext);
    }
    if (cur_n > 0) {
        if (nrows >= rowcap) {
            rowcap *= 2;
            rows = (Cell **)realloc(rows, (size_t)rowcap * sizeof(Cell *));
            rowlen = (int *)realloc(rowlen, (size_t)rowcap * sizeof(int));
        }
        rows[nrows] = cur;
        rowlen[nrows] = cur_n;
        nrows++;
        if (cur_n > ncols) {
            ncols = cur_n;
        }
    } else {
        free(cur);
    }
    blank_line(out);
    for (r = 0; r < nrows; r++) {
        md_buf_putc(out, '|');
        for (c = 0; c < ncols; c++) {
            md_buf_putc(out, ' ');
            if (c < rowlen[r] && rows[r][c].text.p != NULL) {
                md_buf_put(out, rows[r][c].text.p, rows[r][c].text.len);
            }
            md_buf_puts(out, " |");
        }
        md_buf_putc(out, '\n');
        if (r == 0) {
            int row_for_align = nrows > 1 ? 1 : 0;

            md_buf_putc(out, '|');
            for (c = 0; c < ncols; c++) {
                int al = (c < rowlen[row_for_align]) ? rows[row_for_align][c].align : 0;

                md_buf_puts(out, al == 2 ? " :---: |" : al == 3 ? " ---: |" : " --- |");
            }
            md_buf_putc(out, '\n');
        }
    }
    for (r = 0; r < nrows; r++) {
        for (c = 0; c < rowlen[r]; c++) {
            md_buf_free(&rows[r][c].text);
        }
        free(rows[r]);
    }
    free(rows);
    free(rowlen);
    *next = i;
}

static void write_markdown(RP *p, const RtfMdOptions *o, MdBuf *out)
{
    int n = p->npara, i;
    Cls *cls = (Cls *)calloc((size_t)(n > 0 ? n : 1), sizeof(Cls));
    int body_fs = 24;
    long weight[512];
    int raw_li[4096], nraw = 0, lvl_vals[32], nlvl = 0, base_li = 0;
    int in_list = 0, prev_list_level = 0, prev_kind = K_EMPTY, prev_top_ordered = 0;
    int modal_li_zero = 1;

    if (cls == NULL) {
        p->oom = 1;
        return;
    }
    memset(weight, 0, sizeof(weight));
    /* the usual size: whatever most of the text is set in */
    for (i = 0; i < n; i++) {
        const Para *pa = &p->paras[i];
        int k;

        if (pa->pp.intbl) {
            continue;
        }
        for (k = 0; k < pa->n; k++) {
            int fs = pa->runs[k].a.fs;

            if (fs > 0 && fs < 512 && !pa->runs[k].a.bold && !p->fonts[pa->runs[k].a.font >= 0 && pa->runs[k].a.font < MAXFONTS ? pa->runs[k].a.font : 0].mono) {
                weight[fs] += (long)pa->runs[k].text.len;
            }
        }
    }
    {
        long best = 0;

        for (i = 1; i < 512; i++) {
            if (weight[i] > best) {
                best = weight[i];
                body_fs = i;
            }
        }
        if (best == 0) {                                       /* all of it bold: the smallest size is the body */
            int lo = 0;

            for (i = 0; i < n; i++) {
                int k;

                for (k = 0; k < p->paras[i].n; k++) {
                    int fs = p->paras[i].runs[k].a.fs;

                    if (fs > 0 && p->paras[i].runs[k].text.len > 0 && (lo == 0 || fs < lo)) {
                        lo = fs;
                    }
                }
            }
            if (lo > 0) {
                body_fs = lo;
            }
        }
    }
    /* classify */
    for (i = 0; i < n; i++) {
        Para *pa = &p->paras[i];
        Cls *c = &cls[i];
        MdBuf txt;
        int maxfs;
        int style = pa->pp.style;

        if (pa->pp.intbl) {
            c->kind = K_TABLE;
            continue;
        }
        if (!para_has_text(pa) && pa->marker.len == 0) {
            c->kind = K_EMPTY;
            continue;
        }
        md_buf_init(&txt);
        para_plain(pa, &txt);
        if (txt.len > 0 && rule_text(txt.p, txt.len)) {
            c->kind = K_RULE;
            md_buf_free(&txt);
            continue;
        }
        /* a heading by style, or by being short, bold and large */
        if (style_is(p, style, "heading ") && p->styles[style][8] >= '1' && p->styles[style][8] <= '6') {
            c->kind = K_HEAD;
            c->level = p->styles[style][8] - '0';
        } else if (style_is(p, style, "title")) {
            c->kind = K_HEAD;
            c->level = 1;
        } else if (style_is(p, style, "subtitle")) {
            c->kind = K_HEAD;
            c->level = 2;
        } else if (pa->marker.len == 0 && para_all_bold(pa, &maxfs) && maxfs * 100 >= body_fs * 105 && txt.len <= 200 &&
                   memchr(txt.p ? txt.p : "", '\n', txt.len) == NULL) {
            int pct = maxfs * 100 / body_fs;

            c->kind = K_HEAD;
            c->level = pct >= 165 ? 1 : pct >= 140 ? 2 : pct >= 120 ? 3 : 4;
        } else if (para_all_mono(p, pa) || style_is(p, style, "code") || style_is(p, style, "preformatted") || style_is(p, style, "source code")) {
            c->kind = K_CODE;
        } else {
            long num = 0;
            int ordered = 0;
            size_t used = 0;
            int symbol = 0;

            if (pa->marker.len > 0) {
                symbol = pa->marker_font >= 0 && pa->marker_font < MAXFONTS && p->fonts[pa->marker_font].symbol;
                used = detect_marker(pa->marker.p, pa->marker.len, &ordered, &num, symbol, &c->task);
                if (used == 0 && pa->marker.len > 0) {                 /* a marker group we can't read: still a list item, a bullet */
                    ordered = 0;
                    used = 0;
                    c->kind = K_LIST;
                    c->ordered = 0;
                    c->skip = 0;
                }
                if (used > 0 || c->kind == K_LIST) {
                    c->kind = K_LIST;
                    c->ordered = ordered;
                    c->number = num;
                    c->skip = 0;
                }
            } else {
                used = detect_marker(txt.p ? txt.p : "", txt.len, &ordered, &num, 0, &c->task);
                if (used > 0) {
                    c->kind = K_LIST;
                    c->ordered = ordered;
                    c->number = num;
                    c->skip = used;
                }
            }
            if (c->kind == K_LIST) {
                if (nraw < 4096) {
                    raw_li[nraw++] = pa->pp.li;
                }
            } else if (o->quotes && (style_is(p, style, "quote") || style_is(p, style, "block")) ) {
                c->kind = K_QUOTE;
            } else if (o->quotes && pa->pp.li >= 300 && pa->pp.fi <= 0 && para_gray_or_italic(p, pa)) {
                c->kind = K_QUOTE;
            } else {
                c->kind = K_PARA;
            }
        }
        md_buf_free(&txt);
    }
    nlvl = distinct_ranks(raw_li, nraw, lvl_vals);
    (void)base_li;
    (void)modal_li_zero;
    for (i = 0; i < n; i++) {
        if (cls[i].kind == K_LIST) {
            cls[i].level = (p->paras[i].pp.ilvl >= 0 && p->paras[i].pp.ilvl < 8) ? p->paras[i].pp.ilvl : level_for(lvl_vals, nlvl, p->paras[i].pp.li);
        } else if (cls[i].kind == K_QUOTE) {
            int d = (p->paras[i].pp.li + 180) / 360;

            cls[i].level = d < 1 ? 1 : d > 4 ? 4 : d;
        }
    }
    /* write */
    for (i = 0; i < n && !p->oom; ) {
        Para *pa = &p->paras[i];
        Cls *c = &cls[i];
        MdBuf line;

        switch (c->kind) {
        case K_EMPTY:
            i++;
            continue;
        case K_TABLE: {
            int next;

            emit_table(p, i, &next, out);
            prev_kind = K_TABLE;
            in_list = 0;
            i = next;
            continue;
        }
        case K_HEAD: {
            int k;

            md_buf_init(&line);
            p->strip_bold = 1;
            emit_para(p, pa, 0, &line, 1);
            p->strip_bold = 0;
            blank_line(out);
            for (k = 0; k < c->level; k++) {
                md_buf_putc(out, '#');
            }
            md_buf_putc(out, ' ');
            if (line.p) {
                md_buf_put(out, line.p, line.len);
            }
            md_buf_putc(out, '\n');
            md_buf_free(&line);
            in_list = 0;
            prev_kind = K_HEAD;
            i++;
            continue;
        }
        case K_RULE:
            blank_line(out);
            md_buf_puts(out, "---\n");
            in_list = 0;
            prev_kind = K_RULE;
            i++;
            continue;
        case K_CODE: {
            MdBuf code;
            int j = i;
            int blanks = 0;

            md_buf_init(&code);
            while (j < n) {
                if (cls[j].kind == K_CODE && (j == i || (p->paras[j - 1].pp.sa < 60 && p->paras[j].pp.sb < 60) || blanks > 0)) {
                    int b;

                    for (b = 0; b < blanks; b++) {
                        md_buf_putc(&code, '\n');
                    }
                    blanks = 0;
                    {
                        MdBuf raw;
                        size_t k;

                        md_buf_init(&raw);
                        para_plain(&p->paras[j], &raw);
                        for (k = 0; k < raw.len; k++) {
                            if (raw.p[k] == '\t') {
                                md_buf_puts(&code, "    ");
                            } else {
                                md_buf_putc(&code, raw.p[k]);
                            }
                        }
                        md_buf_free(&raw);
                    }
                    md_buf_putc(&code, '\n');
                    j++;
                } else if (cls[j].kind == K_EMPTY && j + 1 < n && (cls[j + 1].kind == K_CODE || cls[j + 1].kind == K_EMPTY)) {
                    blanks++;
                    j++;
                } else {
                    break;
                }
            }
            blank_line(out);
            if (code.p) {
                emit_code(out, code.p, code.len);
            }
            md_buf_free(&code);
            in_list = 0;
            prev_kind = K_CODE;
            i = j;
            continue;
        }
        case K_QUOTE: {
            MdBuf q;
            int depth = c->level, j = i;

            md_buf_init(&q);
            while (j < n && cls[j].kind == K_QUOTE && cls[j].level == depth) {
                if (j > i) {
                    md_buf_putc(&q, '\n');
                    md_buf_putc(&q, '\n');
                }
                emit_para(p, &p->paras[j], 0, &q, 0);
                j++;
            }
            blank_line(out);
            if (q.p) {
                emit_quote_lines(out, q.p, q.len, depth);
            }
            md_buf_free(&q);
            in_list = 0;
            prev_kind = K_QUOTE;
            i = j;
            continue;
        }
        case K_LIST: {
            size_t skip = c->skip;

            md_buf_init(&line);
            emit_para(p, pa, skip, &line, 0);
            if (prev_kind != K_LIST || (c->level == 0 && prev_list_level == 0 && c->ordered != prev_top_ordered)) {
                blank_line(out);
            } else if (out->len > 0 && out->p[out->len - 1] != '\n') {
                md_buf_putc(out, '\n');
            }
            emit_list_prefix(out, c->level, c->ordered, c->number, c->task);
            if (line.p) {
                md_buf_put(out, line.p, line.len);
            }
            md_buf_putc(out, '\n');
            md_buf_free(&line);
            in_list = 1;
            prev_list_level = c->level;
            if (c->level == 0) {
                prev_top_ordered = c->ordered;
            }
            prev_kind = K_LIST;
            i++;
            continue;
        }
        default:
            break;
        }
        /* an ordinary paragraph; one that follows a list item and is indented at least as far belongs to that item */
        md_buf_init(&line);
        emit_para(p, pa, 0, &line, 0);
        if (in_list && pa->pp.li >= 300 && prev_kind == K_LIST) {
            size_t st = 0, k;
            int ind = (prev_list_level + 1) * 4;

            md_buf_putc(out, '\n');
            for (k = 0; k <= line.len; k++) {
                if (k == line.len || line.p[k] == '\n') {
                    int sp;

                    for (sp = 0; sp < ind && k > st; sp++) {
                        md_buf_putc(out, ' ');
                    }
                    md_buf_put(out, line.p + st, k - st);
                    md_buf_putc(out, '\n');
                    st = k + 1;
                }
            }
            prev_kind = K_LIST;                                    /* the item goes on: more of it, or the next item */
        } else {
            blank_line(out);
            if (line.p) {
                md_buf_put(out, line.p, line.len);
            }
            md_buf_putc(out, '\n');
            in_list = 0;
            prev_kind = K_PARA;
        }
        md_buf_free(&line);
        i++;
    }
    free(cls);
}

char *rtf_to_markdown(const char *rtf, size_t len, const RtfMdOptions *opt, size_t *outlen)
{
    RP *p = (RP *)calloc(1, sizeof(RP));
    RtfMdOptions def;
    MdBuf out;
    int i, k;
    char *result;

    if (p == NULL) {
        return NULL;
    }
    if (opt == NULL) {
        rtf_md_defaults(&def);
        opt = &def;
    }
    p->charset = opt->charset == RTF_CHARSET_NEXTSTEP ? RTF_CHARSET_NEXTSTEP : RTF_CHARSET_CP1252;
    p->auto_charset = opt->charset == RTF_CHARSET_AUTO;
    p->deff = 0;
    p->cur_font = -1;
    p->cur_style = -1;
    md_buf_init(&p->name);
    md_buf_init(&p->fldinst);
    reset_char(p, &p->st[0].a);
    reset_para(&p->st[0].pp);
    p->st[0].uc = 1;
    p->st[0].dest = D_NORMAL;
    p->st[0].first = 1;
    read_rtf(p, rtf != NULL ? rtf : "", rtf != NULL ? len : 0);
    md_buf_init(&out);
    if (!p->oom) {
        write_markdown(p, opt, &out);
    }
    for (i = 0; i < p->npara; i++) {
        for (k = 0; k < p->paras[i].n; k++) {
            md_buf_free(&p->paras[i].runs[k].text);
        }
        free(p->paras[i].runs);
        md_buf_free(&p->paras[i].marker);
    }
    free(p->paras);
    for (i = 0; i < p->nlinks; i++) {
        free(p->links[i]);
    }
    free(p->links);
    md_buf_free(&p->name);
    md_buf_free(&p->fldinst);
    if (p->oom || out.fail) {
        md_buf_free(&out);
        free(p);
        return NULL;
    }
    if (out.len > 0 && out.p[out.len - 1] != '\n') {
        md_buf_putc(&out, '\n');
    }
    result = md_buf_finish(&out, outlen);
    free(p);
    return result;
}
