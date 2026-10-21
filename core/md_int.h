/* md_int.h -- what the pieces of the Markdown parser share. Not for users of md.h. */
#ifndef MD_INT_H
#define MD_INT_H

#include "md.h"

typedef struct MdChunk MdChunk;

typedef struct {
    char *label;                 /* normalised: case folded, white space collapsed (malloc'd) */
    const char *url;             /* in the arena */
    const char *title;
} MdRef;

struct MdDoc {
    MdChunk *chunks;
    MdNode *root;
    MdRef *refs;
    int nrefs, refcap;
    int oom;
};

/* ---- arena and tree ---- */
void *md_alloc(MdDoc *d, size_t n);
char *md_strndup(MdDoc *d, const char *s, size_t n);
MdNode *md_new_node(MdDoc *d, MdType t);
void md_append_child(MdNode *parent, MdNode *child);
void md_prepend_child(MdNode *parent, MdNode *child);
void md_insert_after(MdNode *ref, MdNode *n);
void md_insert_before(MdNode *ref, MdNode *n);
void md_unlink(MdNode *n);
void md_node_free_content(MdNode *n);                  /* releases the malloc'd line buffer */
void md_node_add_line(MdNode *n, const char *s, size_t len);   /* appends s and a newline */

/* ---- growable string ---- */
typedef struct { char *p; size_t len, cap; int fail; } MdBuf;
void md_buf_init(MdBuf *b);
void md_buf_put(MdBuf *b, const char *s, size_t n);
void md_buf_putc(MdBuf *b, int c);
void md_buf_puts(MdBuf *b, const char *s);
void md_buf_putint(MdBuf *b, long v);
char *md_buf_finish(MdBuf *b, size_t *len);            /* NUL-terminated, malloc'd; the buffer is empty afterwards */
void md_buf_free(MdBuf *b);

/* ---- characters ---- */
int md_utf8_encode(unsigned long cp, char out[4]);
unsigned long md_utf8_decode(const char *s, size_t n, size_t *adv);    /* U+FFFD for bad bytes; *adv >= 1 */
int md_is_ascii_punct(int c);
int md_is_unicode_space(unsigned long cp);
int md_is_unicode_punct(unsigned long cp);
unsigned long md_entity_lookup(const char *name, size_t len);          /* 0 if unknown */
void md_normalize_label(const char *s, size_t n, MdBuf *out);
int md_strncasecmp(const char *a, const char *b, size_t n);

/* ---- the two parsing passes ---- */
void md_parse_blocks(MdDoc *d, const char *src, size_t len);
void md_parse_inlines(MdDoc *d);
void md_add_ref(MdDoc *d, const char *label, size_t llen, const char *url, size_t ulen, const char *title, size_t tlen);
const MdRef *md_find_ref(const MdDoc *d, const char *label, size_t llen);
/* link reference definitions at the start of a paragraph's text: how many bytes they take (0 if none) */
size_t md_scan_ref_defs(MdDoc *d, const char *s, size_t len);

/* shared scanning helpers (md_inline.c) */
size_t md_scan_html_tag(const char *s, size_t n, size_t i);       /* an open or closing tag at s[i]: the end, or 0 */
size_t md_scan_link_dest(const char *s, size_t n, size_t i, const char **dest, size_t *dlen, int *angle);
size_t md_scan_link_title(const char *s, size_t n, size_t i, const char **title, size_t *tlen);
void md_unescape_into(MdBuf *out, const char *s, size_t n, int entities);

#endif
