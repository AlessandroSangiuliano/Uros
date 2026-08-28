#!/usr/bin/env python3
#
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# host-header-check.py -- does anything built FOR Uros read a header
# belonging to the machine that built it?  (#506)
#
# The question is not answerable by reading CMakeLists: an include root
# arrives through a variable, a target inherits one from a library, and a
# header pulls another one in three levels down.  So this asks the build.
# `ninja -t deps` is the preprocessor's own record of every file it opened,
# and compile_commands.json is the exact command line each object was built
# with -- both written by the build itself, neither restating an intention.
#
# THREE KINDS OF OBJECT, and only one of them has to be clean:
#
#   kernel      compiled -nostdinc: there is no system directory on its
#               path at all, so it cannot reach one.  Immune by
#               construction rather than by discipline.
#
#   target      the Uros userland -- built with the target's -m32/-m64 and
#               linked into things that run on Uros.  THIS is the set that
#               must not read the build machine's headers, because those
#               describe glibc on Linux and not our C library on our kernel.
#
#   host tool   migcom and mkbundle: they run on the machine doing the
#               build, and reading its headers is correct.  Counting them
#               as failures would make the check unpassable, and a check
#               that cannot pass gets disabled.
#
# ⚠️ The compiler's OWN headers are not the system's.  A freestanding
# implementation is required to supply stddef.h, stdint.h, limits.h,
# stdarg.h and the intrinsics; they describe the target and are selected by
# -m32/-m64.  Their directory is asked of the compiler rather than guessed,
# so this keeps working on a machine that arranges them differently.

import json
import os
import re
import subprocess
import sys


def compiler_own_dirs(cc):
    """The compiler's own header directories, from the compiler."""
    dirs = []
    for what in ("include", "include-fixed"):
        try:
            out = subprocess.run([cc, f"-print-file-name={what}"],
                                 capture_output=True, text=True, check=True)
        except (OSError, subprocess.CalledProcessError):
            continue
        p = out.stdout.strip()
        # A compiler that does not have one answers with the name itself.
        if p and p != what and os.path.isdir(p):
            dirs.append(os.path.realpath(p))
    return dirs


def load_commands(build_dir):
    """object path (relative to build_dir) -> its compile command.

    ⚠️ CMake writes `output` as an absolute path and ninja names its
    targets relative to the build directory.  The two have to be brought
    to the same form or every lookup misses and the check reports a clean
    build it never looked at.
    """
    path = os.path.join(build_dir, "compile_commands.json")
    if not os.path.exists(path):
        sys.exit(f"{path}: not found -- configure with "
                 f"CMAKE_EXPORT_COMPILE_COMMANDS=ON")
    root = os.path.realpath(build_dir)
    out = {}
    for e in json.load(open(path)):
        cmd = e.get("command") or " ".join(e.get("arguments", []))
        obj = e.get("output")
        if obj is None:
            # Older CMake does not record `output`; recover it from -o.
            m = re.search(r"-o\s+(\S+)", cmd)
            if not m:
                continue
            obj = m.group(1)
        if not os.path.isabs(obj):
            obj = os.path.join(e.get("directory", build_dir), obj)
        out[os.path.relpath(os.path.realpath(obj), root)] = cmd
    return out


def classify(cmd):
    """kernel / target / host, from the command line itself.

    ⚠️ The discriminant is -ffreestanding and NOT -m32/-m64, which is what
    this asked first.  An arch flag says which ABI, not which machine the
    code is for: a host-run unit test built twice to exercise both ABIs
    carries -m32 and runs on the build machine, and calling it a target unit
    would demand it stop reading the headers it is entitled to.

    -ffreestanding is the right question because it is the same declaration
    the opt-out already makes.  src/ sets it for everything built for Uros;
    migcom, mkbundle and the host-run tests remove it, in one line, with a
    comment.  So the classification and the intent are the same fact.
    """
    words = cmd.split()
    if "-nostdinc" in words:
        return "kernel"
    if "-ffreestanding" in words:
        return "target"
    return "host"


def load_deps(build_dir):
    """object path -> the files the preprocessor opened for it."""
    try:
        out = subprocess.run(["ninja", "-t", "deps"], cwd=build_dir,
                             capture_output=True, text=True, check=True)
    except (OSError, subprocess.CalledProcessError) as exc:
        sys.exit(f"ninja -t deps failed in {build_dir}: {exc}")
    deps = {}
    cur = None
    for line in out.stdout.splitlines():
        if not line.startswith(" "):
            m = re.match(r"^(\S.*?): #deps (\d+)", line)
            if m and not line.rstrip().endswith("(STALE)"):
                cur = os.path.normpath(m.group(1))
                deps[cur] = []
            else:
                cur = None
            continue
        if cur is not None:
            deps[cur].append(line.strip())
    return deps


def main():
    build_dir = sys.argv[1] if len(sys.argv) > 1 else "."
    verbose = "-v" in sys.argv[1:]

    commands = load_commands(build_dir)
    deps = load_deps(build_dir)

    # The compiler is whatever the build used, not whatever is on PATH.
    cc = "cc"
    for cmd in commands.values():
        cc = cmd.split()[0]
        break
    own = compiler_own_dirs(cc)

    src_root = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
    build_root = os.path.realpath(build_dir)

    def is_foreign(dep):
        """Outside the tree, outside the build, and not the compiler's own."""
        if not os.path.isabs(dep):
            return False
        p = os.path.realpath(dep)
        if p.startswith(src_root + os.sep) or p.startswith(build_root + os.sep):
            return False
        return not any(p.startswith(d + os.sep) for d in own)

    counts = {"kernel": 0, "target": 0, "host": 0}
    offenders = {}
    headers = {}

    for obj, cmd in commands.items():
        kind = classify(cmd)
        counts[kind] += 1
        if kind != "target":
            continue
        foreign = sorted({d for d in deps.get(obj, []) if is_foreign(d)})
        if foreign:
            offenders[obj] = foreign
            for h in foreign:
                headers.setdefault(h, set()).add(obj)

    print(f"{os.path.basename(build_root)}: "
          f"{counts['target']} target, {counts['kernel']} kernel, "
          f"{counts['host']} host-tool objects")

    # ⚠️ A green result has to be a green result ABOUT something.  With no
    # target objects to look at, every one of them is clean and the check
    # says nothing -- which is the failure mode this whole issue is about.
    if counts["target"] == 0:
        print("  ERROR: no target-built objects found -- nothing was checked")
        return 2

    seen = sum(1 for o, c in commands.items()
               if classify(c) == "target" and o in deps)
    if seen == 0:
        print("  ERROR: no dependency records for any target object -- "
              "build first, then check")
        return 2
    if seen < counts["target"]:
        print(f"  note: {counts['target'] - seen} target object(s) have no "
              f"dependency record yet and were not checked")

    if not offenders:
        print(f"  {seen} target objects checked, none reads a header of "
              f"the build machine")
        return 0

    print(f"  {len(offenders)} of {seen} target objects read headers that "
          f"belong to the build machine:\n")
    for h, objs in sorted(headers.items(), key=lambda kv: (-len(kv[1]), kv[0])):
        print(f"    {len(objs):4d}  {h}")
    if verbose:
        print()
        for obj in sorted(offenders):
            print(f"    {obj}")
            for h in offenders[obj]:
                print(f"        {h}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
