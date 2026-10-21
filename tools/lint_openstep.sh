#!/bin/sh
# Reject language features and library calls that gcc 2.7.2 / OPENSTEP 4.2's libc do not have.  Run: make lint
cd "$(dirname "$0")/.." || exit 2
status=0
check() {   # description, extended-regex, files...
    desc=$1; re=$2; shift 2
    hits=$(grep -nE "$re" "$@" 2>/dev/null | grep -v '^[^:]*:[0-9]*:[[:space:]]*/\?\*' | grep -v '^[^:]*:[0-9]*:[[:space:]]*\*')
    if [ -n "$hits" ]; then echo "LINT: $desc"; echo "$hits" | sed 's/^/    /'; status=1; fi
}
SRC="core/*.c core/*.h tools/*.c"
check "// comments (gcc 2.7.2 -ansi rejects them in C)"      '(^|[^:"])//[^"]*$'                       $SRC
check "stdint / stdbool / inline / restrict"                  '#include[[:space:]]*<(stdint|stdbool|inttypes)\.h>|\binline\b|\brestrict\b' $SRC
check "declaration in for-init (C99)"                         'for[[:space:]]*\((int|unsigned|size_t|long)[[:space:]]+[a-z_]+[[:space:]]*=' $SRC
check "snprintf/vsnprintf (not guaranteed on 4.2)"            '\bv?snprintf\b'                          $SRC
check "putenv/setenv/unsetenv (getenv only on 4.2)"           '\b(putenv|setenv|unsetenv)[[:space:]]*\(' $SRC
check "strdup/strcasecmp/strncasecmp/getopt (not ANSI; use own)" '\b(strdup|strcasecmp|strncasecmp|getopt|getopt_long|isatty)[[:space:]]*\(' core/*.c tools/*.c
check "long long / LL suffix (32-bit long; keep to long)"     '\blong[[:space:]]+long\b|[0-9]LL\b'      $SRC
check "S_ISREG/S_ISDIR (not in 4.2)"                          '\bS_IS(REG|DIR|LNK)\b'                   $SRC
check "mkdir -p / cp -r / install -D in Makefile.openstep"    '(^|[[:space:]@;&|])(mkdir[[:space:]]+-p|install[[:space:]]+-D|cp[[:space:]]+-a)\b' Makefile.openstep
exit $status
