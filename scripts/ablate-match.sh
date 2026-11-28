#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ablate-match.sh -- whether a boot's log shows what ablate.sh asks of it, a
# line an Expect: or a Premise: ERE matches, read with the log's cut lines
# rejoined, as run-x86_64.sh's verdict reads them (#943).
#
# The console keeps a line whole up to 256 bytes (#578).  Past that, another
# writer's output can land between the two halves, the first closed by
# " <-cut [W]" and the rest opened by "[W]-> " (#757).  run-x86_64.sh rejoins
# them with rejoin-lines.awk and judges the result, and ablate.sh used to read
# the raw log: a WRONG line cut in two read NOT CAUGHT in a boot whose harness
# had seen it whole -- 927-reduced-port-reaches-memory and 927-rights-not-
# checked on 12/11, and 786-xstate-bv-unmarked in the rotation's first batch on
# 26/11, act_test [23]'s line cut at byte 256.
#
# The lines are rejoined in the C locale, byte for byte: the marks are ASCII,
# and a log is not always valid UTF-8.  They are matched in UTF-8, where a
# pattern's em dash is one character and not three bytes (ablate.sh says why).
#
# Usage:  ablate-match.sh LOG ERE-FILE
# Exit:   0 a line of LOG, its cut lines rejoined, matches one of the EREs of
#         ERE-FILE · 1 none does · 2 LOG or ERE-FILE cannot be read
set -u

if [ $# -ne 2 ]; then
	echo "usage: ablate-match.sh LOG ERE-FILE" >&2
	exit 2
fi
if [ ! -r "$1" ] || [ ! -r "$2" ]; then
	echo "ablate-match: cannot read $1 or $2" >&2
	exit 2
fi
LC_ALL=C awk -f "$(dirname "$0")/rejoin-lines.awk" "$1" |
	LC_ALL=C.UTF-8 grep -Eqf "$2"
