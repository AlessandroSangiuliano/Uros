#!/usr/bin/env python3
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Does a structure that crosses a boundary have the same layout on both
# targets? (#415)
#
# ── Why this is not a reading exercise ────────────────────────────────
#
# #415's cheap half is the compiler's: pointer casts and format strings
# announce themselves once the warnings are on. Its expensive half is the one
# no warning reaches -- a structure that a task, a server or a disk reads back
# from bytes the kernel wrote, whose members moved because a member widened.
# Nothing fails to compile. Nothing warns. The field is simply read from the
# wrong offset, on one target only, by code that was never rebuilt against the
# other.
#
# So it is asked of the BUILD rather than of the source. Both kernels are
# compiled with debug information, which records what the compiler actually
# laid out -- after every typedef, every #ifdef, every packing rule and every
# alignment decision the two targets make differently. A header says what
# somebody meant; DWARF says what happened.
#
# ⚠️ A structure absent from one kernel's DWARF is REPORTED, not skipped. The
# compiler emits a type only where it is used, so absence means "this build
# does not touch it" -- which is a fact about coverage and not a pass. A check
# that quietly counts what it could not look at as agreement is the shape this
# project keeps finding in its own instruments.
#
# ── What a difference means ───────────────────────────────────────────
#
# Not every difference is a defect. A structure that is kernel-private, or one
# that deliberately holds a pointer, is entitled to be wider on a wider
# machine. What the issue asks is that each difference be looked at and either
# confirmed harmless or made explicit -- so this prints them and counts them,
# and refuses to decide on anyone's behalf.
#
# Usage:
#   scripts/uapi-layout-check.py [--verbose]
#
# Exit status: 0 if every structure that both builds contain agrees, 1 if any
# differ, 2 if it could not run at all.

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UAPI = os.path.join(REPO, "uros", "uapi")
KERNELS = {
    "i386": os.path.join(REPO, "uros", "build", "export", "uros", "boot",
                         "mach_kernel"),
    "x86-64": os.path.join(REPO, "uros", "build-x86_64", "export", "uros",
                           "boot", "mach_kernel"),
}

# A member line: "	type  name;   /*     0     4 */"
MEMBER = re.compile(r"^\s+.*\b(\w+)\s*(?:\[[^\]]*\])?\s*;\s*/\*\s*(\d+)\s+(\d+)\s*\*/")
SIZE = re.compile(r"/\*\s*size:\s*(\d+)")


def uapi_structs():
    """Every struct and union declared in the kernel/user boundary tree.

    Taken from the headers rather than from a list somebody maintains: uapi/
    IS the boundary since #481 moved it there, so what is declared in it is
    what crosses.
    """
    names = set()
    decl = re.compile(r"^(struct|union)\s+([A-Za-z_]\w*)\s*\{")

    for root, _dirs, files in os.walk(UAPI):
        for f in files:
            if not f.endswith(".h"):
                continue
            with open(os.path.join(root, f), errors="replace") as fh:
                for line in fh:
                    m = decl.match(line)
                    if m:
                        names.add((m.group(1), m.group(2)))

    return sorted(names)


def layout(kernel, kind, name):
    """(total size, [(member, offset, size)]) as the DWARF has it, or None."""
    try:
        out = subprocess.run(["pahole", "-C", name, kernel],
                             capture_output=True, text=True, timeout=120).stdout
    except (OSError, subprocess.TimeoutExpired) as e:
        print("pahole: %s" % e, file=sys.stderr)
        return None

    if not out.strip():
        return None

    # pahole prints every type with that name; the first one is enough, and a
    # second definition under the same name would be its own finding.
    members = []
    total = None
    for line in out.splitlines():
        m = MEMBER.match(line)
        if m:
            members.append((m.group(1), int(m.group(2)), int(m.group(3))))
            continue
        s = SIZE.search(line)
        if s and total is None:
            total = int(s.group(1))

    if total is None:
        return None

    return (total, members)


def main():
    verbose = "--verbose" in sys.argv

    for target, path in KERNELS.items():
        if not os.path.exists(path):
            print("no kernel for %s at %s -- build both targets first"
                  % (target, path), file=sys.stderr)
            return 2

    agreed = []
    differ = []
    absent = []

    for kind, name in uapi_structs():
        a = layout(KERNELS["i386"], kind, name)
        b = layout(KERNELS["x86-64"], kind, name)

        if a is None or b is None:
            where = ("both builds" if a is None and b is None else
                     "the i386 build" if a is None else "the x86-64 build")
            absent.append((name, where))
            continue

        if a == b:
            agreed.append(name)
            if verbose:
                print("  = %-40s %d bytes" % (name, a[0]))
            continue

        differ.append((name, a, b))

    for name, a, b in differ:
        print("≠ %s: %d bytes on i386, %d on x86-64" % (name, a[0], b[0]))
        amap = dict((m, (o, s)) for m, o, s in a[1])
        bmap = dict((m, (o, s)) for m, o, s in b[1])
        for m, o, s in a[1]:
            if m not in bmap:
                print("    %-28s i386 only (offset %d, %d bytes)" % (m, o, s))
            elif bmap[m] != (o, s):
                print("    %-28s i386 offset %d size %d, x86-64 offset %d "
                      "size %d" % (m, o, s, bmap[m][0], bmap[m][1]))
        for m, o, s in b[1]:
            if m not in amap:
                print("    %-28s x86-64 only (offset %d, %d bytes)" % (m, o, s))

    print()
    print("%d structures declared in uapi/" % len(uapi_structs()))
    print("  %d agree on both targets" % len(agreed))
    print("  %d differ" % len(differ))
    print("  %d not in one build's debug information, so NOT CHECKED:"
          % len(absent))
    for name, where in absent:
        print("      %-40s absent from %s" % (name, where))

    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
