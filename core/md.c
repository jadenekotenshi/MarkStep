/* md.c -- md_parse(): the two passes, block structure then inline structure. */
#include <stdlib.h>
#include <string.h>
#include "md_int.h"

#define MAX_TREE_DEPTH 200

/* Emphasis and the like can nest as deep as the text is long; the renderers recurse, so anything deeper than
 * MAX_TREE_DEPTH is lifted out to sit after its parent. Iterative, so that it needs no stack of its own. */
static void flatten_deep(MdNode *root)
{
    MdNode *n = root->first_child;
    int depth = 1;

    while (n != NULL) {
        if (depth >= MAX_TREE_DEPTH && n->first_child != NULL) {
            MdNode *c = n->first_child, *last = n;

            n->first_child = n->last_child = NULL;
            while (c != NULL) {
                MdNode *next = c->next;

                c->prev = c->next = NULL;
                c->parent = NULL;
                md_insert_after(last, c);
                last = c;
                c = next;
            }
        }
        if (n->first_child != NULL) {
            n = n->first_child;
            depth++;
            continue;
        }
        while (n != NULL && n->next == NULL) {
            n = n->parent;
            depth--;
            if (n == root || n == NULL) {
                n = NULL;
                break;
            }
        }
        if (n != NULL) {
            n = n->next;
        }
    }
}

MdDoc *md_parse(const char *src, size_t len)
{
    MdDoc *d = (MdDoc *)calloc(1, sizeof(MdDoc));

    if (d == NULL) {
        return NULL;
    }
    d->root = md_new_node(d, MD_DOCUMENT);
    if (d->root == NULL) {
        md_free(d);
        return NULL;
    }
    md_parse_blocks(d, src != NULL ? src : "", src != NULL ? len : 0);
    md_parse_inlines(d);
    flatten_deep(d->root);
    if (d->oom) {
        md_free(d);
        return NULL;
    }
    return d;
}
