#!/usr/bin/env python3
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
#
"""header-shadow-check -- fail when a translation unit can reach two different
files at the same include-relative path (#481).

Two headers of the same name are not a problem until one target's -I list can
find both, because then which one it compiles against is decided by the ORDER
of its flags -- and nothing reports the choice.  That is what #471 found in the
.defs, #480 in sa_mach/types.h, and #481 in export/include: the same shape
three times, each time discovered by a symptom somewhere else.

So the question this asks is not "does the tree contain duplicate filenames".
It is the one that matters: for each translation unit the build actually
compiles, is there a relative path that two of its own include roots both hold?
It asks the build, via compile_commands.json, rather than reading CMake.

Usage:
    scripts/header-shadow-check.py [build-dir ...]        # default: both
    scripts/header-shadow-check.py --list                 # report, never fail

Exit status is 1 when a shadowed path appears that is not in ACCEPTED below,
and 0 otherwise.  A new entry in ACCEPTED is a decision, and it needs the
sentence saying why -- that is the point of keeping the list here rather than
silencing the check.
"""

import collections
import hashlib
import json
import os
import shlex
import sys

# ---------------------------------------------------------------------------
# The shadows that remain, and why each one is not the defect this looks for.
#
# 🔑 Everything here is a case where two files of one name are REACHABLE and
# that is intended.  Nothing here is "we did not get to it yet" -- those belong
# in an issue, not in an allowlist.
# ---------------------------------------------------------------------------
ACCEPTED = {
    # musl against sa_mach.  The widths agree since #480 -- sa_mach declares a
    # libc type only when nothing else has, and with musl's spelling -- so which
    # one a unit reaches no longer changes what it computes.  A target that
    # links musl gets musl's; a freestanding Mach server gets sa_mach's.
    "math.h", "setjmp.h", "signal.h", "stdarg.h", "stdio.h", "stdlib.h",
    "string.h", "strings.h", "sys/ioctl.h", "sys/syslog.h", "sys/time.h",
    "sys/types.h", "scsi/scsi.h", "pthread.h", "machine/va_list.h",
    "ctype.h", "errno.h", "float.h",

    # A private header of one component whose name is generic enough that
    # another component chose it too.  Each is included by quoted name from
    # its own directory, which searches that directory first, so no -I order
    # reaches across.  ⚠️ This holds only while they stay quoted: an angled
    # <externs.h> from any of the three would pick by flag order.
    "externs.h", "defs.h", "i386/asm.h", "kkt.h",

    # The per-architecture halves of one header: exactly one of the two
    # directories is ever on a given target's include path, chosen by
    # UROS_TARGET_ARCH.  bootstrap's CMakeLists carries the scar from the day
    # AT386 was hardcoded and x86_64 got i386's lock width.
    "pthread_machdep.h", "misc_protos.h",

    # MIG output generated twice into two private directories, because two
    # programs demultiplex the same subsystem and each keeps its own stubs.
    # Both are produced from the same .defs by the same migcom in the same
    # configure, so they cannot disagree without the .defs disagreeing with
    # itself.  ⚠️ This is only true while neither is CHECKED IN -- a versioned
    # copy of MIG output is what #481 removed, and it would land right here.
    "notify.h", "notify_server.h", "service.h", "device_server.h",
    "memory_object.h", "memory_object_default.h", "default_pager_object.h",

    # default_pager's own types.h, which is its private struct definitions and
    # not a libc header despite the name.  Reachable beside sa_mach/types.h
    # only from default_pager itself, which includes it by quoted name.
    "types.h",
}

# ---------------------------------------------------------------------------
# Found by this check, not yet settled, and tracked in #481.  These are NOT
# accepted -- they are the same defect as the rest of the issue, in places the
# first pass did not reach -- but they predate the check, so failing on them
# would only mean the check gets disabled.  It reports them and passes; the
# list must shrink, never grow.
#
# 🔑 Four of them are one shape: MIG names its user header after the .defs, and
# a server's own header is named after the same subsystem.  That is exactly the
# <mach.h> collision this issue fixed in mig_user_header_name(), for
# bootstrap.defs, exc.defs, char_server.defs and gpu_server.defs.
# ---------------------------------------------------------------------------
RECORDED = {
    "bootstrap.h", "exc.h", "char_server.h", "gpu_server.h",
    # migcom carries frozen copies of four Mach headers so it can be built
    # before the tree it generates for.  Whether that is still true is the
    # question; that it disagrees with uapi is not in doubt.
    "mach/boolean.h", "mach/kern_return.h", "mach/message.h",
    # generated against hand-written, one name each.
    "mach/sync.h", "machine/types.h",
    # the kernel's own cpuid.h against the compiler's, on 193 units.
    "cpuid.h",
}


