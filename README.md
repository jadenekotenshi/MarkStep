# MarkStep

A Markdown parser (CommonMark 0.31.2 plus GFM tables, strikethrough and task lists) and a Markdown to RTF converter in
portable C89, aimed at OPENSTEP 4.2: write or read Markdown, view it in Edit.app or OpenWrite (OpenWrite imports RTF).

**Status: work in progress, host-tested only; nothing has run on OPENSTEP yet.**

- `core/` -- parser (`md_block.c`, `md_inline.c`), HTML renderer (for tests), RTF renderer (`md_rtf.c`), NeXTSTEP charset table.
- `tools/md2rtf.c` -- the command-line filter: `md2rtf [options] [input.md]`, see its header for options
  (fonts, sizes, link style, raw HTML handling, `-c cp1252|nextstep|ascii` for accents).
- `tests/spec_run.c` runs the 652 CommonMark examples: 647 pass; the 5 known differences (tabs become spaces, only some
  named entities) are in `tests/spec_known_failures.txt`. Clean under AddressSanitizer/UBSan.
- `tests/test_md.c` (6000+ checks): GFM tables, strikethrough and task lists; the RTF writer (fonts, lists, quotes, code,
  tables, links, HTML handling, the three charsets, escapes); limits (deep nesting, NUL bytes, CR/CRLF); and a 3000-case
  fuzz run that checks nothing crashes and every RTF output has balanced braces.
- `make` builds `build/md2rtf`; `make test` runs the spec examples and the unit tests; `make lint` checks the sources for
  anything gcc 2.7.2 or OPENSTEP's libc lacks. On OPENSTEP: `make -f Makefile.openstep test`, then `... install`.

Not done yet: checking on real hardware that the code builds, and whether Edit.app and OpenWrite read the RTF as intended
(accents: try `-c nextstep` if cp1252 looks wrong). Limits worth knowing: tabs become spaces, only some named character
references are known, a table is written as aligned monospaced text (no RTF table), and links are shown as "text (address)"
because plain RTF has no hyperlinks.
