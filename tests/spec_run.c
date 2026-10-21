/*
 * spec_run.c -- runs the CommonMark specification's examples (tests/spec_cases.txt, made from spec.json 0.31.2 by
 * tools/gen_spec.py; the examples are (c) John MacFarlane and others, CC BY-SA 4.0) through the parser and the HTML
 * renderer, and compares with the expected HTML.
 *
 * usage: spec_run CASES KNOWN_FAILURES [-v N]
 *   - an example listed in KNOWN_FAILURES may fail (these are the things this parser deliberately does not do);
 *     a listed example that passes, or an unlisted one that fails, makes the run fail
 *   - -v N shows example N's input, expected and actual output
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../core/md.h"

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *p;
    long n;

    if (f == NULL) {
        return NULL;
    }
    fseek(f, 0L, SEEK_END);
    n = ftell(f);
    fseek(f, 0L, SEEK_SET);
    p = (char *)malloc((size_t)n + 1);
    if (p == NULL || fread(p, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(p);
        return NULL;
    }
    fclose(f);
    p[n] = '\0';
    *len = (size_t)n;
    return p;
}

static int known(const char *list, int ex)
{
    const char *p = list;

    while (p != NULL && *p != '\0') {
        char *end;
        long v = strtol(p, &end, 10);

        if (end == p) {
            p = strchr(p, '\n');
            if (p != NULL) {
                p++;
            }
            continue;
        }
        if (v == ex) {
            return 1;
        }
        p = end;
        while (*p == ' ' || *p == '\n') {
            p++;
        }
    }
    return 0;
}

int main(int argc, char **argv)
{
    size_t n, kn, pos = 0;
    char *data, *kl;
    int verbose = argc >= 4 && strcmp(argv[3], "-v") == 0 ? atoi(argv[4]) : 0;
    int passed = 0, failed = 0, unexpected = 0, fixed = 0;

    if (argc < 3 || (data = slurp(argv[1], &n)) == NULL) {
        fprintf(stderr, "usage: spec_run CASES KNOWN_FAILURES [-v N]\n");
        return 2;
    }
    kl = slurp(argv[2], &kn);
    while (pos < n) {
        int ex;
        unsigned long mdl, htl;
        char sec[80];
        MdDoc *d;
        char *html;
        size_t hl = 0;
        int ok;
        char *nl = memchr(data + pos, '\n', n - pos);

        if (nl == NULL || sscanf(data + pos, "%d %lu %lu %79s", &ex, &mdl, &htl, sec) != 4) {
            break;
        }
        pos = (size_t)(nl - data) + 1;
        d = md_parse(data + pos, mdl);
        html = d ? md_to_html(d, &hl) : NULL;
        ok = html != NULL && hl == htl && memcmp(html, data + pos + mdl, htl) == 0;
        if (verbose == ex) {
            printf("--- example %d (%s)\n%.*s--- expected\n%.*s--- got\n%s\n", ex, sec, (int)mdl, data + pos, (int)htl, data + pos + mdl, html ? html : "(null)");
        }
        if (ok) {
            passed++;
            if (kl && known(kl, ex)) {
                fixed++;
                printf("now passes (remove from the known failures): %d (%s)\n", ex, sec);
            }
        } else {
            failed++;
            if (!kl || !known(kl, ex)) {
                unexpected++;
                printf("FAIL example %d (%s)\n", ex, sec);
            }
        }
        free(html);
        md_free(d);
        pos += mdl + htl + 1;
    }
    printf("spec: %d passed, %d failed (%d unexpected), %d known failures now pass\n", passed, failed, unexpected, fixed);
    free(data);
    free(kl);
    return (unexpected || fixed) ? 1 : 0;
}
