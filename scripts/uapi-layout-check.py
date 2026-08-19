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
# machine. So a difference is not the verdict: what the issue asks is that
# each one be LOOKED AT and either confirmed harmless or made explicit, and
# the ACCEPTED table below is where that examining is recorded, one sentence
# each.
#
# 🔑 Which makes the useful output the SHORT list, not the long one. A
# difference that is not in the table is one nobody has examined yet, and that
# is what this fails on -- along with an acceptance that no longer describes
# any difference, because a note about code that has moved on will let the
# next reader think a decision was made about the shape in front of them.
#
# Usage:
#   scripts/uapi-layout-check.py [--verbose]
#
# Exit status: 0 when every difference is one that has been examined, 1 when
# one has not (or when an acceptance has gone stale), 2 if it could not run.

import os
import re
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UAPI = os.path.join(REPO, "uros", "uapi")
# ── The differences that have been looked at, and why each is allowed ──
#
# #415 asks that every cross-boundary structure be "examined and either
# confirmed width-stable or explicitly versioned".  This is where the
# examining is recorded, so that the next difference to appear is the one
# nobody has looked at yet -- which is the only useful thing a check like this
# can tell you.
#
# ⚠️ The question is NOT "do the two targets agree".  A kernel and its tasks
# are always the same architecture here, so a layout that differs between
# targets is harmless unless a producer and a consumer were compiled for
# DIFFERENT ones -- a host tool writing bytes a kernel reads, an on-disk
# header, a ring shared between nodes.  Every entry below answers that
# question and not the easier one.
#
# 🔑 The entry that is NOT here is the one that made this file worth writing:
# struct uros_cap declared itself a wire format, was 188 bytes on i386 and 192
# on x86-64, and now agrees by construction with a _Static_assert holding it
# there.
ACCEPTED = {
    # Pointer-wide by definition: the structure IS a pointer, or is made of
    # them.  A wider machine must lay these out wider.
    "task_user_data":
        "one member, `void *user_data' -- it is a pointer",
    "urmach_futexv":
        "holds `unsigned int *uaddr', an address in the caller's own space",
    "exception_action":
        "kernel-private (inside #if MACH_KERNEL) and holds struct ipc_port *; "
        "it lives inside task and thread_act, never in a message",
    "routine_descriptor":
        "two function pointers and a pointer to an argument array, generated "
        "by MIG into the user's own address space",
    "mach_rpc_signature":
        "a routine_descriptor and its argument descriptors -- see above",
    "rpc_subsystem":
        "MIG generates one per subsystem IN THE USER'S ADDRESS SPACE and says "
        "so in the struct: `vm_address_t base_addr; /* Address of this struct "
        "in user */'.  The kernel copies it in and rewrites the pointers "
        "against that address, so it cannot be width-stable and must not be",
    "rpc_copy_state":
        "holds `vm_offset_t alloc_addr', the address to free",

    # MIG info flavours: kernel to user, marshalled as an array of natural_t
    # whose length travels with it -- and every one of those _COUNT macros is
    # `sizeof(struct)/sizeof(natural_t)', computed rather than written down,
    # so the count follows the target of its own accord.  Checked, not
    # assumed: no _COUNT in uapi/ is a literal.
    "host_basic_info":       "host_info flavour, HOST_BASIC_INFO_COUNT computed",
    "machine_info":          "host_info flavour, count computed from sizeof",
    "task_basic_info":       "task_info flavour, TASK_BASIC_INFO_COUNT computed",
    "vm_region_basic_info":  "vm_region flavour, count computed from sizeof",
    "memory_object_attr_info":
        "memory_object_get_attributes flavour, count computed from sizeof",
    "memory_object_perf_info":
        "memory_object_get_attributes flavour, count computed from sizeof",

    # The loader's description of the bootstrap task, in memory, read by the
    # kernel that was loaded beside it.  Never on disk and never across a
    # build: the ON-DISK boot format is the bundle, and that one is fixed
    # width by construction (uint32_t throughout, with a note in
    # servers/bootstrap/bundle.h saying why the mapped address is the one
    # exception).
    "boot_info":
        "three vm_size_t the loader leaves for the kernel in the same boot",
    "region_desc":
        "vm_offset_t/vm_size_t describing the bootstrap task's address space, "
        "in memory, same target",
}

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
    differ_ok = []
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

        if name in ACCEPTED:
            differ_ok.append(name)
            if verbose:
                print("  ~ %-32s differs, accepted: %s"
                      % (name, ACCEPTED[name]))
            continue

        differ.append((name, a, b))

    for name, a, b in differ:
        print("≠ %s: %d bytes on i386, %d on x86-64 -- NOT ACCEPTED"
              % (name, a[0], b[0]))
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

    stale = sorted(set(ACCEPTED) - set(differ_ok))

    print()
    print("%d structures declared in uapi/" % len(uapi_structs()))
    print("  %d agree on both targets" % len(agreed))
    print("  %d differ and have been examined (see ACCEPTED)" % len(differ_ok))
    print("  %d differ and have NOT" % len(differ))
    print("  %d not in one build's debug information, so NOT CHECKED:"
          % len(absent))
    for name, where in absent:
        print("      %-40s absent from %s" % (name, where))

    if stale:
        # An acceptance for a structure that no longer differs is a note about
        # code that has moved on, and leaving it would let the next reader
        # think a decision had been made about the shape in front of them.
        print()
        print("  %d acceptances no longer describe a difference:" % len(stale))
        for name in stale:
            print("      %s" % name)

    return 1 if differ or stale else 0


if __name__ == "__main__":
    sys.exit(main())
