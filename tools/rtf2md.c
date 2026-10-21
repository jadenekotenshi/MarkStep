/*
 * rtf2md -- turns RTF (from OpenWrite, Edit.app, WordPad, Word ...) into Markdown. Lossy by nature: headings, emphasis,
 * lists, quotes, code, tables and links come through; fonts, colours, margins and alignment do not.
 *
 *   rtf2md [options] [input.rtf]
 *
 * With a file name the output goes to the same name ending in .md (or where -o says); with no file name, standard input is
 * read and standard output written. A file name of - means standard input / output.
 *
 *   -o FILE        write FILE ( - for standard output)
 *   -c auto|cp1252|nextstep   how \'hh bytes are read (default auto: NeXTSTEP's own for files starting "{\rtf0", which is
 *                  what NeXT's RTF writer produces, Windows-1252 otherwise)
 *   -q on|off      turn indented grey or italic paragraphs into block quotes (default on)
 *   -V             print the version     -h   this text
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rtf_in.h"

#define VERSION "0.1.0"

static void usage(FILE *f)
{
    fputs("usage: rtf2md [-o out.md] [-c auto|cp1252|nextstep] [-q on|off] [input.rtf]\n"
          "       rtf2md -V | -h\n", f);
}

static char *read_all(FILE *f, size_t *len)
{
    size_t cap = 65536, n = 0;
    char *p = (char *)malloc(cap), *q;

    if (p == NULL) {
        return NULL;
    }
    for (;;) {
        size_t got = fread(p + n, 1, cap - n, f);

        n += got;
        if (n < cap) {
            break;
        }
        cap *= 2;
        q = (char *)realloc(p, cap);
        if (q == NULL) {
            free(p);
            return NULL;
        }
        p = q;
    }
    *len = n;
    return p;
}

static char *default_output(const char *in)
{
    size_t n = strlen(in), dot = n, i;
    char *out = (char *)malloc(n + 4);

    if (out == NULL) {
        return NULL;
    }
    for (i = 0; i < n; i++) {
        if (in[i] == '.') {
            dot = i;
        } else if (in[i] == '/') {
            dot = n;
        }
    }
    memcpy(out, in, dot);
    strcpy(out + dot, ".md");
    return out;
}

int main(int argc, char **argv)
{
    RtfMdOptions o;
    const char *in = NULL, *outname = NULL;
    char *outdefault = NULL, *src, *md;
    size_t len = 0, mlen = 0;
    FILE *fin, *fout;
    int i;

    rtf_md_defaults(&o);
    for (i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = NULL;

        if (a[0] == '-' && a[1] != '\0') {
            char opt = a[1];

            if (opt == 'h') {
                usage(stdout);
                return 0;
            }
            if (opt == 'V') {
                printf("rtf2md %s\n", VERSION);
                return 0;
            }
            if (strchr("ocq", opt) == NULL) {
                fprintf(stderr, "rtf2md: unknown option %s\n", a);
                usage(stderr);
                return 2;
            }
            if (a[2] != '\0') {
                v = a + 2;
            } else if (i + 1 < argc) {
                v = argv[++i];
            } else {
                fprintf(stderr, "rtf2md: option -%c needs a value\n", opt);
                return 2;
            }
            if (opt == 'o') {
                outname = v;
            } else if (opt == 'c') {
                if (strcmp(v, "auto") == 0) o.charset = RTF_CHARSET_AUTO;
                else if (strcmp(v, "cp1252") == 0) o.charset = RTF_CHARSET_CP1252;
                else if (strcmp(v, "nextstep") == 0) o.charset = RTF_CHARSET_NEXTSTEP;
                else { fprintf(stderr, "rtf2md: -c takes auto, cp1252 or nextstep\n"); return 2; }
            } else {
                if (strcmp(v, "on") == 0) o.quotes = 1;
                else if (strcmp(v, "off") == 0) o.quotes = 0;
                else { fprintf(stderr, "rtf2md: -q takes on or off\n"); return 2; }
            }
        } else if (in == NULL) {
            in = a;
        } else {
            fprintf(stderr, "rtf2md: only one input file\n");
            usage(stderr);
            return 2;
        }
    }
    if (in != NULL && strcmp(in, "-") == 0) {
        in = NULL;
    }
    if (in != NULL) {
        fin = fopen(in, "rb");
        if (fin == NULL) {
            fprintf(stderr, "rtf2md: cannot open %s\n", in);
            return 1;
        }
    } else {
        fin = stdin;
    }
    src = read_all(fin, &len);
    if (fin != stdin) {
        fclose(fin);
    }
    if (src == NULL) {
        fprintf(stderr, "rtf2md: out of memory\n");
        return 1;
    }
    md = rtf_to_markdown(src, len, &o, &mlen);
    free(src);
    if (md == NULL) {
        fprintf(stderr, "rtf2md: out of memory\n");
        return 1;
    }
    if (outname == NULL && in != NULL) {
        outdefault = default_output(in);
        outname = outdefault;
    }
    if (outname == NULL || strcmp(outname, "-") == 0) {
        fout = stdout;
    } else {
        fout = fopen(outname, "wb");
        if (fout == NULL) {
            fprintf(stderr, "rtf2md: cannot write %s\n", outname);
            free(md);
            free(outdefault);
            return 1;
        }
    }
    if (fwrite(md, 1, mlen, fout) != mlen) {
        fprintf(stderr, "rtf2md: write failed\n");
        return 1;
    }
    if (fout != stdout && fclose(fout) != 0) {
        fprintf(stderr, "rtf2md: write failed\n");
        return 1;
    }
    free(md);
    free(outdefault);
    return 0;
}
