# Host-side build & test (macOS/Linux).  The OPENSTEP build lives in Makefile.openstep.
# Flags approximate what gcc 2.7.2 will accept: strict C89, no // comments, no mixed declarations, no stdint.
CC      ?= cc
CFLAGS  = -std=c89 -pedantic -Wall -Wextra -Wdeclaration-after-statement -Wno-long-long -Wno-unused-parameter -O2 -g -Icore
BUILD   = build
CORE    = core/md.c core/md_util.c core/md_block.c core/md_inline.c core/md_html.c core/md_rtf.c core/nsenc.c
COREOBJ = $(patsubst core/%.c,$(BUILD)/%.o,$(CORE))

all: test

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: core/%.c core/md.h core/md_int.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/spec_run: tests/spec_run.c $(COREOBJ)
	$(CC) $(CFLAGS) tests/spec_run.c $(COREOBJ) -o $@

$(BUILD)/test_md: tests/test_md.c tests/test.h $(COREOBJ)
	$(CC) $(CFLAGS) tests/test_md.c $(COREOBJ) -o $@

$(BUILD)/md2rtf: tools/md2rtf.c $(COREOBJ)
	$(CC) $(CFLAGS) tools/md2rtf.c $(COREOBJ) -o $@

all: $(BUILD)/md2rtf

test: $(BUILD)/spec_run $(BUILD)/test_md
	$(BUILD)/spec_run tests/spec_cases.txt tests/spec_known_failures.txt
	$(BUILD)/test_md

lint:
	sh tools/lint_openstep.sh

clean:
	rm -rf $(BUILD)

.PHONY: all test lint clean
