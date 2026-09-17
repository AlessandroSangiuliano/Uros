#!/usr/bin/env python3
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# No vector instruction in the kernel except where one is declared (#561).
#
# ── What this is defending ────────────────────────────────────────────
#
# x86-64 stopped carrying a thread's vector state across every context switch.
# A thread of the kernel task is exempt, because everything it executes is
# compiled -mgeneral-regs-only and the compiler therefore cannot emit an SSE or
# AVX instruction into it.  That exemption is worth 114 ns a switch, and it is
# correct for exactly as long as that sentence stays true.
#
# 🔴 THE COMPILER FLAG IS NOT THE GUARANTEE.  A human can write a vector
# instruction by hand in inline asm, and one does: x86_64/thread/fpu_stress.c
# holds a pattern in all sixteen registers on purpose, which is the whole point
# of that file.  It declares itself -- context_needs_vector_state() -- and gets
# its state carried.
#
# What has no defence at all is the NEXT one: someone writing vector asm in a
# kernel path a year from now, in a thread that never declared anything.  It
# would corrupt whatever the registers held, which is some user thread's state,
# and nothing would report it.  A wrong answer with no witness is what this
# project keeps having to correct; so this is the witness, and it runs in the
# build rather than in somebody's memory.
#
# ⚠️ Not a substitute for fpu_stress.  That asks whether a declared thread's
# state survives; this asks whether an UNdeclared one has any.  Both directions
# are needed and neither implies the other.
#
# ── How it decides ────────────────────────────────────────────────────
#
# objdump groups its disassembly by symbol, so every instruction is attributed
# to the function it is in.  A vector instruction outside the allow list below
# fails the build and prints the symbol, the address and the instruction, which
# is enough to go and read it.
#
# 🔑 The allow list is by SYMBOL and not by file, because that is what the
# binary knows.  Adding to it is not forbidden -- it is a decision, and the
# entry has to say which thread declares the state those instructions touch.

import re
import subprocess
import sys

#
# Symbols allowed to contain vector instructions, and why.
#
# Every entry names the thread that declares the state, because an entry that
# cannot name one is an entry that should not be here.
#
ALLOWED = {
    # The switch machinery itself: these ARE the save and the restore.
    # x86_64/thread/fpu.c, one instruction each.
    "fpu_save": "the save instruction (XSAVEOPT/XSAVE/FXSAVE) itself",
    "fpu_restore": "the restore instruction (XRSTOR/FXRSTOR) itself",

    # x86_64/thread/context.S, called from boot_c.c before there are threads:
    # it switches between two contexts built by hand to ask whether vector
    # state survives a switch at all (#453).  Those contexts go through
    # context_init(), which sets the carrying flag -- so they are declared, by
    # the default rather than by a call.
    "fpu_probe": "boot probe: hand-built contexts, carried by context_init's "
                 "default, before any thread exists",

    # x86_64/thread/fpu_stress.S, run by the threads fpu_stress.c creates --
    # which call context_needs_vector_state() precisely so that these
    # instructions are safe.  This is the exception the whole rule is written
    # around.
    "fpu_stress": "fpu_stress threads, declared with "
                  "context_needs_vector_state()",
}

# What a vector instruction looks like in objdump's output: an operand naming a
# vector register, or one of the state-moving instructions by name.
VECTOR_OPERAND = re.compile(r"%(x|y|z)mm\d")
VECTOR_MNEMONIC = re.compile(
    r"^\s*(v?(mov(a|u)p[sd]|movdq[au]|movs[sd])|"
    r"xsave\w*|xrstor\w*|fxsave\w*|fxrstor\w*|vzero\w+)\b")

SYMBOL_LINE = re.compile(r"^[0-9a-f]+ <([^>]+)>:")


def main(argv):
    if len(argv) != 2:
        print("usage: kernel-vector-check.py <kernel-elf>", file=sys.stderr)
        return 2

    kernel = argv[1]
    try:
        out = subprocess.run(["objdump", "-d", kernel],
                             capture_output=True, text=True, check=True).stdout
    except FileNotFoundError:
        print("kernel-vector-check: objdump not found — NOT CHECKED",
              file=sys.stderr)
        return 2
    except subprocess.CalledProcessError as e:
        print("kernel-vector-check: objdump failed on %s — NOT CHECKED: %s"
              % (kernel, e), file=sys.stderr)
        return 2

    symbol = "<none>"
    bad = []
    seen_any_instruction = False

    for line in out.splitlines():
        m = SYMBOL_LINE.match(line)
        if m:
            symbol = m.group(1)
            continue

        # An instruction line: address, tab, bytes, tab, mnemonic.
        if "\t" not in line:
            continue
        seen_any_instruction = True

        text = line.split("\t", 2)[-1]
        if not (VECTOR_OPERAND.search(text) or VECTOR_MNEMONIC.match(text)):
            continue
        if symbol in ALLOWED:
            continue
        bad.append((symbol, line.strip()))

    # 🔴 An empty answer from an instrument that could not run is not a pass.
    # This file's own project has paid for that one more than once: a grep that
    # matched nothing because it was pointed at the wrong path read exactly
    # like a grep that matched nothing because the tree was clean.
    if not seen_any_instruction:
        print("kernel-vector-check: no instructions found in %s — the check "
              "did NOT run, which is not the same as passing" % kernel,
              file=sys.stderr)
        return 2

    if bad:
        print("kernel-vector-check: VECTOR INSTRUCTIONS IN UNDECLARED KERNEL "
              "CODE (#561)\n", file=sys.stderr)
        for sym, line in bad[:40]:
            print("    %-32s %s" % (sym, line), file=sys.stderr)
        if len(bad) > 40:
            print("    ... and %d more" % (len(bad) - 40), file=sys.stderr)
        print("""
A kernel thread does not have its vector state carried across a context switch
(#561): it is exempt unless it declares itself with
context_needs_vector_state().  Code above executes vector instructions in a
thread that has not declared anything, so it is writing over whatever the
registers held -- which is some user thread's state, and nothing will report
it.

Either declare the thread, and add its symbol to ALLOWED in this script with
the reason, or do not use vector registers there.""", file=sys.stderr)
        return 1

    print("kernel-vector-check: no vector instructions outside the %d declared "
          "symbols (#561)" % len(ALLOWED))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
