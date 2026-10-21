/* test_md.c -- what the CommonMark examples do not cover: GFM extensions, the RTF writer and its options, charsets,
 * limits, and a fuzz run over random Markdown-ish input. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../core/md.h"
#include "../core/nsenc.h"
#include "test.h"

static char *html(const char *md)
{
    MdDoc *d = md_parse(md, strlen(md));
    char *h = d ? md_to_html(d, NULL) : NULL;

    md_free(d);
    return h;
}

static char *rtf_with(const char *md, const MdRtfOptions *o)
{
    MdDoc *d = md_parse(md, strlen(md));
    char *r = d ? md_to_rtf(d, o, NULL) : NULL;

    md_free(d);
    return r;
}

static char *rtf(const char *md)
{
    MdRtfOptions o;

    md_rtf_defaults(&o);
    return rtf_with(md, &o);
}

static int html_is(const char *md, const char *want)
{
    char *h = html(md);
    int ok = h != NULL && strcmp(h, want) == 0;

    if (!ok) {
        fprintf(stderr, "    markdown: %s\n    wanted:   %s\n    got:      %s\n", md, want, h ? h : "(null)");
    }
    free(h);
    return ok;
}

static int rtf_has(const char *md, const char *needle)
{
    char *r = rtf(md);
    int ok = r != NULL && strstr(r, needle) != NULL;

    if (!ok) {
        fprintf(stderr, "    markdown: %s\n    missing:  %s\n    got:      %s\n", md, needle, r ? r : "(null)");
    }
    free(r);
    return ok;
}

/* every { has its }, ignoring the escaped ones */
static int braces_balanced(const char *s)
{
    int depth = 0;

    for (; *s; s++) {
        if (*s == '\\' && s[1] != '\0') {
            s++;
        } else if (*s == '{') {
            depth++;
        } else if (*s == '}') {
            if (--depth < 0) {
                return 0;
            }
        }
    }
    return depth == 0;
}

static void test_gfm(void)
{
    CHECK(html_is("| a | b |\n|---|:-:|\n| 1 | 2 |\n",
                  "<table>\n<thead>\n<tr>\n<th>a</th>\n<th align=\"center\">b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n"
                  "<td align=\"center\">2</td>\n</tr>\n</tbody>\n</table>\n"));
    CHECK(html_is("a|b\n-|-\n1|2\n", "<table>\n<thead>\n<tr>\n<th>a</th>\n<th>b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n<td>2</td>\n</tr>\n</tbody>\n</table>\n"));
    /* short rows are padded, long ones cut; alignments */
    CHECK(html_is("| a | b |\n|:--|--:|\n| 1 |\n| 1 | 2 | 3 |\n",
                  "<table>\n<thead>\n<tr>\n<th align=\"left\">a</th>\n<th align=\"right\">b</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n"
                  "<td align=\"left\">1</td>\n<td align=\"right\"></td>\n</tr>\n<tr>\n<td align=\"left\">1</td>\n<td align=\"right\">2</td>\n</tr>\n</tbody>\n</table>\n"));
    /* an escaped pipe stays in its cell; inline markup works in cells */
    CHECK(html_is("| a |\n|---|\n| x \\| y *z* |\n", "<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>x | y <em>z</em></td>\n</tr>\n</tbody>\n</table>\n"));
    /* a header and delimiter row of different widths is no table; a blank line ends one */
    CHECK(html_is("| a | b |\n|---|\n", "<p>| a | b |\n|---|</p>\n"));
    CHECK(html_is("| a |\n|---|\n| 1 |\n\ntext\n", "<table>\n<thead>\n<tr>\n<th>a</th>\n</tr>\n</thead>\n<tbody>\n<tr>\n<td>1</td>\n</tr>\n</tbody>\n</table>\n<p>text</p>\n"));
    /* strikethrough needs a pair of tildes; task lists */
    CHECK(html_is("~~gone~~ ~one~ ~~~three~~~\n", "<p><del>gone</del> ~one~ ~~~three~~~</p>\n"));
    CHECK(html_is("- [ ] a\n- [x] b\n- [X] c\n- [] d\n- [ ]e\n",
                  "<ul>\n<li><input type=\"checkbox\" disabled=\"\" /> a</li>\n<li><input type=\"checkbox\" checked=\"\" disabled=\"\" /> b</li>\n"
                  "<li><input type=\"checkbox\" checked=\"\" disabled=\"\" /> c</li>\n<li>[] d</li>\n<li>[ ]e</li>\n</ul>\n"));
}