def include_roots(command, directory):
    """The -I and -isystem roots of one compile command, in order."""
    args = shlex.split(command)
    roots, i = [], 0
    while i < len(args):
        a = args[i]
        if a.startswith("-I") and len(a) > 2:
            roots.append(a[2:])
        elif a == "-I":
            roots.append(args[i + 1]); i += 1
        elif a.startswith("-isystem") and len(a) > 8:
            roots.append(a[8:])
        elif a == "-isystem":
            roots.append(args[i + 1]); i += 1
        i += 1
    return [r if os.path.isabs(r) else os.path.normpath(os.path.join(directory, r))
            for r in roots]


_index_cache = {}


def index_of(root):
    """relative path -> absolute path, for every header under one root."""
    if root not in _index_cache:
        m = {}
        if os.path.isdir(root):
            for dirpath, _dirs, files in os.walk(root):
                for f in files:
                    if f.endswith((".h", ".defs")):
                        p = os.path.join(dirpath, f)
                        m[os.path.relpath(p, root)] = p
        _index_cache[root] = m
    return _index_cache[root]


_digest_cache = {}


def digest(path):
    if path not in _digest_cache:
        with open(path, "rb") as fh:
            _digest_cache[path] = hashlib.md5(fh.read()).hexdigest()
    return _digest_cache[path]


def scan(build_dirs):
    """relative path -> (set of distinct files, set of translation units)."""
    shadows = collections.defaultdict(lambda: (set(), set()))
    for build in build_dirs:
        cc = os.path.join(build, "compile_commands.json")
        if not os.path.exists(cc):
            print("skip %s: no compile_commands.json" % build, file=sys.stderr)
            continue
        for entry in json.load(open(cc)):
            roots = include_roots(entry["command"], entry["directory"])
            seen = {}
            for root in roots:
                for rel, path in index_of(root).items():
                    seen.setdefault(rel, []).append(path)
            for rel, paths in seen.items():
                real = []
                for p in paths:
                    rp = os.path.realpath(p)
                    if rp not in real:
                        real.append(rp)
                if len(real) < 2:
                    continue
                files, units = shadows[rel]
                files.update(real)
                units.add(entry["file"])
    return shadows


def main():
    argv = [a for a in sys.argv[1:] if not a.startswith("--")]
    listing = "--list" in sys.argv
    here = os.path.dirname(os.path.abspath(__file__))
    builds = argv or [os.path.join(here, "..", "uros", "build"),
                      os.path.join(here, "..", "uros", "build-x86_64")]

    shadows = scan(builds)
    unexpected = sorted(r for r in shadows
                        if r not in ACCEPTED and r not in RECORDED)
    recorded = sorted(r for r in shadows if r in RECORDED)

    if listing or unexpected:
        rows = sorted(shadows) if listing else unexpected
        for rel in rows:
            files, units = shadows[rel]
            differ = len({digest(f) for f in files}) > 1
            print("%-9s %-34s %d files, %d units"
                  % ("DIVERGE" if differ else "same", rel, len(files), len(units)))
            for f in sorted(files):
                print("              %s" % f)

    if unexpected:
        print("\n%d shadowed include path(s) that nothing accounts for.\n"
              "Each is a header two of one unit's own -I roots both hold, so which\n"
              "one it compiles against is decided by flag order and reported by\n"
              "nothing.  Remove the second copy, or add it to ACCEPTED in this\n"
              "script with the sentence that says why it is reachable on purpose."
              % len(unexpected), file=sys.stderr)
        return 1

    if not listing:
        print("no new shadowed headers: %d accepted, %d recorded and still open"
              % (len(shadows) - len(recorded), len(recorded)))
        if recorded:
            print("still open (#481): %s" % ", ".join(recorded))
    return 0


if __name__ == "__main__":
    sys.exit(main())
