/* test_rtf_in.c -- RTF to Markdown: crafted RTF of the kinds word processors write, round trips through md2rtf's own output,
 * and a fuzz run over random RTF-ish input. */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "../core/md.h"
#include "../core/rtf_in.h"
#include "../core/nsenc.h"
#include "test.h"

static char *conv(const char *rtf, int charset, int quotes)
{
    RtfMdOptions o;

    rtf_md_defaults(&o);
    o.charset = charset;
    o.quotes = quotes;
    return rtf_to_markdown(rtf, strlen(rtf), &o, NULL);
}

static int md_is(const char *rtf, const char *want)
{
    char *got = conv(rtf, RTF_CHARSET_AUTO, 1);
    int ok = got != NULL && strcmp(got, want) == 0;

    if (!ok) {
        fprintf(stderr, "    rtf:    %s\n    wanted: [%s]\n    got:    [%s]\n", rtf, want, got ? got : "(null)");
    }
    free(got);
    return ok;
}

#define HEAD "{\\rtf1\\ansi\\deff0{\\fonttbl{\\f0\\fswiss Helvetica;}{\\f1\\fmodern Courier;}{\\f2\\fnil Symbol;}}{\\colortbl;\\red0\\green0\\blue0;\\red0\\green0\\blue200;\\red100\\green100\\blue100;}"

static int round_trip(const char *md, const char *want)
{
    MdDoc *d = md_parse(md, strlen(md));
    char *rtf = d ? md_to_rtf(d, NULL, NULL) : NULL;
    char *back = rtf ? rtf_to_markdown(rtf, strlen(rtf), NULL, NULL) : NULL;
    int ok = back != NULL && strcmp(back, want) == 0;

    if (!ok) {
        fprintf(stderr, "    markdown: [%s]\n    wanted:   [%s]\n    came back:[%s]\n", md, want, back ? back : "(null)");
    }
    free(back);
    free(rtf);
    md_free(d);
    return ok;
}

static void test_text(void)
{
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 Hello world\\par}", "Hello world\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 A {\\b bold}, {\\i italic}, {\\b\\i both}, {\\strike gone}.\\par}", "A **bold**, *italic*, ***both***, ~~gone~~.\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 one\\par\\pard\\plain\\f0\\fs24 two\\par}", "one\n\ntwo\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 a\\line b\\par}", "a\\\nb\n"));                                  /* \line: a hard break */
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\b bold }text {\\i  it }end\\par}", "**bold** text *it* end\n"));     /* blanks go outside the markers */
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 \\b bold\\b0  plain\\par}", "**bold** plain\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\b a} {\\b b}\\par}", "**a b**\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 x {\\v hidden} y\\par}", "x y\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 Use {\\f1 printf()} and {\\f1 a`b}.\\par}", "Use `printf()` and ``a`b``.\n"));
    CHECK(md_is("", ""));
    CHECK(md_is("{\\rtf1}", ""));
    CHECK(md_is("plain text, no RTF at all", "plain text, no RTF at all\n"));
}

