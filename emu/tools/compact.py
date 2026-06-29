#!/usr/bin/env python3
"""Compact a generated engine source (purv.c) in place, without changing meaning.

The flattener emits very sparse C -- one struct field per line, lots of blank
lines, three-line guard blocks. This makes two whitespace-only passes:

  1. clang-format with a dense (but still 4-space, readable) style: collapse
     blank-line runs and put short blocks/ifs on one line.
  2. collapse the many single-field `typedef struct {...}` definitions that
     clang-format always leaves spread over three lines.

Both are purely cosmetic (C is free-form), so the compiled output is unchanged.
The inline style means no .clang-format file is left around to affect the
hand-written sources. Used by `make regen`; run directly as: compact.py FILE...
"""
import re, subprocess, sys

STYLE = ("{BasedOnStyle: LLVM, IndentWidth: 4, ColumnLimit: 100, "
         "MaxEmptyLinesToKeep: 1, AllowShortBlocksOnASingleLine: Always, "
         "AllowShortIfStatementsOnASingleLine: AllIfsAndElse, "
         "AllowShortLoopsOnASingleLine: true, AllowShortCaseLabelsOnASingleLine: true, "
         "AllowShortFunctionsOnASingleLine: Inline, AllowShortEnumsOnASingleLine: true, "
         "BreakBeforeBraces: Attach, SpaceBeforeParens: ControlStatements}")

# A single-field packed struct: three lines clang-format won't join -> one line.
SINGLE_FIELD = re.compile(
    r'typedef struct __attribute__\(\(packed\)\) \{\n {4}([^\n{}]+;)\n\} (\w+);')
# A blank line between two one-line packed-struct typedefs -> drop it (they read
# as a table of register/instruction layouts).
ONELINE = r'typedef struct __attribute__\(\(packed\)\) \{[^\n]*\} \w+;'
BLANK_BETWEEN = re.compile(r'(' + ONELINE + r')\n\n(?=' + ONELINE + r')')

for path in sys.argv[1:]:
    subprocess.run(["clang-format", "-i", "--style=" + STYLE, path], check=True)
    s = open(path).read()
    s = SINGLE_FIELD.sub(r'typedef struct __attribute__((packed)) { \1 } \2;', s)
    s = BLANK_BETWEEN.sub(r'\1\n', s)
    open(path, "w").write(s)
    print(f"compacted {path}: {s.count(chr(10)) + 1} lines")
