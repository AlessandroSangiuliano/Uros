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


# ── Does the trap convert to the SAME thing the RPC converts to? (#543) ───
#
# 🔴 THE CHECK ABOVE MISSED A PRIVILEGE ESCALATION AND THIS ONE EXISTS BECAUSE
# OF IT.  syscall_vm_wire converted its host argument with port_name_to_host,
# which accepts IKOT_HOST *or* IKOT_HOST_PRIV, while mach_host.defs declares
# that argument `host_priv_t' -- and MIG's server side converts host_priv_t with
# convert_port_to_host_priv, which takes only the privileged port.  So the trap
# accepted a credential its own RPC refuses, and every task holds the weaker
# one.  syscall_host_statistics had it too: both routines declaring host_priv_t
# and having a trap were wrong, two out of two.
#
# 🔑 The authority for "what should this convert to" is not a table written
# here.  It is the `intran:' clause on the type in the .defs -- the same
# declaration MIG's server side is generated from.  Reading it means this check
# cannot disagree with the RPC, because it is asking the RPC.
#
# ⚠️ The trap spells it port_name_to_FOO where the server side says
# convert_port_to_FOO.  The two names are the same fact, so either satisfies the
# requirement; anything else is the finding.

# ⚠️ NOT ONE REGEX OVER THE FILE.  The first version was
#   type\s+(\w+)\s*=.*?intran:  with re.S
# and `.*?' happily crossed from one type declaration into the next, pairing
# `recnum_t' with dev_port_lookup() and `thread_state_flavor_t' with
# convert_port_to_map().  Every one of those was a false alarm, and a check
# that cries wolf is a check people learn to skip.  A type declaration ends at
# its `;', so the text is split there first and each piece asked on its own.
TYPE_NAME = re.compile(r'\btype\s+(\w+)\s*=')
INTRAN_IN = re.compile(r'intran:\s*\w+\s+(\w+)\s*\(')
# routine NAME( arg : type; ... );
ROUTINE = re.compile(r'^\s*(?:simpleroutine|routine)\s+(\w+)\s*\((.*?)\)\s*;',
                     re.S | re.M)


def defs_files(root: Path):
    return sorted((root / 'uros/uapi').rglob('*.defs'))


def intran_map(root: Path):
    """MIG type name -> the converter its server side uses."""
    out = {}
    for f in defs_files(root):
        for chunk in f.read_text(errors='replace').split(';'):
            nm = TYPE_NAME.search(chunk)
            it = INTRAN_IN.search(chunk)
            if nm and it:
                out[nm.group(1)] = it.group(1)
    return out


def routine_port_types(root: Path):
    """routine name -> list of declared argument type names."""
    out = {}
    for f in defs_files(root):
        for m in ROUTINE.finditer(f.read_text(errors='replace')):
            args = []
            for part in m.group(2).split(';'):
                if ':' not in part:
                    continue
                lhs, ty = part.split(':', 1)
                # ⚠️ `out' AND `inout' ARGUMENTS ARE RESULTS, NOT INPUTS, and
                # counting them was pure noise: thread_create's second argument
                # is `out child_act : thread_act_t', so the report accused the
                # trap of not converting a port it is supposed to PRODUCE.  MIG
                # translates those with outtran, in the other direction.
                if re.search(r'\b(out|inout)\b', lhs):
                    continue
                ty = re.split(r'[,\s]', ty.strip(), 1)[0]
                args.append(ty)
            out.setdefault(m.group(1), args)
    return out


IKOT = re.compile(r'\bIKOT_[A-Z0-9_]+\b')
HELPER = re.compile(r'\b(port_name_to_\w+|convert_port_to_\w+|\w+_lookup)\s*\(')


CALLS = re.compile(r'\b([a-z_]\w*)\s*\(')


def kinds_of(fn: str, impl, depth: int = 3, seen=None) -> set:
    """
    The IKOT_* kinds a conversion helper is willing to accept.

    ⚠️ FOLLOWS DELEGATION, because most converters do not do the test
    themselves.  convert_port_to_task() names no IKOT_* at all: it calls
    ref_task_port_locked(), and the kind check is in there.  Reading one level
    only would have left 41 of 55 arguments unjudged and called that an answer.
    """
    if seen is None:
        seen = set()
    if fn in seen or depth <= 0:
        return set()
    seen.add(fn)
    body = impl.get(fn)
    if not body:
        return set()
    kinds = set(IKOT.findall(body))
    if kinds:
        return kinds
    for callee in CALLS.findall(body):
        if callee == fn or callee in ('if', 'while', 'for', 'return',
                                      'sizeof', 'assert'):
            continue
        kinds |= kinds_of(callee, impl, depth - 1, seen)
    return kinds