static void test_chars(void)
{
    char want[64], rtf[200];
    char *got;

    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 caf\\'e9 \\'93q\\'94 \\'97 \\'80\\par}", "caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x94 \xE2\x82\xAC\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 \\u8220?q\\u8221? \\u233e\\par}", "\xE2\x80\x9Cq\xE2\x80\x9D \xC3\xA9\n"));  /* \uN with its one fallback character (the e) skipped */
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 \\u-10179?\\u-8704?!\\par}", "\xF0\x9F\x98\x80!\n"));                  /* a surrogate pair */
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 a\\~b \\emdash \\endash \\bullet \\lquote x\\rquote \\ldblquote y\\rdblquote\\par}",
                "a b \xE2\x80\x94\xE2\x80\x93\xE2\x80\xA2\xE2\x80\x98" "x\xE2\x80\x99\xE2\x80\x9C" "y\xE2\x80\x9D\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 \\\\ \\{ \\}\\par}", "\\\\ { }\n"));
    /* \rtf0 is how NeXT's writer starts: the bytes are NeXTSTEP's own; \rtf1 means Windows-1252; -c overrides */
    sprintf(rtf, "{\\rtf0\\ansi{\\fonttbl\\f0\\fswiss Helvetica;}\\f0\\fs24 caf\\'%02x\\par}", nsenc_encode(0xE9));
    got = conv(rtf, RTF_CHARSET_AUTO, 1);
    CHECK(got != NULL && strcmp(got, "caf\xC3\xA9\n") == 0);
    free(got);
    sprintf(rtf, "{\\rtf1\\ansi{\\fonttbl\\f0\\fswiss Helvetica;}\\f0\\fs24 caf\\'e9\\par}");
    got = conv(rtf, RTF_CHARSET_NEXTSTEP, 1);
    sprintf(want, "caf%s\n", "\xC3\xA9");
    CHECK(got != NULL && strcmp(got, want) != 0);                                                               /* 0xE9 is another letter in NeXTSTEP */
    free(got);
    got = conv("{\\rtf0\\ansi\\f0 caf\\'e9\\par}", RTF_CHARSET_CP1252, 1);
    CHECK(got != NULL && strcmp(got, "caf\xC3\xA9\n") == 0);
    free(got);
}

static void test_escaping(void)
{
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 2 * 3 = 6, snake_case, _lead, a [b] <c> & AT&T &amp; x\\par}", "2 \\* 3 = 6, snake_case, \\_lead, a \\[b\\] \\<c> & AT&T \\&amp; x\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 # not a heading\\par}", "\\# not a heading\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 > not a quote\\par}", "\\> not a quote\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 x\\line - not a list\\par}", "x\\\n\\- not a list\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 ~~x~~ and ~y~\\par}", "\\~~x\\~~ and ~y~\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\b *star*}\\par}", "**\\*star\\***\n"));
}

