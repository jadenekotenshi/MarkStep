# MarkStep

A Markdown parser (CommonMark 0.31.2 plus GFM tables, strikethrough and task lists) and a Markdown to RTF converter in
portable C89, aimed at OPENSTEP 4.2: write or read Markdown, view it in Edit.app or OpenWrite (OpenWrite imports RTF).

**Status: work in progress, host-tested only; nothing has run on OPENSTEP yet.** MIT licensed (see `LICENSE`).

- `core/` -- parser (`md_block.c`, `md_inline.c`), HTML renderer (for tests), RTF renderer (`md_rtf.c`), NeXTSTEP charset table.
- `tools/md2rtf.c` -- the command-line filter: `md2rtf [options] [input.md]`, see its header for options
  (fonts, sizes, link style, raw HTML handling, `-c cp1252|nextstep|ascii` for accents).
- `core/rtf_in.c`, `tools/rtf2md.c` -- the way back, `rtf2md [options] [input.rtf]`: lossy, but headings (from style names or
  from large bold text), bold/italic/strikethrough, monospaced runs as code, bulleted and numbered lists with their nesting
  (and `\\listtext`/`\\pntext` markers), block quotes, Word-style tables, hyperlink fields, images and rules come back as
  Markdown; fonts, colours, margins and alignment are dropped. An RTF starting `{\\rtf0` (how NeXT's writer begins) is
  read as NeXTSTEP-encoded, anything else as Windows-1252; `-c` overrides. What `md2rtf` writes comes back close to what
  went in (`tests/test_rtf_in.c` has the round trips).
- `tests/spec_run.c` runs the 652 CommonMark examples: 647 pass; the 5 known differences (tabs become spaces, only some
  named entities) are in `tests/spec_known_failures.txt`. Clean under AddressSanitizer/UBSan.
- `tests/test_md.c` (6000+ checks): GFM tables, strikethrough and task lists; the RTF writer (fonts, lists, quotes, code,
  tables, links, HTML handling, the three charsets, escapes); limits (deep nesting, NUL bytes, CR/CRLF); and a 3000-case
  fuzz run that checks nothing crashes and every RTF output has balanced braces.
- `tests/test_rtf_in.c` (4000+ checks): crafted RTF of the kinds word processors write, escapes, charsets, round trips through
  `md2rtf`, robustness (unbalanced, absurdly nested, garbage) and a 4000-case fuzz run.
- `make` builds `build/md2rtf` and `build/rtf2md`; `make test` runs the spec examples and the unit tests; `make lint` checks the sources for
  anything gcc 2.7.2 or OPENSTEP's libc lacks. On OPENSTEP: `make -f Makefile.openstep test`, then `... install`.

Not done yet: checking on real hardware that the code builds, and whether Edit.app and OpenWrite read the RTF as intended
(accents: try `-c nextstep` if cp1252 looks wrong). Limits worth knowing: tabs become spaces, only some named character
references are known, a table is written as aligned monospaced text (no RTF table), and links are shown as "text (address)"
because plain RTF has no hyperlinks.

The code is under the MIT licence. The CommonMark examples in `tests/spec_cases.txt` are (c) John MacFarlane and contributors, CC BY-SA 4.0: see `tests/SPEC-NOTICE.txt`.
