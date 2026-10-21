/*
 * rtf_in.h -- RTF to Markdown, lossy by nature.
 *
 * Reads the RTF a word processor writes (OpenWrite, Edit.app, WordPad, Word, TextEdit ... and this project's own md2rtf)
 * and gives back Markdown: headings from styles or from large bold text, bold, italic and strikethrough, monospaced runs
 * as code, bulleted and numbered lists with their nesting, block quotes, tables, hyperlinks, rules. What Markdown has no word
 * for (fonts, colours, margins, alignment) is dropped. Pure C89, no I/O.
 */
#ifndef RTF_IN_H
#define RTF_IN_H

#include <stddef.h>

enum {
    RTF_CHARSET_AUTO = 0,        /* NeXTSTEP's own for "{\rtf0" (what NeXT's RTF writer starts with), Windows-1252 otherwise */
    RTF_CHARSET_CP1252 = 1,
    RTF_CHARSET_NEXTSTEP = 2
};

typedef struct {
    int charset;                 /* RTF_CHARSET_*: how \'hh bytes (and plain bytes above 127) are read */
    int quotes;                  /* 1 (default): indented grey or italic paragraphs and "Quote" styles become block quotes; 0: never */
} RtfMdOptions;

void rtf_md_defaults(RtfMdOptions *o);

/* Returns the Markdown as a malloc'd NUL-terminated UTF-8 string (free() it), or NULL if out of memory. */
char *rtf_to_markdown(const char *rtf, size_t len, const RtfMdOptions *opt, size_t *outlen);

#endif