static void test_inlines(void)
{
    CHECK(html_is("[Foo Bar]\n\n[foo   bar]: /url \"t\"\n", "<p><a href=\"/url\" title=\"t\">Foo Bar</a></p>\n"));
    CHECK(html_is("![alt *x*][img]\n\n[img]: a.png\n", "<p><img src=\"a.png\" alt=\"alt x\" /></p>\n"));
    CHECK(html_is("<http://a.b/c> <me@x.org>\n", "<p><a href=\"http://a.b/c\">http://a.b/c</a> <a href=\"mailto:me@x.org\">me@x.org</a></p>\n"));
    CHECK(html_is("a <b>x</b> <!-- c --> d\n", "<p>a <b>x</b> <!-- c --> d</p>\n"));
    CHECK(html_is("&copy; &#65; &#x42; &bogus;\n", "<p>\xC2\xA9 A B &amp;bogus;</p>\n"));
    CHECK(html_is("a  \nb\\\nc\n", "<p>a<br />\nb<br />\nc</p>\n"));
}

static void test_rtf_basics(void)
{
    char *r = rtf("# Title\n\ntext\n");

    CHECK(r != NULL && strncmp(r, "{\\rtf1\\ansi", 11) == 0 && braces_balanced(r));
    CHECK(r != NULL && strstr(r, "{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\fmodern Courier;}}") != NULL);
    CHECK(r != NULL && strstr(r, "\\fs43") == NULL && strstr(r, "\\fs42\\li0\\fi0\\sb200\\sa100 \\b Title\\par") != NULL);     /* 12pt * 1.8, whole points */
    free(r);
    CHECK(rtf_has("a **b *c* d** `e`\n", "a {\\b b {\\i c} d} {\\f1\\fs20 e}"));
    CHECK(rtf_has("~~x~~\n", "{\\strike x}"));
    CHECK(rtf_has("back\\\\slash \\{ and \\} `{x}`\n", "back\\\\slash \\{ and \\} {\\f1\\fs20 \\{x\\}}"));
    CHECK(rtf_has("soft\nbreak  \nhard\n", "soft break\\line hard"));
    CHECK(rtf_has("> quote\n", "\\li360\\fi0\\sb0\\sa120\\cf3 quote\\par"));
    CHECK(rtf_has("```\na\n  b\n\n```\n", "\\f1\\fs20\\li240\\fi0\\sb0\\sa120 a\\line   b\\par"));
    CHECK(rtf_has("---\n", "\\qc \\'97"));
    CHECK(rtf_has("a\n", "\\pard\\plain\\f0\\fs24\\li0\\fi0\\sb0\\sa120 a\\par"));
    {
        char *e = rtf("");

        CHECK(e != NULL && braces_balanced(e) && strstr(e, "\\par") == NULL);
        free(e);
    }
}

static void test_rtf_lists(void)
{
    CHECK(rtf_has("- a\n- b\n", "\\li360\\fi-360\\sb0\\sa40 \\tx360 \\'95\\tab a\\par"));
    CHECK(rtf_has("- a\n- b\n", "\\sa120 \\tx360 \\'95\\tab b\\par"));                  /* the last item of a top-level list spaces out */
    CHECK(rtf_has("3. a\n4. b\n", "\\tx480 3.\\tab a\\par"));
    CHECK(rtf_has("3. a\n4. b\n", "4.\\tab b"));
    CHECK(rtf_has("1) a\n", "1)\\tab a"));
    CHECK(rtf_has("- a\n  - b\n    - c\n", "\\li720\\fi-360\\sb0\\sa40 \\tx720 \\'96\\tab b\\par"));        /* nested: en dash, deeper indent */
    CHECK(rtf_has("- a\n  - b\n    - c\n", "\\li1080\\fi-360\\sb0\\sa120 \\tx1080 \\'95\\tab c\\par"));
    CHECK(rtf_has("- [x] done\n- [ ] todo\n", "[x]\\tab done"));
    CHECK(rtf_has("- [x] done\n- [ ] todo\n", "[ ]\\tab todo"));
    CHECK(rtf_has("- a\n\n  para two\n", "\\li360\\fi0\\sb0\\sa120 para two\\par"));
    CHECK(rtf_has("- ```\n  code\n  ```\n", "\\tab \\par"));                              /* an item that starts with a block */
    CHECK(rtf_has("> - q\n", "\\li720\\fi-360\\sb0\\sa120\\cf3 \\tx720"));
}

