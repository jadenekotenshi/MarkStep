/* md_html.c -- the tree as HTML, shaped like the CommonMark reference renderer's output (the tests compare against it). */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"

static void esc_html(MdBuf *b, const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        switch (s[i]) {
        case '&': md_buf_puts(b, "&amp;"); break;
        case '<': md_buf_puts(b, "&lt;"); break;
        case '>': md_buf_puts(b, "&gt;"); break;
        case '"': md_buf_puts(b, "&quot;"); break;
        default: md_buf_putc(b, s[i]); break;
        }
    }
}

static void esc_href(MdBuf *b, const char *s, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t i;

    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];

        if (c == '&') {
            md_buf_puts(b, "&amp;");
        } else if (c == '\'') {
            md_buf_puts(b, "&#x27;");
        } else if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || strchr("-_.+!*(),%#@?=;:/$~", c) != NULL) {
            md_buf_putc(b, (char)c);
        } else {
            md_buf_putc(b, '%');
            md_buf_putc(b, hex[c >> 4]);
            md_buf_putc(b, hex[c & 15]);
        }
    }
}

static void cr(MdBuf *b)
{
    if (b->len > 0 && b->p[b->len - 1] != '\n') {
        md_buf_putc(b, '\n');
    }
}

static void alt_text(MdBuf *b, const MdNode *n)
{
    const MdNode *c;

    for (c = n->first_child; c != NULL; c = c->next) {
        if (c->type == MD_TEXT || c->type == MD_CODE || c->type == MD_HTML_INLINE) {
            esc_html(b, c->text, c->text_len);
        } else if (c->type == MD_SOFTBREAK || c->type == MD_LINEBREAK) {
            md_buf_putc(b, ' ');
        } else {
            alt_text(b, c);
        }
    }
}

static void render(MdBuf *b, const MdNode *n);

static void render_children(MdBuf *b, const MdNode *n)
{
    const MdNode *c;

    for (c = n->first_child; c != NULL; c = c->next) {
        render(b, c);
    }
}

