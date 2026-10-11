#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# fbcons-no-glyph.py — the lines of a serial log that the framebuffer console
# cannot draw as they read (#661).
#
# The console decodes the UTF-8 it is handed and draws each code point with
# the glyph device/fbcons_font.h's table names; a code point the table does not
# name is drawn as the replacement, and that line does not read on the screen
# as it reads on the wire.  This reads the log the wire wrote and names every
# such line, with its code points, one line of output each:
#
#   <line number>: U+XXXX[ U+YYYY...]: <the line>
#
# 🔑 FROM THE LOG, NOT FROM THE KERNEL'S COUNT.  The kernel counts them too,
# and says the count when the quiet census reports; but a census reports when
# the machine idles, and a line printed after its last report was never
# counted anywhere a reader could see.  Entry 9's "⚠️" was: the bench prints
# it after the census's last word, and a judge reading the count passed it.
#
# The table is parsed by fbcons-readback.py's own parser, so the console, the
# screen reader and this read one table.  Only the guest's own output: from
# the kernel's first line, since what came before it is the loader's, on a
# terminal of its own, to the first line run-x86_64.sh appends after the run
# -- qemu's messages, the run's conditions, whose "⚠️ THE CLOCK MOVED" no
# screen ever drew -- found as double-panic-check.sh finds it.  Bytes that
# are not UTF-8 are not named: the wire carries them as broken as the screen
# does.
#
# Usage:  scripts/fbcons-no-glyph.py LOG
# Exit:   0, whatever it found; 2 when the table or the log cannot be read.

import importlib.util, os, re, sys

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
# Where the guest's output ends and what the harness appends begins.
APPENDED = re.compile(r"^(qemu-system-x86_64:|=== run conditions|"
                      r"=== qemu.s own messages)")


def main():
    if len(sys.argv) != 2:
        print("usage: fbcons-no-glyph.py LOG", file=sys.stderr)
        return 2
    sys.dont_write_bytecode = True		# nothing left in scripts/
    spec = importlib.util.spec_from_file_location(
        "fbcons_readback", os.path.join(REPO, "scripts/fbcons-readback.py"))
    rb = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rb)
    table = rb.load_map(os.path.join(REPO, rb.FONT_H))
    try:
        text = open(sys.argv[1], "rb").read().decode("utf-8", errors="replace")
    except OSError as e:
        print(f"fbcons-no-glyph: {e}", file=sys.stderr)
        return 2
    start = text.find(rb.KERNEL_FIRST)
    if start < 0:
        return 0
    first = text.count("\n", 0, start) + 1
    for n, line in enumerate(text[start:].split("\n"), first):
        if APPENDED.match(line):
            break
        lacking = sorted({ord(ch) for ch in line
                          if ord(ch) >= 0x80 and ord(ch) not in table})
        if lacking:
            cps = " ".join("U+%04X" % cp for cp in lacking)
            print(f"{n}: {cps}: {line.rstrip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
