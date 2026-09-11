#!/usr/bin/env python3
#
# mig-array-bounds-check.py — a MIG array bound is a measurement, not a number
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ── Why this exists ──────────────────────────────────────────────────────
#
# The .defs bound an inline array like this:
#
#     type task_info_t = array[*:8] of integer_t;
#
# and MIG's generated stub caps the count it sends at that bound.  The number
# is really the size of the widest struct the array has to carry, counted in
# words — a MEASUREMENT, written as a literal, with a comment beside it saying
# to revisit it "if other flavors are added".
#
# 🔴 WHAT BROKE THEM WAS NOT A NEW FLAVOR BUT A NEW WORD SIZE.  On x86-64
# vm_offset_t and time_value_t are eight bytes, so the structs grew and three
# bounds went stale:
#
#   task_info_t       needed 12, allowed 8   -- task_info(TASK_BASIC_INFO) had
#                                               never once worked through the
#                                               message path on this target
#   vm_region_info_t  needed 10, allowed 8   -- same, found by #548's sweep
#   policy_info_t     needed  5, allowed 2   -- found by this check, before
#                                               anything called it
#
# 🔑 The first two were found by something failing.  The third was found by
# counting, which is the difference this script exists to make.  And five more
# bounds are exactly at their limit right now: correct today, and silently
# wrong the moment any field in those structs grows.
#
# ⚠️ It compiles a probe against this tree's own uapi headers rather than
# parsing C by hand.  Struct layout is the compiler's answer, not a thing to
# recompute: padding and alignment are where every one of these went wrong.
#
# Uso:
#   scripts/mig-array-bounds-check.py [radice]
# Esce 1 se un limite è troppo stretto.

import re
import subprocess
import sys
import tempfile
from pathlib import Path

# type NAME = array[*:N] of TYPE;
BOUND = re.compile(r'type\s+(\w+)\s*=\s*array\[\*:(\d+)\]\s*of\s+(\w+)\s*;')

# Which COUNT macros belong to which array type, and the header that declares
# them.  🔑 Written out rather than guessed: a name-matching rule would have to
# know that VM_REGION_BASIC_INFO_COUNT belongs to vm_region_info_t but
# HOST_BASIC_INFO_COUNT belongs to host_info_t, and would be wrong the first
# time somebody names a flavor differently.
FLAVORS = {
    'task_info_t':          ['TASK_BASIC_INFO_COUNT'],
    'thread_info_t':        ['THREAD_BASIC_INFO_COUNT'],
    'vm_region_info_t':     ['VM_REGION_BASIC_INFO_COUNT'],
    'host_info_t':          ['HOST_BASIC_INFO_COUNT', 'HOST_SCHED_INFO_COUNT',
                             'HOST_CPU_LOAD_INFO_COUNT',
                             'HOST_PRIORITY_INFO_COUNT'],
    'mach_port_info_t':     ['MACH_PORT_RECEIVE_STATUS_COUNT',
                             'MACH_PORT_LIMITS_INFO_COUNT'],
    'memory_object_info_t': ['MEMORY_OBJECT_ATTR_INFO_COUNT',
                             'MEMORY_OBJECT_BEHAVE_INFO_COUNT',
                             'MEMORY_OBJECT_PERF_INFO_COUNT'],
    'processor_info_t':     ['PROCESSOR_BASIC_INFO_COUNT',
                             'PROCESSOR_CPU_LOAD_INFO_COUNT'],
    'processor_set_info_t': ['PROCESSOR_SET_BASIC_INFO_COUNT',
                             'PROCESSOR_SET_LOAD_INFO_COUNT'],
    'policy_base_t':        ['POLICY_TIMESHARE_BASE_COUNT'],
    'policy_info_t':        ['POLICY_TIMESHARE_INFO_COUNT'],
    'policy_limit_t':       ['POLICY_TIMESHARE_LIMIT_COUNT'],
}

HEADERS = """
#include <stdio.h>
#include <mach/mach_types.h>
#include <mach/task_info.h>
#include <mach/thread_info.h>
#include <mach/host_info.h>
#include <mach/vm_region.h>
#include <mach/port.h>
#include <mach/policy.h>
#include <mach/processor_info.h>
#include <mach/memory_object.h>
"""


def bounds_from_defs(root: Path):
    out = {}
    for f in sorted((root / 'uros/uapi').rglob('*.defs')):
        for m in BOUND.finditer(f.read_text(errors='replace')):
            out[m.group(1)] = int(m.group(2))
    return out


def measure(root: Path, bits: str):
    """Ask the compiler what every flavor actually needs."""
    src = [HEADERS, 'int main(void){']
    for ty, macros in FLAVORS.items():
        for mac in macros:
            src.append(f'#ifdef {mac}')
            src.append(f'  printf("{ty} {mac} %zu\\n", (size_t){mac});')
            src.append('#endif')
    src.append('  return 0; }')

    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / 'probe.c'
        exe = Path(d) / 'probe'
        c.write_text('\n'.join(src))
        cmd = ['cc', bits, '-I', str(root / 'uros/uapi'),
               '-I', str(root / 'uros/build-x86_64/arch-include'),
               '-o', str(exe), str(c)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            return None, r.stderr.strip().splitlines()[:3]
        out = subprocess.run([str(exe)], capture_output=True, text=True)
        got = {}
        for line in out.stdout.splitlines():
            ty, mac, n = line.split()
            got.setdefault(ty, []).append((mac, int(n)))
        return got, None


def main(root: Path) -> int:
    bounds = bounds_from_defs(root)
    got, err = measure(root, '-m64')
    if got is None:
        # 🔴 A probe that will not build is not a pass.  Saying so beats
        # reporting nothing and letting every bound go unchecked in silence.
        print('mig-array-bounds: the probe did not compile — nothing was '
              'checked:', file=sys.stderr)
        for line in err or []:
            print('   ', line, file=sys.stderr)
        return 2

    bad, tight = [], []
    for ty, entries in sorted(got.items()):
        b = bounds.get(ty)
        if b is None:
            continue
        for mac, n in entries:
            if n > b:
                bad.append((ty, mac, n, b))
            elif n == b:
                tight.append((ty, mac, n))

    print(f'x86-64: {sum(len(v) for v in got.values())} flavor(s) against '
          f'{len(got)} array bound(s)')
    if bad:
        print('\n🔴 THE BOUND IS TOO SMALL — the stub caps the count and the '
              'kernel refuses the call:')
        for ty, mac, n, b in bad:
            print(f'  {ty}: {mac} needs {n}, the .defs allows {b}')
    if tight:
        print('\n⚠️ Exactly at the limit — correct today, silently wrong the '
              'moment a field grows:')
        for ty, mac, n in tight:
            print(f'  {ty}: {mac} needs {n}, the .defs allows {n}')
    if not bad:
        print('\n  no bound is too small')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()))
