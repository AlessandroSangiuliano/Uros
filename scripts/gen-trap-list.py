#!/usr/bin/env python3
#
# gen-trap-list.py — the traps migcom may short-circuit to (#543)
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ── Why a generated list and not a hand-written one ──────────────────────
#
# migcom has to know which kernel traps exist before it can emit a call to
# one, and it must not guess: a name that is not there is a link error, and a
# name that is there with different arguments is worse.
#
# 🔑 The list is derived from syscall_sw.h, which is the same file that DEFINES
# the traps.  A hand-kept list would drift the moment somebody adds or removes
# an entry, and it would drift silently -- a stale name simply stops being
# emitted, and nobody notices a fast path that quietly went away.
#
# ⚠️ Two layers of checking, not one.  This file carries the argument count so
# migcom can decline a routine whose stub takes a different number, and the
# generated call is compiled against the prototypes in <mach/mach_syscalls.h>,
# so a TYPE mismatch is a build error rather than a corrupted call frame.
# Neither layer alone is enough: the count cannot see types, and the compiler
# never sees a call that was never emitted.
#
# Output, one per line:
#     <trap name> <argument count>
#
# Uso:
#   scripts/gen-trap-list.py <syscall_sw.h> [output]

import re
import sys
from pathlib import Path

# kernel_trap(syscall_mach_port_allocate,-72,3)
ENTRY = re.compile(r'^\s*kernel_trap\(\s*([A-Za-z_]\w*)\s*,\s*(-?\d+)\s*,'
                   r'\s*(\d+)\s*\)', re.M)


def main(argv) -> int:
    if len(argv) < 2:
        print(__doc__ or 'uso: gen-trap-list.py <syscall_sw.h> [output]',
              file=sys.stderr)
        return 2

    src = Path(argv[1])
    text = src.read_text(errors='replace')

    seen = {}
    for m in ENTRY.finditer(text):
        name, _num, argc = m.group(1), m.group(2), int(m.group(3))
        # ⚠️ A name appearing twice with two argument counts is not something
        # to pick a winner for.  It means the header disagrees with itself and
        # whatever migcom emitted would be right for one caller and wrong for
        # the other.
        if name in seen and seen[name] != argc:
            print(f'{src}: {name} declared with {seen[name]} and {argc} '
                  f'arguments', file=sys.stderr)
            return 1
        seen[name] = argc

    if not seen:
        # 🔴 An empty list is not "no traps to use", it is "this file no longer
        # looks the way this script expects".  Saying so beats emitting nothing
        # and letting every fast path disappear without a word.
        print(f'{src}: no kernel_trap() entries matched — the header has '
              f'changed shape', file=sys.stderr)
        return 1

    lines = [f'{n} {seen[n]}\n' for n in sorted(seen)]
    if len(argv) > 2:
        Path(argv[2]).write_text(''.join(lines))
    else:
        sys.stdout.writelines(lines)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
