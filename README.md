# MarkStep

A Markdown parser (CommonMark 0.31.2 plus GFM tables, strikethrough and task lists) and a Markdown to RTF converter in
portable C89, aimed at OPENSTEP 4.2: write or read Markdown, view it in Edit.app or OpenWrite (OpenWrite imports RTF).

**Status: work in progress, host-tested only; nothing has run on OPENSTEP yet.**

- `core/` -- parser (`md_block.c`, `md_inline.c`), HTML renderer (for tests), RTF renderer (`md_rtf.c`), NeXTSTEP charset table.
- `tools/md2rtf.c` -- the command-line filter: `md2rtf [options] [input.md]`, see its header for options
  (fonts, sizes, link style, raw HTML handling, `-c cp1252|nextstep|ascii` for accents).
- `tests/spec_run.c` runs the 652 CommonMark examples: 647 pass; the 5 known differences (tabs become spaces, only some
  named entities) are in `tests/spec_known_failures.txt`. Clean under AddressSanitizer/UBSan. `tests/test_md.c` is a stub.
- `make` builds `build/md2rtf`; `make test` runs the spec examples.

Not done yet: unit tests for tables/RTF output/fuzzing, `Makefile.openstep`, lint script, and checking on real
hardware whether Edit.app/OpenWrite read the RTF as intended (accents: try `-c nextstep` if cp1252 looks wrong).
