#!/usr/bin/env python3
#
# trap-rpc-parity.py — does every kernel trap check what the RPC checks? (#543)
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ── Why this exists ──────────────────────────────────────────────────────
#
# libmach carries trap fast paths that nothing calls, because the ms_*.c layer
# that called them was never compiled.  Before any of them is switched on, one
# thing has to be true and has to be shown rather than assumed:
#
# 🔴 A TRAP MUST NOT REACH THE KERNEL BY A SHORTER ROUTE THAN THE MESSAGE.
# #511 made device and configuration-space access a right that is checked.  If
# a trap skips a check the RPC makes, using it reopens #511 while looking like
# an optimisation -- which is the worst shape a regression can have.
#
# 🔑 What the check IS, in this kernel, is the port-name resolution.  On the
# message path the IPC machinery translates the destination port before the
# demux ever runs, so a name the caller does not hold never arrives.  A trap
# gets a raw name and must do the same translation itself:
#
#     space = port_name_to_space(task);
#     if (space == IS_NULL)
#             return MACH_SEND_INTERRUPTED;
#
# The failure return is not an error code, it is the "not mine" signal the
# userland wrapper falls back to the RPC on.  So a trap is sound when it
# (a) converts every port argument, and (b) says MACH_SEND_INTERRUPTED when a
# conversion fails.  A trap that takes a port and never converts it is the one
# to find.
#
# ⚠️ THIS IS A CENSUS, NOT A PROOF.  It reads for a pattern; it cannot tell
# that a conversion is the RIGHT one, nor that the routine underneath is the
# same one the RPC reaches.  It narrows 97 traps down to the ones a person must
# read.  Anything it reports as clean still needs its convergence checked by
# hand before that trap is used -- which is why it prints what each one calls.
#
# Uso:
#   scripts/trap-rpc-parity.py [radice]

import re
import sys
from pathlib import Path

TRAP_TABLE = 'uros/src/mach_kernel/kern/syscall_sw.c'
IMPL = 'uros/src/mach_kernel/kern/ipc_mig.c'

PLACEHOLDERS = {'not_implemented', 'kern_invalid', 'null_port'}

# ── What counts as resolving a port, and why this is not a list of names ──
#
# 🔴 THIS WAS A LIST OF FUNCTION NAMES AND IT WAS WRONG FOUR TIMES.  Each time
# a trap was reported as checking nothing, and each time the trap was fine and
# the list was short: ipc_object_copyin (syscall_vm_remap),
# ipc_port_translate_send (syscall_thread_switch), and the raw
# `current_act()->task->itk_space' of mach_msg_overwrite_trap, which does not
# call a conversion helper because it IS the path that converts.
#
# 🔑 So what is looked for is the PROPERTY and not the spelling: does this trap
# reach the CALLER'S SPACE?  A name is only a name until it is resolved against
# a space, and every honest form of that resolution has to mention one.  A new
# helper invented tomorrow still will.
SPACE = re.compile(r'\bcurrent_space\s*\(|\bitk_space\b|'
                   r'\bport_name_to_\w+\s*\(|\bconvert_port_to_\w+\s*\(|'
                   r'\bipc_object_copyin\s*\(|\bipc_port_translate\w*\s*\(')
CONVERT = SPACE
FALLBACK = re.compile(r'\bMACH_SEND_INTERRUPTED\b')
# A port-shaped argument: if a trap takes one of these it has something to
# resolve.  Deliberately generous -- a false "has a port" costs a read, a
# missed one costs a hole.
PORTISH = re.compile(r'\b(mach_port_t|ipc_port_t|task_port_t|thread_port_t|'
                     r'device_port_t|host_port_t|mach_port_name_t)\b')


def trap_names(root: Path):
    text = (root / TRAP_TABLE).read_text(errors='replace')
    names = []
    # ⚠️ MACH_TRAP_STACK TOO, and missing it was not cosmetic: clock_sleep_trap
    # is one of the seven, and it is the one entry in the table that
    # dereferences a converted port without checking it first.  A matcher that
    # cannot see a form proves nothing about that form, and the seven it could
    # not see included mach_msg_overwrite_trap and urmach_msg.
    for m in re.finditer(r'MACH_TRAP(?:_STACK)?\(\s*([A-Za-z_]\w*)', text):
        n = m.group(1)
        if n not in PLACEHOLDERS and n not in names:
            names.append(n)
    return names


