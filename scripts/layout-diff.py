#!/usr/bin/env python3
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
"""Compare what a header's structures actually look like on both targets (#415).

  scripts/layout-diff.py <header> <include dir> [<include dir> ...]

Use @arch@ where the per-target machine headers go, as the build does:

  scripts/layout-diff.py mach/message.h uros/uapi uros/src/mach_kernel @arch@

The audit's expensive half is the structures that cross a boundary -- a disk,
a wire, a shared page -- where both sides compile without complaint and only
the bytes disagree.  Scanning for suspicious type names finds some of it and
is a guess.  This asks the compiler where it put every field, for i386 and for
x86-64, and diffs the answers.

clang is the instrument here and not the compiler: the kernel is built with
gcc and nothing this prints ends up in it.  clang is used because it will hand
over its record layouts, which is a question gcc has no answer for.

Two things worth knowing before trusting the output:

  - The include path MUST differ per target, which is what @arch@ is for.
    Compiling at -m64 against the i386 machine headers leaves vm_offset_t
    thirty-two bits wide, and every answer involving it is then a fiction.

  - A difference is not automatically a defect.  mach/message.h differs by
    design (#413); the MIG *_info structures differ but travel with a count
    computed from their own size; cap_manifest_entry differs only in
    alignment while its fields and its size stay put.  This reports, and the
    classification is the reader's.
"""

import collections
import os
import re
import subprocess
import sys

ARCH = {32: 'uros/build/arch-include', 64: 'uros/build-x86_64/arch-include'}


def probe_names(path):
    """Which types to ask about.

    The guesswork is confined to this function; the answer comes from clang.
    Braces are counted rather than matched with a regex because `} d_un;'
    closes a union member and not a typedef, and taking it for a type name
    fails the whole measurement on a header that is perfectly fine.
    """
    txt = open(path, errors='replace').read()
    txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
    named, tdef = set(), set()
    for m in re.finditer(r'\b(typedef\s+)?(?:struct|union)\s+(\w+)?\s*\{', txt):
        is_typedef, name = bool(m.group(1)), m.group(2)
        depth, i = 1, m.end()
        while i < len(txt) and depth:
            if txt[i] == '{':
                depth += 1
            elif txt[i] == '}':
                depth -= 1
            i += 1
        if name and not is_typedef:
            named.add(name)
        if is_typedef:
            t = re.match(r'\s*\**\s*(\w+)\s*;', txt[i:])
            if t:
                tdef.add(t.group(1))
            elif name:
                named.add(name)
    return sorted(named), sorted(tdef)


def layouts(header, incs, bits, names):
    """Record layouts as the compiler computes them for this target."""
    src = '#include <%s>\n' % header
    # sizeof, not a bare declaration: clang prints the layout only of records
    # whose layout is actually demanded.
    for i, n in enumerate(names[0]):
        src += 'unsigned long __p%d = sizeof(struct %s);\n' % (i, n)
    for i, n in enumerate(names[1]):
        src += 'unsigned long __t%d = sizeof(%s);\n' % (i, n)

    cmd = ['clang', '-m%d' % bits, '-fsyntax-only',
           '-Xclang', '-fdump-record-layouts',
           '-nostdlibinc', '-ffreestanding', '-w']
    for i in incs:
        cmd += ['-I', i.replace('@arch@', ARCH[bits])]

    # The SAME path for both runs: anonymous records are named "(unnamed at
    # path:line:col)", so a different path would make every one of them differ
    # between the targets and the measurement would report a lie.
    tmp = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.layout-probe.c')
    open(tmp, 'w').write(src)
    p = subprocess.run(cmd + [tmp], capture_output=True, text=True)
    os.unlink(tmp)

    # A record's name is the line right AFTER the dump header.  Recognising it
    # by shape instead picks up the fields too, which have the same shape:
    # "<offset> | <text>".
    out, cur, expect = collections.OrderedDict(), None, False
    for line in p.stdout.split('\n'):
        if 'Dumping AST Record Layout' in line:
            expect = True
            continue
        if expect:
            m = re.match(r'\s*\d+ \| (?:(?:struct|union|class)\s+)?(\S.*?)\s*$', line)
            if m:
                cur = m.group(1)
                out.setdefault(cur, [])
                expect = False
            continue
        if cur is None:
            continue
        m = re.match(r'\s*\| \[sizeof=(\d+), align=(\d+)\]', line)
        if m:
            out[cur].append(('size', int(m.group(1)), int(m.group(2))))
            cur = None
            continue
        m = re.match(r'\s*(\d+)(?::(\d+))? \|\s+(.*\S)\s*$', line)
        if m:
            out[cur].append((int(m.group(1)), m.group(2) or '', m.group(3)))
    return out, p.stderr


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    header, incs = sys.argv[1], sys.argv[2:]

    path = None
    for i in incs:
        cand = os.path.join(i.replace('@arch@', ARCH[32]), header)
        if os.path.exists(cand):
            path = cand
            break
    names = probe_names(path) if path else ((), ())

    a, err_a = layouts(header, incs, 32, names)
    b, _ = layouts(header, incs, 64, names)
    if not a and not b:
        first = (err_a or '').strip().split('\n')
        print('  [??? ] %s: does not compile (%s)'
              % (header, first[0][:90] if first else '?'))
        return 1

    both = sorted(set(a) | set(b))
    differ = [n for n in both if a.get(n) != b.get(n)]
    print('  [%s] %s: %d records, %d differ'
          % ('DIFF' if differ else 'SAME', header, len(both), len(differ)))
    for n in differ:
        sa = next((x for x in a.get(n, []) if x[0] == 'size'), None)
        sb = next((x for x in b.get(n, []) if x[0] == 'size'), None)
        print('        %-34s m32=%-6s m64=%s'
              % (n, sa[1] if sa else '?', sb[1] if sb else '?'))
        for x, y in zip(a.get(n, []), b.get(n, [])):
            if x != y and x[0] != 'size':
                print('            %-40s vs %s' % (x[2], y[2]))
    return 1 if differ else 0


sys.exit(main())