static void test_rtf_charsets(void)
{
    MdRtfOptions o;
    char *r, want[32];

    CHECK(rtf_has("caf\xC3\xA9\n", "caf\\'e9"));
    CHECK(rtf_has("\xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x94 \xE2\x80\xA2 \xE2\x82\xAC\n", "\\'93q\\'94 \\'97 \\'95 \\'80"));
    CHECK(rtf_has("a\xC2\xA0" "b\n", "a\\~b"));
    CHECK(rtf_has("\xE2\x9C\x93 \xE2\x86\x92 \xE2\x80\xA6\n", "\\u10003v \\u8594> \\'85"));          /* no cp1252 check mark or arrow: \u and a look-alike */
    CHECK(rtf_has("\xF0\x9F\x98\x80\n", "\\u-10179?\\u-8704?"));                                 /* beyond the BMP: a surrogate pair */
    CHECK(rtf_has("\xE4\xB8\xAD\n", "\\u20013?"));
    CHECK(rtf_has("bad \xFF byte\n", "bad \\u-3?"));                                             /* U+FFFD */
    md_rtf_defaults(&o);
    o.charset = MD_CHARSET_ASCII;
    r = rtf_with("caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D\n", &o);
    CHECK(r != NULL && strstr(r, "caf\\u233? \\u8220\"q\\u8221\"") != NULL && strchr(r, '\'') == NULL);
    free(r);
    o.charset = MD_CHARSET_NEXTSTEP;
    r = rtf_with("caf\xC3\xA9\n", &o);
    sprintf(want, "caf\\'%02x", nsenc_encode(0xE9));
    CHECK(r != NULL && strstr(r, want) != NULL);
    free(r);
}

static void test_rtf_options(void)
{
    MdRtfOptions o;
    char *r;

    CHECK(rtf_has("[t](http://x.org/a)\n", "{\\ul\\cf2 t}{\\cf3  (http://x.org/a)}"));
    CHECK(rtf_has("<http://x.org>\n", "{\\ul\\cf2 http://x.org}\\par"));                          /* the same text and address: shown once */
    CHECK(rtf_has("<me@x.org>\n", "{\\ul\\cf2 me@x.org}\\par"));
    CHECK(rtf_has("![a logo](l.png)\n", "{\\i [image: a logo]}{\\cf3  (l.png)}"));
    md_rtf_defaults(&o);
    o.links = MD_LINKS_TEXT;
    r = rtf_with("[t](http://x.org/a) ![i](p.png)\n", &o);
    CHECK(r != NULL && strstr(r, "{\\ul\\cf2 t} {\\i [image: i]}") != NULL && strstr(r, "x.org") == NULL);
    free(r);
    o.links = MD_LINKS_URL;
    r = rtf_with("[t](http://x.org/a)\n", &o);
    CHECK(r != NULL && strstr(r, "{\\ul\\cf2 http://x.org/a}") != NULL && strstr(r, "t}") == NULL);
    free(r);
    md_rtf_defaults(&o);
    r = rtf_with("a <b>x</b><br>b<br/>c <!-- no -->\n\n<div>\nhtml\n</div>\n", &o);
    CHECK(r != NULL && strstr(r, "a x\\line b\\line c ") != NULL && strstr(r, "div") == NULL && strstr(r, "html") == NULL && strstr(r, "no") == NULL);
    free(r);
    o.html = MD_HTML_TEXT;
    r = rtf_with("a <b>x</b>\n\n<div>\nhtml\n</div>\n", &o);
    CHECK(r != NULL && strstr(r, "{\\f1\\fs20 <b>}x{\\f1\\fs20 </b>}") != NULL && strstr(r, "<div>\\line html\\line </div>") != NULL);
    free(r);
    md_rtf_defaults(&o);
    o.body_font = "Times-Roman";
    o.mono_font = "Monaco";
    o.body_pt = 14;
    o.mono_pt = 9;
    r = rtf_with("# h\n\ntext `c`\n", &o);
    CHECK(r != NULL && strstr(r, "{\\f0\\fswiss Times-Roman;}{\\f1\\fmodern Monaco;}") != NULL && strstr(r, "\\fs28\\li0") != NULL &&
          strstr(r, "{\\f1\\fs18 c}") != NULL && strstr(r, "\\fs50\\li0\\fi0\\sb200") != NULL);        /* h1 = 28 half-points * 1.8, whole points */
    free(r);
}