def bodies(root: Path):
    """Map function name -> body text, across the whole kernel.

    ⚠️ It used to read ipc_mig.c alone, and reported 35 traps as "defined
    elsewhere" -- which reads as a finding and is really a gap.  A census that
    covers two thirds of its subject answers the question badly rather than
    partly: the traps it did not look at are exactly the ones nobody has
    looked at.
    """
    text = ''
    for p in sorted((root / 'uros/src/mach_kernel').rglob('*.c')):
        text += p.read_text(errors='replace') + '\n'
    out = {}
    for m in re.finditer(r'^([A-Za-z_]\w*)\s*\(', text, re.M):
        name = m.group(1)
        start = m.start()
        depth, i, seen = 0, text.find('{', start), False
        if i < 0:
            continue
        while i < len(text):
            if text[i] == '{':
                depth += 1
                seen = True
            elif text[i] == '}':
                depth -= 1
                if seen and depth == 0:
                    break
            i += 1
        out[name] = text[start:i + 1]
    return out


def calls_in(body: str, own: str):
    """Kernel routines this body calls, minus the noise."""
    skip = {own, 'if', 'while', 'for', 'switch', 'return', 'sizeof', 'panic',
            'copyout', 'copyin'}
    seen = []
    for m in re.finditer(r'\b([a-z_]\w*)\s*\(', body):
        n = m.group(1)
        if n in skip or CONVERT.match(n + '(') or n.startswith('port_name_to'):
            continue
        if n not in seen:
            seen.append(n)
    return seen


def main(root: Path) -> int:
    names = trap_names(root)
    impl = bodies(root)

    clean, notfb, suspect, absent = [], [], [], []

    for n in names:
        body = impl.get(n)
        if body is None:
            absent.append(n)
            continue
        has_port = bool(PORTISH.search(body.split('{', 1)[0]))
        converts = bool(CONVERT.search(body))
        falls_back = bool(FALLBACK.search(body))
        if not has_port:
            clean.append((n, 'takes no port', calls_in(body, n)[:3]))
        elif converts and falls_back:
            clean.append((n, 'converts + falls back', calls_in(body, n)[:3]))
        elif converts:
            # 🔑 SAFE IS NOT THE SAME AS USABLE, and this class is the reason
            # the script reports three outcomes instead of two.
            # syscall_clock_get_time converts its port properly and
            # clock_get_time refuses CLOCK_NULL, so nothing is reachable that
            # should not be -- and wiring it up would still be wrong.  Returning
            # an error where the message path would have carried the call turns
            # a case Mach handles (a port that is not this kernel's object --
            # interposed, proxied, remote) into a failure.  The fallback is not
            # politeness; it is what keeps a trap from narrowing the system.
            notfb.append((n, 'converts, but answers an error instead of '
                             'MACH_SEND_INTERRUPTED', calls_in(body, n)[:3]))
        else:
            # 🔑 A trap may resolve by DELEGATION: urmach_msg's whole body is a
            # call to mach_msg_overwrite_trap, which reaches the caller's space
            # itself.  Reporting it as checking nothing is true of its text and
            # false of its behaviour, and a report that is true of the text is
            # not what anybody wants from this.
            via = [c for c in calls_in(body, n)
                   if c in impl and SPACE.search(impl[c])]
            if via:
                clean.append((n, f'resolves via {via[0]}()',
                              calls_in(body, n)[:3]))
            else:
                suspect.append((n, 'NO conversion of a port argument',
                                calls_in(body, n)[:3]))

    print(f'traps in the table (placeholders removed): {len(names)}')
    print(f'  defined in the kernel: {len(names) - len(absent)}')
    print(f'  no definition found: {len(absent)}')
    print()

    if suspect:
        print('🔴 DANGEROUS UNTIL READ — takes a port and does not resolve it:')
        for n, why, c in suspect:
            print(f'  {n}\n      {why}\n      calls: {", ".join(c) or "-"}')
        print()

    if notfb:
        print('⚠️ SAFE BUT NOT A CANDIDATE — would turn a handled case into an '
              'error:')
        for n, why, c in notfb:
            print(f'  {n}\n      {why}\n      calls: {", ".join(c) or "-"}')
        print()

    print(f'candidates: {len(clean)}  ·  safe-not-candidate: {len(notfb)}'
          f'  ·  dangerous: {len(suspect)}')
    if absent:
        print('\nno definition found anywhere in the kernel:')
        for n in absent:
            print(f'  {n}')

    # ⚠️ Exit 0 even with suspects.  This is a census that tells a person where
    # to read, not a gate: failing the build on it would say the question has
    # been answered, and it has not.
    return 0


if __name__ == '__main__':
    sys.exit(main(Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()))
