#!/usr/bin/env python3
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# No floating-point instruction in the kernel except where one is declared
# (#561 on x86-64, #560 on i386).
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

    # ── i386 (#560) ───────────────────────────────────────────────────
    #
    # This target has no -mgeneral-regs-only: -mno-sse stops the compiler
    # reaching for SSE, and stops nothing at all from reaching for x87, which
    # is i386's default floating point.  So the check matters MORE here, and
    # it found something: _doprnt_ext held 124 floating-point instructions
    # formatting %f/%e/%g, executed in whatever thread had called printf.
    # Since the switch carries the state (#560), that thread's live x87
    # registers were in the unit, and the conversion overwrote them.  The
    # kernel is built KERNEL_FLOAT_OK=0 now and those instructions are gone.
    #
    # Everything below is i386/fpu.c, and every entry is the save or the
    # restore itself rather than something using the unit.
    "fpu_save_context": "the switch's save (XSAVE/FXSAVE), i386",
    "fpu_load_context": "the switch's restore (XRSTOR/FXRSTOR), i386",
    "fp_save": "save helper behind fpu_get_state and the error paths, i386",
    "fp_load": "restore helper, i386",
    "fpu_module_init": "builds the clean image by taking it FROM the unit "
                       "after fpinit(), i386",
    "fpu_get_state": "thread_get_state: saves so the caller can read, i386",
    "fpexterrflt": "saves the faulting status before raising, i386",
    "fpsseflt": "#XF: saves the faulting MXCSR before raising, i386 (#515)",
    "fpintr": "IRQ 13: saves the faulting state before the AST, i386",

    # i386/fpu_stress.S, run by the threads fpu_stress.c creates.  Every
    # thread on this target carries its state -- there is no exemption to
    # declare -- so what these need is only to be allowed to exist.
    "fpu_stress_load": "#560 test: writes the pattern into the unit",
    "fpu_stress_check": "#560 test: reads the SSE half back",
    "fpu_stress_unload": "#560 test: takes the x87 stack apart",
}

# What a floating-point instruction looks like in objdump's output: an operand
# naming a vector register, or one of the state-moving instructions by name.
VECTOR_OPERAND = re.compile(r"%(x|y|z)mm\d")
VECTOR_MNEMONIC = re.compile(
    r"^\s*(v?(mov(a|u)p[sd]|movdq[au]|movs[sd])|"
    r"xsave\w*|xrstor\w*|fxsave\w*|fxrstor\w*|fnsave\b|frstor\b|vzero\w+)\b")

# 🔴 x87, which the check did not know about and which is i386's DEFAULT
# floating point.  -mno-sse stops the compiler reaching for SSE and stops
# nothing from reaching for this, so a kernel built that way can be full of
# floating-point instructions while this check says nothing -- and was: 124 of
# them in _doprnt_ext (#560).
#
# Every x87 mnemonic starts with `f'.  The ones below only MANAGE the unit --
# they set the control word, read the status, clear exceptions, or wait -- and
# do not compute with or move the registers a thread's state lives in, so they
# are not what this check is about.  Everything else beginning with `f' is.
X87_MANAGEMENT = {
    "fninit", "finit", "fnstcw", "fstcw", "fldcw", "fnstsw", "fstsw",
    "fnclex", "fclex", "fwait", "fnstenv", "fstenv", "fldenv",
    # ⚠️ Not x87 at all: the FS segment prefix, which objdump prints as a
    # lone `fs' when the byte 0x64 turns up outside an instruction.  It does
    # when a string literal sits in .text -- locore.S's "interrupt end" ends
    # in `d', which is 0x64.  Data read as code, and the first thing this
    # check reported.
    "fs",
}
X87_MNEMONIC = re.compile(r"^\s*(f[a-z0-9]+)\b")

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
        #
        # 🔴 THREE fields, and the count is the check.  A long instruction's
        # byte dump wraps onto continuation lines that carry only the address
        # and more bytes -- and reading the last field of one of those gets
        # hex, where `ff', `fa' and `fe' are indistinguishable from x87
        # mnemonics.  Taking [-1] of a two-way split reported fourteen
        # floating-point instructions in idle_thread_continue and
        # mach_msg_overwrite_trap that were never there.
        parts = line.split("\t")
        if len(parts) < 3:
            continue
        seen_any_instruction = True

        text = parts[2]
        x87 = X87_MNEMONIC.match(text)
        if x87 and x87.group(1) in X87_MANAGEMENT:
            x87 = None
        if not (VECTOR_OPERAND.search(text) or VECTOR_MNEMONIC.match(text)
                or x87):
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
        print("kernel-vector-check: FLOATING-POINT INSTRUCTIONS IN UNDECLARED "
              "KERNEL CODE (#560/#561)\n", file=sys.stderr)
        for sym, line in bad[:40]:
            print("    %-32s %s" % (sym, line), file=sys.stderr)
        if len(bad) > 40:
            print("    ... and %d more" % (len(bad) - 40), file=sys.stderr)
        print("""
The code above executes floating-point instructions in a thread that did not
ask for the registers it is writing over.  What is in them is another thread's
state, and nothing will report the damage.

The two targets get there by different routes, so read whichever applies:

  x86-64 (#561): a thread of the kernel task does not have its vector state
  carried across a switch at all -- it is exempt unless it declares itself
  with context_needs_vector_state().  Code here that uses the registers
  without declaring corrupts whatever user thread's state they held.

  i386 (#560): every thread carries its state, so there is nothing to
  declare -- and that is the problem.  A thread that has trapped into the
  kernel has its OWN live registers in the unit, so kernel code computing in
  them overwrites that thread's state, and it returns to user mode wrong.
  This is what _doprnt_ext did with %f/%e/%g before KERNEL_FLOAT_OK went to
  zero.

  ⚠️ On i386 the compiler is held back by -mno-sse, which stops SSE and does
  NOT stop x87 -- i386's default floating point, and the half this check had
  to learn about.

Either arrange for the thread to own those registers and add its symbol to
ALLOWED in this script with the reason, or do not use them there.""",
              file=sys.stderr)
        return 1

    print("kernel-vector-check: no floating-point instructions outside the %d "
          "declared symbols (#560/#561)" % len(ALLOWED))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
