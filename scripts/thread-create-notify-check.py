#!/usr/bin/env python3
#
# thread-create-notify-check.py — every task that makes itself a second thread
# must tell the allocator before it does (#542).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ── Why this exists ──────────────────────────────────────────────────────
#
# malloc and free skip their lock while a task has never held a second thread.
# That is worth 96 cycles a pair against 47, and it is correct exactly as long
# as the flag is true.  The flag is set by hand at every call that creates a
# thread in the calling task, and a call that forgets does not fail, does not
# warn, and does not look wrong: it silently reopens #540, where two threads
# were handed the same block and bootstrap relocated the wrong file.
#
# 🔑 So forgetting is made impossible rather than remembered.  A new caller
# that does not notify fails this check, which is the difference between a
# rule and a comment asking people to follow one.
#
# ⚠️ It deliberately has NO exemption list.  Call sites in code that is not
# built today — tgdb, the deprecated libcthreads and librthreads — are held to
# the same rule, because an exemption list is a thing that rots: the entry
# outlives the reason, and the next person reads it as permission.
#
# Uso:
#   scripts/thread-create-notify-check.py [radice]
# Esce 1 se un chiamante non notifica.

import re
import sys
from pathlib import Path

# thread_create / thread_create_running against our own task.  A call naming
# another task's port creates a thread THERE, and that task's own allocator is
# not affected by it.
CALL = re.compile(r'thread_create(?:_running)?\s*\(\s*mach_task_self\s*\(\s*\)')
NOTIFY = 'note_thread_created'

# How far back the notify may sit.  It has to be BEFORE the call — a thread
# can be created already running — and close enough that the two read as one
# action.
LOOKBACK = 12

# ⚠️ THE FIRST THING THIS CHECK CAUGHT WAS A COMMENT.  malloc.c explains the
# rule in prose, spelling the call out to say where it must go, and a matcher
# that reads text cannot tell that from code — the same way `grep -c' counts
# strings and not events.  A check that reports the documentation of a rule as
# a violation of it teaches people to ignore the check.
COMMENTISH = re.compile(r'^\s*(\*|/\*|//|\*/)')


def is_comment(line: str) -> bool:
    return bool(COMMENTISH.match(line))


def check(root: Path) -> int:
    bad = []
    seen = 0

    for path in sorted(root.rglob('*.c')):
        parts = path.parts
        if 'build' in parts or 'build-x86_64' in parts:
            continue

        try:
            lines = path.read_text(errors='replace').splitlines()
        except OSError as e:
            print(f'{path}: unreadable: {e}', file=sys.stderr)
            return 2

        for n, line in enumerate(lines):
            if not CALL.search(line) or is_comment(line):
                continue
            seen += 1
            window = lines[max(0, n - LOOKBACK):n]
            if not any(NOTIFY in w for w in window):
                bad.append((path, n + 1, line.strip()))

    rel = lambda p: p.relative_to(root) if p.is_relative_to(root) else p

    if not seen:
        # 🔴 A check that matches nothing is not a check that passed.  If the
        # call ever gets spelled differently, this is the line that says so
        # instead of quietly approving everything.
        print('thread-create-notify: NO call site matched at all — the '
              'pattern no longer describes the tree, which is a finding and '
              'not a pass', file=sys.stderr)
        return 2

    for path, n, text in bad:
        print(f'{rel(path)}:{n}: creates a thread in this task without '
              f'_{NOTIFY}() before it\n    {text}', file=sys.stderr)

    if bad:
        print(f'\nthread-create-notify: {len(bad)} of {seen} call site(s) do '
              f'not notify the allocator.\nSee mach.h: malloc skips its lock '
              f'until a task has had a second thread, so a caller that does '
              f'not say so reopens #540.', file=sys.stderr)
        return 1

    print(f'thread-create-notify: {seen} call site(s), all notify')
    return 0


if __name__ == '__main__':
    root = Path(sys.argv[1] if len(sys.argv) > 1 else '.').resolve()
    sys.exit(check(root))
