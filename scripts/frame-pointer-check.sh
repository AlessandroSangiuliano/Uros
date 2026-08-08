#!/bin/sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# frame-pointer-check.sh — can every function that calls something be walked
# through? (#409)
#
# The issue's own "watch for" asks this in as many words: "Backtraces rely on
# frame pointers (-fno-omit-frame-pointer, kept deliberately). Confirm that
# still holds on the new target, because losing backtraces here would cost far
# more than the frame pointer does."
#
# ⚠️ Reading the flag out of CMakeLists.txt is not confirming it.  A flag can be
# present and overridden, present in one target's options and absent from
# another's, or defeated by an attribute on one function.  What decides is what
# came out, so this asks the image.
#
# ── The question is narrower than "does every function push %rbp" ─────
#
# It is not, and asking that gets 659 answers out of 1960 on a kernel whose
# backtraces are perfectly good.  A LEAF function needs no frame: it is named
# by the saved instruction pointer, never appears in the middle of a chain, and
# GCC is right to leave the register alone.  The flag does not promise
# otherwise.
#
# What breaks a walk is a function that CALLS something and keeps no frame.
# That one does appear in the middle, and when the chain reaches it there is
# nothing linking its caller to its callee: the walk steps straight past, and
# the result is a stack that is missing a frame and says nothing about it.
#
# So the invariant is: a body containing a call establishes a frame somewhere
# in it.  Not necessarily first: GCC shrink-wraps, moving the prologue past
# early exits that never reach a call, so requiring it at the entry flags 311
# functions on a kernel whose backtraces are correct.
#
# ── And the exceptions, which are exceptions and not noise ────────────
#
# Only STT_FUNC symbols are considered, because that type is what a compiler
# emits and an assembler does not.  Hand-written entry points keep no frame by
# design — trap/entry.S, the syscall path, the probes — and are outside the set
# for a reason rather than by a filter that happens to drop them.
#
# Usage: frame-pointer-check.sh [kernel-elf]
# Exit status: 0 every calling function opens a frame · 1 some do not
#              · 2 could not ask.
set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
KERNEL=${1:-$REPO/uros/build-x86_64/export/uros/boot/mach_kernel}

if [ ! -f "$KERNEL" ]; then
	echo "frame-pointer-check: no kernel at $KERNEL" >&2
	exit 2
fi

python3 - "$KERNEL" <<'PY'
import re
import subprocess
import sys

kernel = sys.argv[1]

# Every compiler-emitted function with a body.  A zero-sized symbol describes
# no instructions, so there is nothing to ask about it.
funcs = {}
for line in subprocess.check_output(['readelf', '-sW', kernel],
                                    text=True).splitlines():
    f = line.split()
    if len(f) >= 8 and f[3] == 'FUNC' and f[2].isdigit() and int(f[2]) > 0:
        funcs[int(f[1], 16)] = f[7]

if not funcs:
    print('frame-pointer-check: no function symbols — is this the kernel?')
    sys.exit(2)

# Each function's instructions, gathered by the symbol header objdump prints.
#
# ⚠️ \s* and not \s+ on the instruction lines: objdump pads short addresses
# with leading spaces and prints long ones flush left.  A pattern that required
# them matched every function in .boot and not one in .text — and reported
# success, which is how a check that can never fail looks from outside.
bodies = {}
here = None
for line in subprocess.check_output(
        ['objdump', '-d', '--no-show-raw-insn', kernel], text=True).splitlines():
    m = re.match(r'^([0-9a-f]+) <([^>]+)>:', line)
    if m:
        here = int(m.group(1), 16)
        bodies[here] = []
        continue
    m = re.match(r'^\s*([0-9a-f]+):\s+(\S.*)$', line)
    if m and here is not None:
        bodies[here].append(m.group(2).strip())

bad = []
leaves = 0
checked = 0
for addr, name in sorted(funcs.items()):
    body = bodies.get(addr)
    if not body:
        continue                # not in a disassembled section

    if not any(i.startswith('call') for i in body):
        leaves += 1             # never in the middle of a chain
        continue

    checked += 1

    # ⚠️ Not "the FIRST instruction is push %rbp".  That was tried and
    # answered 311 of 1603 on a kernel whose backtraces are correct: GCC
    # shrink-wraps, moving the prologue past the early exits that never reach
    # a call, so the frame is set up on exactly the paths that need one and
    # the entry instruction is something else entirely.  What matters is that
    # the function establishes a frame at all — where it does so is the
    # compiler's business and it is not free to get that wrong.
    # ⚠️ And a `.cold' partition is not a frame of its own.
    #
    # GCC splits a function into a hot half and a cold one and gives the cold
    # half a symbol of its own, but it is reached by a JUMP from the hot half
    # and shares its frame — a walk that lands in it reads the parent's, which
    # is right.  So the question for one of these is whether the PARENT
    # establishes a frame, and asking it of the fragment answers about a
    # prologue that was never going to be there.
    subject, opening = name, body[0]
    if name.endswith('.cold'):
        parent = name[:-len('.cold')]
        parent_addr = next((a for a, n in funcs.items() if n == parent), None)
        if parent_addr is not None and bodies.get(parent_addr):
            subject, body = parent, bodies[parent_addr]

    if not any(i.startswith('push') and '%rbp' in i for i in body):
        bad.append((subject, opening))

print(f'frame-pointer-check: {checked} functions make a call, '
      f'{len(bad)} of them never establish one '
      f'({leaves} leaves need none and are not asked)')
for name, ins in bad[:40]:
    print(f'    {name}: no `push %rbp\' anywhere; opens with `{ins}\'')
if len(bad) > 40:
    print(f'    ... and {len(bad) - 40} more')

sys.exit(1 if bad else 0)
PY
