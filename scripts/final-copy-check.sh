#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# final-copy-check.sh -- #702: the console's final copy comes out whole once
# the machine is stopped.
#
# halt_cpu() says the console's final copy (cons_ring_report(), #567) on every
# stop -- a self-test's "halting the machine", -b's "restarting the machine",
# a panic -- and its last line is the ring's:
#
#   UrMach x86-64: console: over this boot the ring was handed over N times ...
#
# In an orderly halt every processor that passes that report's plain word
# says a copy, and inside the console's report they come one after the other.
# Taken out of it (ablations/702-final-copy-outside-the-report.patch), four
# copies at four processors came out a byte of each in turn and no line of
# them was whole, in 6 boots of 6 at entry 21 under TCG; the harness's
# garbled-lines.awk, which looks for a program's name glued inside a line,
# said GARBLED in 2 of the 6, since a name is rarely whole either.  This asks
# about the copy itself, from the first line that says the machine stops:
#
#   1. at least one of the ring's lines comes out whole after it;
#   2. no line after it carries a piece of the copy -- its framebuffer, glyphs
#      or ring words -- without being one of those lines whole.  Only words
#      that are the copy's own count: the quiet census's "console so far"
#      line also says "paid for room" and "without a glyph", and a line that
#      was being written when the stop came is left cut by the report, on
#      purpose (#702), as that one was at entry 21 in 2 boots of 1776 of
#      46d1c9d8's campaign, with every copy whole.
#
# Prints one line, PASS, WRONG or NOT ASKED; exits 0, 1 or 2.  A log that
# never says the machine stops is NOT ASKED: a run the harness ended has no
# final copy to ask about.
#
#   ./scripts/final-copy-check.sh LOG
set -u
LOG=${1:?usage: $0 LOG}
[ -r "$LOG" ] || { echo "final-copy-check: cannot read $LOG" >&2; exit 2; }

tr -d '\r' < "$LOG" | awk '
BEGIN {
	stop = "(halting|restarting) the machine|^panic\\(cpu [0-9]+\\)"
	fb = "^UrMach x86-64: console: (the framebuffer drew [0-9]+ glyphs in [0-9]+ cycles( = [0-9]+ ns a glyph)? on a [0-9]+x[0-9]+ screen, [0-9]+ scrolls( — NOT ASKED for the time, no calibrated TSC)? \\(#568\\)|no framebuffer was drawn on this boot — COM1 was the only output there was \\(#568\\))$"
	glyph = "^UrMach x86-64: console: the framebuffer drew (every character it was handed: 0 without a glyph|■ for [0-9]+ without a glyph \\(the first U\\+[0-9A-F]+\\)), [0-9]+ not UTF-8 \\(#661\\)$"
	ring = "^UrMach x86-64: console: over this boot the ring was handed over [0-9]+ times by the thread that printed, [0-9]+ by a tick or an idle processor, [0-9]+ on the way down; [0-9]+ writers paid for room, [0-9]+ bytes are still queued \\(#567\\)$"
	piece = "framebuffer drew|not UTF-8 \\(#661\\)|over this boot|the ring was handed|still queued|\\(#567\\)|\\(#568\\)"
}
/^qemu-system-x86_64:/ || /^=== run conditions/ || /^=== qemu.s own messages/ { done = 1 }
done { next }
!said && $0 ~ stop { said = NR; why = $0; next }
said {
	if ($0 ~ ring) {
		whole++
	} else if ($0 ~ piece && $0 !~ fb && $0 !~ glyph && !broken) {
		broken = NR
		bline = $0
	}
}
END {
	if (!said) {
		print "final-copy-check: NOT ASKED — the log never says the machine stops"
		exit 2
	}
	if (broken) {
		printf "final-copy-check: WRONG — line %d carries a piece of the console'\''s final copy and is not one of its lines whole: %.120s\n", broken, bline
		exit 1
	}
	if (whole == 0) {
		printf "final-copy-check: WRONG — the machine stops at line %d and no line of the console'\''s final copy comes out whole after it\n", said
		exit 1
	}
	printf "final-copy-check: PASS — %d whole cop%s of the ring'\''s line after the stop at line %d\n", whole, whole == 1 ? "y" : "ies", said
	exit 0
}'
