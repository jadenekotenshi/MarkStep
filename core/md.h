/*
 * md.h -- a Markdown parser (CommonMark, plus GFM tables, strikethrough and task lists) and renderers for HTML and RTF.
 *
 * Pure C89, no I/O.  md_parse() turns UTF-8 text into a tree of MdNode; md_to_rtf() and md_to_html() turn the tree
 * into text for a viewer.  Everything the tree points at lives in the MdDoc and goes away with md_free().
 *
 * Divergences from CommonMark, all deliberate: tabs are expanded to 4-column stops before parsing (so a tab inside a
 * paragraph or code block comes out as spaces), only a few dozen of the named character references are known (numeric
 * ones all are), and Unicode case folding of link labels is ASCII plus Latin-1.
 */
#ifndef MD_H
#define MD_H

#include <stddef.h>

typedef enum {
    MD_DOCUMENT = 0,
    /* blocks */
    MD_BLOCKQUOTE, MD_LIST, MD_ITEM, MD_CODE_BLOCK, MD_HTML_BLOCK, MD_PARAGRAPH, MD_HEADING, MD_THEMATIC_BREAK,
    MD_TABLE, MD_TABLE_ROW, MD_TABLE_CELL,
    /* inlines */
    MD_TEXT, MD_SOFTBREAK, MD_LINEBREAK, MD_CODE, MD_HTML_INLINE, MD_EMPH, MD_STRONG, MD_STRIKE, MD_LINK, MD_IMAGE
} MdType;

typedef struct MdNode MdNode;
struct MdNode {
    MdType type;
    MdNode *parent, *first_child, *last_child, *prev, *next;

    const char *text;            /* TEXT, CODE, CODE_BLOCK, HTML_BLOCK, HTML_INLINE: the literal (UTF-8; use text_len, not the NUL) */
    size_t text_len;
    const char *info;            /* CODE_BLOCK: the info string of a fenced block ("" if none) */
    const char *url, *title;     /* LINK, IMAGE ("" if none) */
    int level;                   /* HEADING: 1..6 */
    int ordered;                 /* LIST: 1 for a numbered list */
    int start;                   /* LIST: the first number */
    int tight;                   /* LIST: no blank lines between its items or their blocks */
    char delim;                  /* LIST: the bullet character, or '.' / ')' after the number */
    int checked;                 /* ITEM: -1 an ordinary item, 0 an open task "[ ]", 1 a done task "[x]" */
    int align;                   /* TABLE_CELL: 0 none, 1 left, 2 center, 3 right; TABLE_ROW: 1 for the header row */

    /* parser use */
    int open;
    int last_line_blank;
    int start_line;
    int fenced;                  /* CODE_BLOCK */
    int fence_char, fence_len, fence_offset;
    int marker_offset, padding;  /* ITEM */
    char *content;               /* accumulated lines of an open leaf block */
    size_t content_len, content_cap;
};

typedef struct MdDoc MdDoc;

/* Parses len bytes of UTF-8 (a NUL byte becomes U+FFFD). Returns NULL only if out of memory. */
MdDoc *md_parse(const char *src, size_t len);
MdNode *md_root(const MdDoc *doc);
void md_free(MdDoc *doc);

/* The document as HTML, following CommonMark's reference output closely enough for its test suite (used by the tests).
 * Returns a malloc'd NUL-terminated string (free() it) or NULL; *len gets its length if len is not NULL. */
char *md_to_html(const MdDoc *doc, size_t *len);

/* ---- RTF ---- */

enum { MD_LINKS_TEXT = 0, MD_LINKS_URL = 1, MD_LINKS_BOTH = 2 };    /* what a link shows: its text, its address, or "text (address)" */
enum { MD_HTML_DROP = 0, MD_HTML_TEXT = 1 };                         /* raw HTML: leave it out, or show its source as it is */

enum { MD_CHARSET_CP1252 = 0, MD_CHARSET_NEXTSTEP = 1, MD_CHARSET_ASCII = 2 };   /* Windows-1252 (what RTF says), NeXTSTEP's own, or \uN only */

typedef struct {
    const char *body_font;       /* RTF font name for text ("Helvetica") */
    const char *mono_font;       /* for code ("Courier") */
    int body_pt;                 /* point size of body text (12) */
    int mono_pt;                 /* of code (10) */
    int links;                   /* MD_LINKS_* (default: MD_LINKS_BOTH: "text (address)", or just the text when they are the same) */
    int html;                    /* MD_HTML_* (default: MD_HTML_DROP) */
    int charset;                 /* MD_CHARSET_*: how characters beyond ASCII are written as \'hh (default: MD_CHARSET_CP1252) */
} MdRtfOptions;

void md_rtf_defaults(MdRtfOptions *o);
/* The document as RTF (\rtf1\ansi; characters beyond ASCII as \'hh in the chosen charset where it has them, else \uN and a
 * look-alike ASCII fallback). Returns a malloc'd NUL-terminated string. */
char *md_to_rtf(const MdDoc *doc, const MdRtfOptions *opt, size_t *len);

#endif