static void test_rtf_tables(void)
{
    CHECK(rtf_has("| Name | Qty |\n|:-----|----:|\n| ab | 7 |\n| c | 12345 |\n",
                  "| Name |   Qty |\\line |------|-------|\\line | ab   |     7 |\\line | c    | 12345 |"));
    CHECK(rtf_has("| a |\n|:-:|\n| xyz |\n", "|  a  |\\line |-----|\\line | xyz |"));
    CHECK(rtf_has("| \xC3\xA9 | b |\n|---|---|\n", "| \\'e9   | b   |"));                                    /* width counts characters, not bytes */
}

static void test_limits(void)
{
    size_t i, n = 20000;
    char *s = (char *)malloc(n * 4 + 16), *r;
    MdDoc *d;

    /* deep nesting is cut off, not recursed into */
    for (i = 0; i < n; i++) {
        s[i] = '>';
    }
    strcpy(s + n, " x\n");
    d = md_parse(s, strlen(s));
    CHECK(d != NULL);
    r = d ? md_to_rtf(d, NULL, NULL) : NULL;
    CHECK(r != NULL && braces_balanced(r));
    free(r);
    md_free(d);
    for (i = 0; i < n; i++) {
        s[2 * i] = '-';
        s[2 * i + 1] = ' ';
    }
    strcpy(s + 2 * n, "x\n");
    r = rtf(s);
    CHECK(r != NULL && braces_balanced(r));
    free(r);
    for (i = 0; i < n; i++) {
        s[i] = '*';
    }
    s[n] = 'a';
    for (i = 0; i < n; i++) {
        s[n + 1 + i] = '*';
    }
    s[2 * n + 1] = '\0';
    r = rtf(s);
    CHECK(r != NULL && braces_balanced(r));
    free(r);
    for (i = 0; i < n; i++) {
        s[i] = '[';
    }
    s[n] = 'x';
    for (i = 0; i < n; i++) {
        s[n + 1 + i] = ']';
    }
    s[2 * n + 1] = '\0';
    r = rtf(s);
    CHECK(r != NULL && braces_balanced(r));
    free(r);
    /* NUL bytes, bare CR and CRLF line ends, no final newline */
    {
        MdDoc *e = md_parse("a\0b\r\nc\rd", 8);

        r = e ? md_to_html(e, NULL) : NULL;
        CHECK(r != NULL && strcmp(r, "<p>a\xEF\xBF\xBD" "b\nc\nd</p>\n") == 0);
        free(r);
        md_free(e);
    }
    CHECK(md_parse(NULL, 0) != NULL);
    free(s);
}

/* random Markdown-ish text must never crash and must always give balanced RTF */
static unsigned long seed = 12345UL;

static unsigned long rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (seed >> 8) & 0xFFFFFFUL;
}

static void test_fuzz(void)
{
    static const char *const bits[] = {
        "# ", "## ", "- ", "* ", "1. ", "2) ", "> ", "    ", "\t", "```", "~~~", "---", "***", "===", "|", "|---|", ":-:", "[", "]", "(",
        ")", "![", "<", ">", "<b>", "</b>", "<!--", "-->", "`", "``", "*", "**", "_", "__", "~~", "\\", "&amp;", "&#x41;", "&", "\n", "\n\n",
        "  \n", " ", "word", "caf\xC3\xA9", "\xE2\x80\x9C", "\xF0\x9F\x98\x80", "http://x.org", "[x]: /u \"t\"", "[x]", "- [ ] ", "<http://a.b>"
    };
    int iter, k;

    for (iter = 0; iter < 3000; iter++) {
        char buf[2048];
        size_t len = 0;
        int pieces = 1 + (int)(rnd() % 60);
        MdDoc *d;
        char *r, *h;

        for (k = 0; k < pieces; k++) {
            const char *b = bits[rnd() % (sizeof(bits) / sizeof(bits[0]))];

            if (len + strlen(b) + 1 < sizeof(buf)) {
                strcpy(buf + len, b);
                len += strlen(b);
            }
        }
        d = md_parse(buf, len);
        CHECK(d != NULL);
        if (d == NULL) {
            continue;
        }
        r = md_to_rtf(d, NULL, NULL);
        h = md_to_html(d, NULL);
        if (r == NULL || h == NULL || !braces_balanced(r)) {
            t_fail++;
            fprintf(stderr, "  FAIL fuzz iteration %d: %.*s\n", iter, (int)len, buf);
        } else {
            t_pass++;
        }
        free(r);
        free(h);
        md_free(d);
    }
}

int main(void)
{
    test_gfm();
    test_inlines();
    test_rtf_basics();
    test_rtf_lists();
    test_rtf_charsets();
    test_rtf_options();
    test_rtf_tables();
    test_limits();
    test_fuzz();
    TEST_DONE("md");
}
