#!/usr/bin/env python3
#
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# mig-target-check.py -- does every MIG invocation say which target it is
# generating for?  (#520)
#
# migcom lays out a message from a table of per-target sizes: header 24 bytes
# on both, but body 4 against 8, descriptor 12 against 16, maximum alignment 4
# against 8.  It picks i386 unless told otherwise, and that default is correct:
# every invocation written before the option existed meant i386, and guessing
# from the host would be the same mistake somewhere new.
#
# 🔴 THE FAILURE IS SILENT, WHICH IS WHY THIS EXISTS.  A stub generated with
# i386's layout for an x86-64 program compiles, links, and composes messages
# whose fields are in the wrong places.  Nothing says so.  block_device_server
# had four such invocations and hal_server had had three before it (#427); the
# _Static_asserts migcom plants caught them, but only for the .defs whose types
# happen to disagree in a way an assert can see -- a subsystem of scalars
# generates quietly wrong stubs and no assert at all.
#
# So the rule is not "the sizes must agree", which is checked where it can be.
# The rule is that the target must be STATED, everywhere, so that the quiet
# case cannot arise.
#
# ⚠️ THIS READS CMakeLists, NOT THE BUILD, and that is deliberate here even
# though the opposite is usually right.  The point is to catch an invocation
# that is not compiled on any target configured today -- ext_server's four, for
# instance, which are i386-only until #498 lifts them and would otherwise be
# found by whoever ports it, the hard way.  Asking the build would only ever
# see what the build already builds.
#
# Two ways to say it, both accepted:
#
#   ${UROS_MIG_TARGET_ARGS}     the named form, set once in uros/CMakeLists.txt
#   -target ${UROS_TARGET_ARCH} written out, as hal_server does
#
# and a third that needs nothing, because it says it for you:
#
#   add_mig_userland(...)       calls migcom directly, passing the target
#                               itself -- see uros/cmake/mig.cmake

import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ROOTS = [os.path.join(REPO, "uros")]

# A migcom invocation names the binary; the target must follow within a few
# lines, because the flags of one add_custom_command sit together.
INVOCATION = re.compile(r"^\s*-migcom\b")
TARGET = re.compile(r"UROS_MIG_TARGET_ARGS|^\s*-target\b")
COMMENT = re.compile(r"^\s*#")

# How many FLAGS after `-migcom' the target may appear within.  Small on
# purpose: further away it belongs to a different invocation, and this check
# would then pass a file where one call is targeted and the next is not.
#
# ⚠️ Flags, not lines, and the difference is not pedantry -- it is the first
# thing this script got wrong.  Counting lines made it report hal_server and
# libnetname, whose targets sit behind six-line comments explaining this very
# defect: a comment is what a careful author writes at exactly the place a
# checker must not treat as distance.
WINDOW = 4


def check(path):
    """Return a list of (line number, offending line) for this file."""
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")

    bad = []
    for i, line in enumerate(lines):
        if not INVOCATION.match(line):
            continue

        found = False
        budget = WINDOW
        for j in range(i + 1, len(lines)):
            if COMMENT.match(lines[j]) or not lines[j].strip():
                continue
            if TARGET.search(lines[j]):
                found = True
                break
            budget -= 1
            if budget == 0:
                break

        if not found:
            bad.append((i + 1, line.strip()))

    return bad


def main():
    failures = []
    seen = 0

    for root in ROOTS:
        for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
            # Build directories hold generated copies; they are not sources.
            dirnames[:] = [d for d in dirnames if not d.startswith("build")]

            if "CMakeLists.txt" not in filenames:
                continue

            path = os.path.join(dirpath, "CMakeLists.txt")
            with open(path, encoding="utf-8") as f:
                if "-migcom" not in f.read():
                    continue

            seen += 1
            for lineno, text in check(path):
                failures.append((os.path.relpath(path, REPO), lineno, text))

    if failures:
        print("MIG invocations that do not state their target:\n")
        for path, lineno, text in failures:
            print(f"  {path}:{lineno}: {text}")
        print(
            f"\n{len(failures)} invocation(s) in {seen} file(s) would use "
            "migcom's default, which is i386.\n"
            "Add ${UROS_MIG_TARGET_ARGS} after the -migcom line, or use "
            "add_mig_userland()."
        )
        return 1

    print(f"every MIG invocation states its target ({seen} files)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
