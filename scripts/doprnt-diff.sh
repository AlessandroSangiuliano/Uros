#!/bin/sh
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# doprnt-diff.sh — run libmach's formatter against glibc and compare bytes.
#
# ── Why this exists ──────────────────────────────────────────────────
#
# scripts/doprnt-harness.sh runs the KERNEL's _doprnt at both target widths
# and checks it against itself.  This one asks a different question of the
# USERLAND formatter: does it print what a C library prints?
#
# 🔑 The answer has to be measured, and the measuring is what found the
# defects.  A hand-picked set of 725 float cases passed clean while 1287 of
# 40000 random doubles came out with wrong digits -- the sample had simply
# missed the magnitudes that broke.  Both kinds of case are run here for that
# reason: the chosen ones say what was thought about, the random ones say
# what was not.
#
# ⚠️ It compiles doprnt.c FOR THE HOST, which is legitimate for this question
# and only for this one: formatting is arithmetic on values the caller passes,
# with no target in it.  Anything about widths belongs in doprnt-harness.sh.
#
# Usage: scripts/doprnt-diff.sh
# Exit:  0 every case identical to glibc, 1 otherwise

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
SRC="$REPO/uros/src/lib/libmach/doprnt.c"
CASES="$REPO/scripts/doprnt-diff"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

[ -f "$SRC" ] || { echo "doprnt-diff: $SRC is missing" >&2; exit 1; }

# The formatter needs two headers it will not find on the host, and needs
# neither: one declares boolean_t and the other declares itself.
sed -e 's|#include <mach/boolean.h>||' \
    -e 's|#include "externs.h"||' "$SRC" > "$WORK/doprnt.c"

rc=0
for c in cases_int cases_float random_int random_float; do
	cc -std=gnu11 -w -o "$WORK/$c" "$WORK/doprnt.c" "$CASES/$c.c" -lm
	out=$("$WORK/$c")
	echo "$out" | tail -1
	echo "$out" | grep -q ', 0 differ ===' || rc=1
	echo "$out" | grep -v '===' | head -6
done

if [ $rc -eq 0 ]; then
	echo "doprnt-diff: every case printed what glibc printed"
else
	echo "doprnt-diff: FAILED — see the cases above" >&2
fi
exit $rc