static void test_structure(void)
{
    /* headings: by style name, by being large and bold; bold alone stays bold */
    CHECK(md_is("{\\rtf1\\ansi{\\fonttbl{\\f0 Helvetica;}}{\\stylesheet{\\s0 Normal;}{\\s1 heading 1;}{\\s2 heading 2;}}"
                "\\pard\\s1\\f0\\fs40 Top\\par\\pard\\s2\\f0\\fs30 Next\\par\\pard\\s0\\f0\\fs24 text\\par}", "# Top\n\n## Next\n\ntext\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 body text here and more body text\\par\\pard\\plain\\f0\\fs43\\b Big\\par\\pard\\plain\\f0\\fs30\\b Mid\\par"
                "\\pard\\plain\\f0\\fs24 more body text and some more words\\par}", "body text here and more body text\n\n# Big\n\n### Mid\n\nmore body text and some more words\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\b Just bold\\par\\pard\\plain\\f0\\fs24 and then body text, plenty of it, so that 24 is the body size\\par}",
                "**Just bold**\n\nand then body text, plenty of it, so that 24 is the body size\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 above\\par\\pard\\qc\\f0\\fs24 \\emdash\\emdash\\emdash\\emdash\\emdash\\emdash\\emdash\\emdash\\par\\pard\\plain\\f0\\fs24 below\\par}", "above\n\n---\n\nbelow\n"));
    /* lists: a bullet and tab in the text, a Symbol-font \listtext, numbers, letters, nesting by indent, continuation paragraphs */
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab one\\par\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab two\\par}", "- one\n- two\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li720\\fi-360{\\listtext\\f2\\'b7\\tab}one\\par\\pard\\plain\\f0\\fs24\\li720\\fi-360{\\pntext\\f2\\'b7\\tab}two\\par}", "- one\n- two\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 1.\\tab a\\par\\pard\\plain\\f0\\fs24 2.\\tab b\\par\\pard\\plain\\f0\\fs24 3) c\\par}", "1. a\n2. b\n3. c\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 5.\\tab five\\par\\pard\\plain\\f0\\fs24 6.\\tab six\\par}", "5. five\n6. six\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 a.\\tab first\\par\\pard\\plain\\f0\\fs24 b.\\tab second\\par}", "1. first\n2. second\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab a\\par\\pard\\plain\\f0\\fs24\\li720\\fi-360 \\bullet\\tab b\\par\\pard\\plain\\f0\\fs24\\li1080\\fi-360 \\bullet\\tab c\\par"
                "\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab d\\par}", "- a\n    - b\n        - c\n- d\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab a\\par\\pard\\plain\\f0\\fs24\\li360 more of a\\par\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab b\\par}",
                "- a\n\n    more of a\n- b\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li360\\fi-360 \\bullet\\tab a\\par\\pard\\plain\\f0\\fs24 plain after\\par}", "- a\n\nplain after\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 [x]\\tab done\\par\\pard\\plain\\f0\\fs24 [ ]\\tab todo\\par}", "- [x] done\n- [ ] todo\n"));
    /* code: monospaced paragraphs join into one block when set tight, stay apart when spaced; inline mono */
    CHECK(md_is(HEAD "\\pard\\plain\\f1\\fs20 int x;\\par\\pard\\plain\\f1\\fs20   return x;\\par}", "```\nint x;\n  return x;\n```\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f1\\fs20\\sa120 one\\par\\pard\\plain\\f1\\fs20\\sa120 two\\par}", "```\none\n```\n\n```\ntwo\n```\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f1\\fs20\\li240\\sa120 a\\line   b\\par}", "```\na\n  b\n```\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f1\\fs20 has ``` inside\\par}", "````\nhas ``` inside\n````\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f1\\fs20 a\\tab b\\par}", "```\na    b\n```\n"));
    /* quotes: grey or italic and indented; by style; off by option */
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li360\\cf3 quoted text\\par}", "> quoted text\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li360\\i quoted\\par\\pard\\plain\\f0\\fs24\\li360\\i again\\par}", "> *quoted*\n>\n> *again*\n"));
    CHECK(md_is("{\\rtf1\\ansi{\\fonttbl{\\f0 Helvetica;}}{\\stylesheet{\\s5 Quote;}}\\pard\\s5\\f0\\fs24 by style\\par}", "> by style\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24\\li720\\cf3 deeper\\par}", "> > deeper\n"));
    {
        char *g = conv(HEAD "\\pard\\plain\\f0\\fs24\\li360\\cf3 quoted text\\par}", RTF_CHARSET_AUTO, 0);

        CHECK(g != NULL && strcmp(g, "quoted text\n") == 0);
        free(g);
    }
}

static void test_links_images_tables(void)
{
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 see {\\field{\\*\\fldinst HYPERLINK \"http://example.com/a\"}{\\fldrslt {\\ul\\cf2 the page}}} now\\par}", "see [the page](http://example.com/a) now\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\field{\\*\\fldinst PAGE}{\\fldrslt 3}}\\par}", "3\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\ul\\cf2 text}{\\cf3  (http://x.org/a)} and {\\ul\\cf2 http://y.org}\\par}", "[text](http://x.org/a) and <http://y.org>\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\i [image: a logo]}{\\cf3  (l.png)} x {\\i [image]}\\par}", "![a logo](l.png) x ![]()\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 {\\ul underlined} plain\\par}", "underlined plain\n"));
    /* a Word-style table */
    CHECK(md_is(HEAD "\\trowd\\cellx2000\\cellx4000\\pard\\intbl\\f0\\fs24 Name\\cell\\pard\\intbl\\f0\\fs24 Qty\\cell\\row"
                "\\trowd\\cellx2000\\cellx4000\\pard\\intbl\\qc\\f0\\fs24 apple\\cell\\pard\\intbl\\qr\\f0\\fs24 3\\cell\\row\\pard\\plain\\f0\\fs24 after\\par}",
                "| Name | Qty |\n| :---: | ---: |\n| apple | 3 |\n\nafter\n"));
    CHECK(md_is(HEAD "\\trowd\\cellx2000\\pard\\intbl\\f0\\fs24 a|b\\cell\\row}", "| a\\|b |\n| --- |\n"));
    /* what md2rtf wrote for a table: aligned text in a monospaced block comes back as a table */
    CHECK(md_is(HEAD "\\pard\\plain\\f1\\fs20\\li240\\sa120 | a   |   b |\\line |-----|-----|\\line | 1   |  22 |\\par}", "| a   |   b |\n|-----|-----|\n| 1   |  22 |\n"));
}