def check_converters(root: Path, names, impl) -> int:
    """
    🔴 NOT A COMPARISON OF NAMES.  The first version of this compared the helper
    the trap calls against the one the .defs names in `intran:', and it reported
    syscall_device_read as wrong: the .defs says dev_port_lookup() and the trap
    calls port_name_to_device().  Those are the same check -- both accept
    IKOT_DEVICE and take a reference -- differing only in that the trap also
    translates the name, which on the message path the IPC layer has already
    done.  A name table would need an entry for every such pair and would be
    wrong again the next time somebody adds a helper.

    🔑 So what is compared is what each side ACCEPTS.  A trap must not accept a
    port kind its RPC twin would refuse, and that is exactly the defect this
    check was written after: syscall_vm_wire took IKOT_HOST or IKOT_HOST_PRIV
    where convert_port_to_host_priv takes only the second, so any task's
    mach_host_self() got in.

    ⚠️ Still textual, still a census.  It reads which IKOT_* names appear in each
    helper's body; it cannot see logic that rejects a kind some other way.  What
    it does is make the vm_wire class of defect impossible to keep, which is
    more than reading 104 traps by hand achieved.
    """
    intran = intran_map(root)
    routines = routine_port_types(root)
    bad = []
    checked = 0
    unjudged = []

    for trap in names:
        if not trap.startswith('syscall_') or trap not in impl:
            continue
        rpc = trap[len('syscall_'):]
        if rpc not in routines:
            continue
        body = impl[trap]
        helpers = set(HELPER.findall(body))

        for ty in routines[rpc]:
            conv = intran.get(ty)
            if not conv or conv == 'null_conversion':
                continue
            wanted = kinds_of(conv, impl)
            if not wanted:
                # 🔴 SAID OUT LOUD, NOT SKIPPED.  The intran's body names no
                # IKOT_*, so this argument cannot be judged here -- and a count
                # of what was checked, with no count of what was not, reads as
                # coverage it does not have.  That mistake cost this file a
                # rewrite once already.
                unjudged.append((trap, ty, conv))
                continue
            checked += 1

            # Any helper this trap calls that stays within what the RPC allows?
            ok = False
            widest = None
            for h in helpers:
                direct = set(IKOT.findall(impl.get(h, '')))
                got = direct or kinds_of(h, impl)
                if not got:
                    continue
                if got <= wanted:
                    ok = True
                    break
                if got & wanted:
                    # 🔑 TWO STRENGTHS OF EVIDENCE, and conflating them makes
                    # the strong one unbelievable.  When the helper names both
                    # kinds ITSELF, it plainly accepts both -- that is
                    # port_name_to_host with IKOT_HOST and IKOT_HOST_PRIV, and
                    # it is a finding.  When the extra kind only turns up by
                    # following what the helper calls, it may belong to an
                    # unrelated path: port_name_to_subsystem reaches IKOT_NONE
                    # through its fallback, and reporting that as an escalation
                    # is how a check loses its authority.
                    widest = (h, got, bool(direct))
            if not ok and widest is not None:
                bad.append((trap, ty, conv, sorted(wanted), widest))

    print()
    print(f'── what each side accepts: {checked} converted argument(s) ──')
    strong = [b for b in bad if b[4][2]]
    weak = [b for b in bad if not b[4][2]]

    if strong:
        print('🔴 THE TRAP ACCEPTS A PORT KIND ITS RPC TWIN REFUSES:')
        for trap, ty, conv, wanted, (h, got, _) in strong:
            print(f'  {trap}')
            print(f'      argument declared {ty}; {conv}() accepts '
                  f'{", ".join(wanted)}')
            print(f'      {h}() names {", ".join(sorted(set(got) - set(wanted)))}'
                  f' itself, so it takes that too')
    if weak:
        print('⚠️ WORTH A READ — the extra kind appears only through what the '
              'helper calls, which may be an unrelated path:')
        for trap, ty, conv, wanted, (h, got, _) in weak:
            print(f'  {trap}: {ty} — {conv}() accepts {", ".join(wanted)}, '
                  f'{h}() reaches {", ".join(sorted(set(got) - set(wanted)))}')
    if not bad:
        print('  no trap accepts a kind its RPC twin would refuse')
    if unjudged:
        print(f'\n  ⚠️ {len(unjudged)} argument(s) NOT judged — the converter'
              f' the .defs names does not test an IKOT_* kind in its own body,'
              f' so this check has nothing to compare:')
        for trap, ty, conv in unjudged:
            print(f'       {trap}: {ty} via {conv}()')
    return len(bad)


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

    check_converters(root, names, impl)

    # ⚠️ Exit 0 even with suspects.  This is a census that tells a person where
    # to read, not a gate: failing the build on it would say the question has
    # been answered, and it has not.
    return 0


if __name__ == '__main__':
    sys.exit(main(Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()))
