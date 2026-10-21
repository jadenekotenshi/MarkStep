/*
 * md2rtf -- turns Markdown into RTF, for viewing and editing in Edit.app, OpenWrite and anything else that reads RTF.
 *
 *   md2rtf [options] [input.md]
 *
 * With a file name the output goes to the same name ending in .rtf (or where -o says); with no file name, standard input
 * is read and standard output written. A file name of - means standard input / output.
 *
 *   -o FILE        write FILE ( - for standard output)
 *   -f FONT        body font (default Helvetica)         -m FONT   monospaced font (default Courier)
 *   -s POINTS      body size (default 12)                -z POINTS code size (default 10)
 *   -l text|url|both   what a link shows (default both: "text (address)")
 *   -H drop|text   raw HTML: leave it out (default) or show its source
 *   -c cp1252|nextstep|ascii   how characters beyond ASCII are written (default cp1252, which is what RTF specifies;
 *                  nextstep if a NeXT application shows accents wrongly)
 *   -V             print the version     -h   this text
 *
 * Options are parsed by hand: OPENSTEP's libc getopt is declared but not always linkable.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md.h"

#define VERSION "0.1.0"

static void usage(FILE *f)
{
    fputs("usage: md2rtf [-o out.rtf] [-f font] [-m monofont] [-s points] [-z points] [-l text|url|both]\n"
          "              [-H drop|text] [-c cp1252|nextstep|ascii] [input.md]\n"
          "       md2rtf -V | -h\n", f);
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
    char *out = (char *)malloc(n + 5);

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
    strcpy(out + dot, ".rtf");
    return out;
}

int main(int argc, char **argv)
{
    MdRtfOptions o;
    const char *in = NULL, *outname = NULL;
    char *outdefault = NULL, *src, *rtf;
    size_t len = 0, rlen = 0;
    FILE *fin, *fout;
    MdDoc *doc;
    int i;

    md_rtf_defaults(&o);
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
                printf("md2rtf %s\n", VERSION);
                return 0;
            }
            if (strchr("ofmszlHc", opt) == NULL) {
                fprintf(stderr, "md2rtf: unknown option %s\n", a);
                usage(stderr);
                return 2;
            }
            if (a[2] != '\0') {
                v = a + 2;
            } else if (i + 1 < argc) {
                v = argv[++i];
            } else {
                fprintf(stderr, "md2rtf: option -%c needs a value\n", opt);
                return 2;
            }
            switch (opt) {
            case 'o': outname = v; break;
            case 'f': o.body_font = v; break;
            case 'm': o.mono_font = v; break;
            case 's': o.body_pt = atoi(v); break;
            case 'z': o.mono_pt = atoi(v); break;
            case 'l':
                if (strcmp(v, "text") == 0) o.links = MD_LINKS_TEXT;
                else if (strcmp(v, "url") == 0) o.links = MD_LINKS_URL;
                else if (strcmp(v, "both") == 0) o.links = MD_LINKS_BOTH;
                else { fprintf(stderr, "md2rtf: -l takes text, url or both\n"); return 2; }
                break;
            case 'H':
                if (strcmp(v, "drop") == 0) o.html = MD_HTML_DROP;
                else if (strcmp(v, "text") == 0) o.html = MD_HTML_TEXT;
                else { fprintf(stderr, "md2rtf: -H takes drop or text\n"); return 2; }
                break;
            case 'c':
                if (strcmp(v, "cp1252") == 0) o.charset = MD_CHARSET_CP1252;
                else if (strcmp(v, "nextstep") == 0) o.charset = MD_CHARSET_NEXTSTEP;
                else if (strcmp(v, "ascii") == 0) o.charset = MD_CHARSET_ASCII;
                else { fprintf(stderr, "md2rtf: -c takes cp1252, nextstep or ascii\n"); return 2; }
                break;
            }
        } else if (in == NULL) {
            in = a;
        } else {
            fprintf(stderr, "md2rtf: only one input file\n");
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
            fprintf(stderr, "md2rtf: cannot open %s\n", in);
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
        fprintf(stderr, "md2rtf: out of memory\n");
        return 1;
    }
    doc = md_parse(src, len);
    free(src);
    if (doc == NULL) {
        fprintf(stderr, "md2rtf: out of memory\n");
        return 1;
    }
    rtf = md_to_rtf(doc, &o, &rlen);
    md_free(doc);
    if (rtf == NULL) {
        fprintf(stderr, "md2rtf: out of memory\n");
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
            fprintf(stderr, "md2rtf: cannot write %s\n", outname);
            free(rtf);
            free(outdefault);
            return 1;
        }
    }
    if (fwrite(rtf, 1, rlen, fout) != rlen) {
        fprintf(stderr, "md2rtf: write failed\n");
        return 1;
    }
    if (fout != stdout && fclose(fout) != 0) {
        fprintf(stderr, "md2rtf: write failed\n");
        return 1;
    }
    free(rtf);
    free(outdefault);
    return 0;
}