static void test_skipping(void)
{
    CHECK(md_is("{\\rtf1\\ansi{\\fonttbl{\\f0\\fswiss Helvetica;}}{\\info{\\title Secret title}{\\author Me}}{\\*\\generator Word 99;}\\pard\\f0\\fs24 body\\par}", "body\n"));
    CHECK(md_is(HEAD "\\pard\\plain\\f0\\fs24 a{\\pict\\wmetafile8 0102030405}b{\\*\\unknowndest hidden words}c\\par}", "abc\n"));
    CHECK(md_is(HEAD "{\\header\\pard header text\\par}\\pard\\plain\\f0\\fs24 body\\par{\\footer\\pard footer\\par}}", "body\n"));
    CHECK(md_is("{\\rtf1\\ansi{\\fonttbl{\\f0\\fswiss\\fcharset0 Arial;}{\\f1\\fmodern Courier New;}}\\pard\\f0 {\\f1 mono} Arial\\par}", "`mono` Arial\n"));
}

static void test_robust(void)
{
    size_t i, n = 20000;
    char *s = (char *)malloc(n * 3 + 64), *g;

    /* unbalanced and absurd input: no crash, some answer */
    g = conv("{\\rtf1 {{{{ text", RTF_CHARSET_AUTO, 1);
    CHECK(g != NULL);
    free(g);
    g = conv("}}}} text {\\b bold", RTF_CHARSET_AUTO, 1);
    CHECK(g != NULL);
    free(g);
    g = conv("{\\rtf1 \\b\\i\\fs\\f\\li\\uc\\u \\' \\'z \\*\\ \\", RTF_CHARSET_AUTO, 1);
    CHECK(g != NULL);
    free(g);
    for (i = 0; i < n; i++) {
        s[i] = '{';
    }
    strcpy(s + n, "text");
    g = rtf_to_markdown(s, strlen(s), NULL, NULL);
    CHECK(g != NULL);
    free(g);
    strcpy(s, "{\\rtf1 ");
    for (i = 0; i < n; i++) {
        strcat(s, "{\\b x}");
        if (i > 3000) {
            break;
        }
    }
    g = rtf_to_markdown(s, strlen(s), NULL, NULL);
    CHECK(g != NULL);
    free(g);
    CHECK(rtf_to_markdown(NULL, 0, NULL, NULL) != NULL);
    free(s);
}

static unsigned long seed = 4242UL;

static unsigned long rnd(void)
{
    seed = seed * 1103515245UL + 12345UL;
    return (seed >> 8) & 0xFFFFFFUL;
}