static void render(MdBuf *b, const MdNode *n)
{
    switch (n->type) {
    case MD_DOCUMENT:
        render_children(b, n);
        break;
    case MD_BLOCKQUOTE:
        cr(b);
        md_buf_puts(b, "<blockquote>\n");
        render_children(b, n);
        cr(b);
        md_buf_puts(b, "</blockquote>\n");
        break;
    case MD_LIST:
        cr(b);
        if (n->ordered) {
            if (n->start != 1) {
                md_buf_puts(b, "<ol start=\"");
                md_buf_putint(b, n->start);
                md_buf_puts(b, "\">\n");
            } else {
                md_buf_puts(b, "<ol>\n");
            }
        } else {
            md_buf_puts(b, "<ul>\n");
        }
        render_children(b, n);
        cr(b);
        md_buf_puts(b, n->ordered ? "</ol>\n" : "</ul>\n");
        break;
    case MD_ITEM:
        cr(b);
        md_buf_puts(b, "<li>");
        if (n->checked >= 0) {
            md_buf_puts(b, n->checked ? "<input type=\"checkbox\" checked=\"\" disabled=\"\" /> " : "<input type=\"checkbox\" disabled=\"\" /> ");
        }
        render_children(b, n);
        md_buf_puts(b, "</li>\n");
        break;
    case MD_CODE_BLOCK: {
        size_t w = 0;

        cr(b);
        md_buf_puts(b, "<pre><code");
        while (n->info[w] != '\0' && n->info[w] != ' ') {
            w++;
        }
        if (w > 0) {
            md_buf_puts(b, " class=\"language-");
            esc_html(b, n->info, w);
            md_buf_putc(b, '"');
        }
        md_buf_putc(b, '>');
        esc_html(b, n->text, n->text_len);
        md_buf_puts(b, "</code></pre>\n");
        break;
    }
    case MD_HTML_BLOCK:
        cr(b);
        md_buf_put(b, n->text, n->text_len);
        cr(b);
        break;
    case MD_PARAGRAPH: {
        const MdNode *gp = n->parent ? n->parent->parent : NULL;
        int tight = gp != NULL && gp->type == MD_LIST && gp->tight;

        if (!tight) {
            cr(b);
            md_buf_puts(b, "<p>");
        }
        render_children(b, n);
        if (!tight) {
            md_buf_puts(b, "</p>\n");
        }
        break;
    }
    case MD_HEADING:
        cr(b);
        md_buf_puts(b, "<h");
        md_buf_putint(b, n->level);
        md_buf_putc(b, '>');
        render_children(b, n);
        md_buf_puts(b, "</h");
        md_buf_putint(b, n->level);
        md_buf_puts(b, ">\n");
        break;
    case MD_THEMATIC_BREAK:
        cr(b);
        md_buf_puts(b, "<hr />\n");
        break;
    case MD_TABLE: {
        const MdNode *row;
        int body = 0;

        cr(b);
        md_buf_puts(b, "<table>\n");
        for (row = n->first_child; row != NULL; row = row->next) {
            if (row->align) {
                md_buf_puts(b, "<thead>\n");
            } else if (!body) {
                md_buf_puts(b, "<tbody>\n");
                body = 1;
            }
            render(b, row);
            if (row->align) {
                md_buf_puts(b, "</thead>\n");
            }
        }
        if (body) {
            md_buf_puts(b, "</tbody>\n");
        }
        md_buf_puts(b, "</table>\n");
        break;
    }
    case MD_TABLE_ROW:
        md_buf_puts(b, "<tr>\n");
        render_children(b, n);
        md_buf_puts(b, "</tr>\n");
        break;
    case MD_TABLE_CELL:
        md_buf_puts(b, n->level ? "<th" : "<td");
        if (n->align == 1) {
            md_buf_puts(b, " align=\"left\"");
        } else if (n->align == 2) {
            md_buf_puts(b, " align=\"center\"");
        } else if (n->align == 3) {
            md_buf_puts(b, " align=\"right\"");
        }
        md_buf_putc(b, '>');
        render_children(b, n);
        md_buf_puts(b, n->level ? "</th>\n" : "</td>\n");
        break;
    case MD_TEXT:
        esc_html(b, n->text, n->text_len);
        break;
    case MD_SOFTBREAK:
        md_buf_putc(b, '\n');
        break;
    case MD_LINEBREAK:
        md_buf_puts(b, "<br />\n");
        break;
    case MD_CODE:
        md_buf_puts(b, "<code>");
        esc_html(b, n->text, n->text_len);
        md_buf_puts(b, "</code>");
        break;
    case MD_HTML_INLINE:
        md_buf_put(b, n->text, n->text_len);
        break;
    case MD_EMPH:
        md_buf_puts(b, "<em>");
        render_children(b, n);
        md_buf_puts(b, "</em>");
        break;
    case MD_STRONG:
        md_buf_puts(b, "<strong>");
        render_children(b, n);
        md_buf_puts(b, "</strong>");
        break;
    case MD_STRIKE:
        md_buf_puts(b, "<del>");
        render_children(b, n);
        md_buf_puts(b, "</del>");
        break;
    case MD_LINK:
        md_buf_puts(b, "<a href=\"");
        esc_href(b, n->url, strlen(n->url));
        md_buf_putc(b, '"');
        if (n->title[0] != '\0') {
            md_buf_puts(b, " title=\"");
            esc_html(b, n->title, strlen(n->title));
            md_buf_putc(b, '"');
        }
        md_buf_putc(b, '>');
        render_children(b, n);
        md_buf_puts(b, "</a>");
        break;
    case MD_IMAGE:
        md_buf_puts(b, "<img src=\"");
        esc_href(b, n->url, strlen(n->url));
        md_buf_puts(b, "\" alt=\"");
        alt_text(b, n);
        md_buf_putc(b, '"');
        if (n->title[0] != '\0') {
            md_buf_puts(b, " title=\"");
            esc_html(b, n->title, strlen(n->title));
            md_buf_putc(b, '"');
        }
        md_buf_puts(b, " />");
        break;
    }
}

char *md_to_html(const MdDoc *doc, size_t *len)
{
    MdBuf b;

    md_buf_init(&b);
    render(&b, doc->root);
    return md_buf_finish(&b, len);
}