static void test_fuzz(void)
{
    static const char *const bits[] = {
        "{", "}", "\\rtf1", "\\rtf0", "\\ansi", "\\pard", "\\plain", "\\par ", "\\line ", "\\tab ", "\\b ", "\\b0 ", "\\i ", "\\ul ", "\\strike ",
        "\\fs24 ", "\\fs40 ", "\\f1 ", "\\f0 ", "\\li360", "\\fi-360", "\\cf3 ", "\\qc ", "\\intbl ", "\\cell ", "\\row ", "\\trowd ", "\\s1 ",
        "\\'e9", "\\'", "\\u233?", "\\u-10179?", "\\uc0 ", "\\bullet ", "\\emdash ", "\\*\\fldinst HYPERLINK \"http://x.org\"", "\\field",
        "\\fldrslt ", "\\fonttbl", "\\colortbl;\\red1\\green2\\blue3;", "\\stylesheet", "\\listtext", "\\pntext", "\\info", "\\pict", "\\*",
        "text ", "word ", "1.\\tab ", "\\bullet\\tab ", "[x]\\tab ", "# ", "* ", "_", "|", "\\\\", "\\{", "\\}", ";", "Courier;", "\\fmodern ", "\\v "
    };
    int iter, k;

    for (iter = 0; iter < 4000; iter++) {
        char buf[2048];
        size_t len = 0;
        int pieces = 1 + (int)(rnd() % 80);
        char *g;

        for (k = 0; k < pieces; k++) {
            const char *b = bits[rnd() % (sizeof(bits) / sizeof(bits[0]))];

            if (len + strlen(b) + 1 < sizeof(buf)) {
                strcpy(buf + len, b);
                len += strlen(b);
            }
        }
        g = rtf_to_markdown(buf, len, NULL, NULL);
        if (g == NULL) {
            t_fail++;
            fprintf(stderr, "  FAIL fuzz iteration %d: %.*s\n", iter, (int)len, buf);
        } else {
            t_pass++;
        }
        free(g);
    }
}

static void test_round_trips(void)
{
    CHECK(round_trip("# Title\n\ntext with **bold**, *italic*, ***both*** and `code`.\n", "# Title\n\ntext with **bold**, *italic*, ***both*** and `code`.\n"));
    CHECK(round_trip("## Two\n\n### Three\n\nbody\n", "## Two\n\n### Three\n\nbody\n"));
    CHECK(round_trip("- a\n- b\n  - c\n- d\n", "- a\n- b\n    - c\n- d\n"));
    CHECK(round_trip("1. x\n2. y\n3. z\n", "1. x\n2. y\n3. z\n"));
    CHECK(round_trip("> quote one\n>\n> quote two\n", "> quote one\n>\n> quote two\n"));
    CHECK(round_trip("```\ncode  line\n  indented\n```\n", "```\ncode  line\n  indented\n```\n"));
    CHECK(round_trip("[t](http://x.org/a) and <http://y.org> and ![alt](p.png)\n", "[t](http://x.org/a) and <http://y.org> and ![alt](p.png)\n"));
    CHECK(round_trip("- [x] done\n- [ ] open\n", "- [x] done\n- [ ] open\n"));
    CHECK(round_trip("a\n\n---\n\nb\n", "a\n\n---\n\nb\n"));
    CHECK(round_trip("~~gone~~ and \\*stars\\* and snake_case\n", "~~gone~~ and \\*stars\\* and snake_case\n"));
    CHECK(round_trip("| a | b |\n|---|--:|\n| 1 | 22 |\n", "| a   |   b |\n|-----|-----|\n| 1   |  22 |\n"));
    CHECK(round_trip("caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x94 \xE2\x82\xAC\n", "caf\xC3\xA9 \xE2\x80\x9Cq\xE2\x80\x9D \xE2\x80\x94 \xE2\x82\xAC\n"));
    CHECK(round_trip("line one  \nline two\n", "line one\\\nline two\n"));
    CHECK(round_trip("1. a\n\n   para in item\n2. b\n", "1. a\n\n    para in item\n2. b\n"));
}

int main(void)
{
    test_text();
    test_chars();
    test_escaping();
    test_structure();
    test_links_images_tables();
    test_skipping();
    test_robust();
    test_fuzz();
    test_round_trips();
    TEST_DONE("rtf_in");
}
