#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# #599: the verdict on a -Z boot (GRUB entry "two processors panic at once").
#
# The kernel does not survive the question, so the answer is read from the log
# run-x86_64.sh wrote (UROS_X86_64_LOG), from the panic message to the end of
# the kernel's output:
#
#   1. one panic message, whole -- "panic(cpu N): double_panic: two processors
#      panic at once, this one is cpu N (#599)", the same N twice -- and no
#      line that starts one and does not finish it;
#   2. the console's final copy, whole and once, after the message and before
#      the first backtrace (#567, #568);
#   3. one whole backtrace header per processor, each processor once;
#   4. no line after it that carries a piece of one of these and is not it
#      whole: that is two outputs run together.  A whole line of some other
#      kind is allowed -- a processor not yet stopped may print one between
#      the message and the halt -- and counted.
#
# Prints one line, PASS, WRONG or NOT ASKED; exits 0, 1 or 2.  A boot that
# never posed the question -- one processor, interrupts off -- is NOT ASKED.
#
#   ./scripts/double-panic-check.sh LOG [processors]
#
# The processor count is the kernel's own ("startup: N processors in the
# scheduler") unless given.
set -u
LOG=${1:?usage: $0 LOG [processors]}
[ -r "$LOG" ] || { echo "double-panic-check: cannot read $LOG" >&2; exit 2; }
WANT=${2:-}

tr -d '\r' < "$LOG" | awk -v want="$WANT" '
function wrong(why) {
	printf "double-panic-check: WRONG — %s\n", why
	bad = 1
}
BEGIN {
	msg = "^panic\\(cpu [0-9]+\\): double_panic: two processors panic at once, this one is cpu [0-9]+ \\(#599\\)$"
	fb = "^UrMach x86-64: console: (the framebuffer drew [0-9]+ glyphs in [0-9]+ cycles( = [0-9]+ ns a glyph)? on a [0-9]+x[0-9]+ screen, [0-9]+ scrolls( — NOT ASKED for the time, no calibrated TSC)? \\(#568\\)|no framebuffer was drawn on this boot — COM1 was the only output there was \\(#568\\))$"
	ring = "^UrMach x86-64: console: over this boot the ring was handed over [0-9]+ times by the thread that printed, [0-9]+ by a tick or an idle processor, [0-9]+ on the way down; [0-9]+ writers paid for room, [0-9]+ bytes are still queued \\(#567\\)$"
	hdr = "^  cpu [0-9]+ backtrace \\(addr2line for file and line\\):$"
	frame = "^    0x[0-9a-f]+( <[^<>]+>)?$"
}
/^qemu-system-x86_64:/ || /^=== run conditions/ || /^=== qemu.s own messages/ { done = 1 }
done { next }
/^startup: [0-9]+ processors in the scheduler/ { cpus = $2 }
/^double_panic: NOT ASKED/ { declined = $0 }
/^double_panic: WRONG/ { refused = $0 }
/^double_panic: processors / { asked = 1 }
{
	if ($0 ~ msg) {
		split($0, a, /[()]/)
		n1 = a[2]; sub(/^cpu /, "", n1)
		n2 = $0; sub(/.*this one is cpu /, "", n2); sub(/ .*/, "", n2)
		if (n1 != n2)
			mixed++
		whole++
		if (!at)
			at = NR
		next
	}
	if ($0 ~ /^panic/ || index($0, "double_panic: two processors") > 0) {
		cut++
		if (!firstcut)
			firstcut = $0
		next
	}
	if (!at)
		next
	if ($0 ~ fb) { nfb++; if (nhdr) late++; next }
	if ($0 ~ ring) { nring++; if (nhdr) late++; next }
	if ($0 ~ hdr) {
		id = $2
		if (id in seen)
			twice++
		seen[id] = 1
		nhdr++
		next
	}
	if ($0 ~ frame || $0 == "")
		next
	if (index($0, "UrMach x86-64: console:") || index($0, "backtrace (addr2line") ||
	    index($0, "0xffffffff8") || $0 ~ /^    0x/) {
		other++
		if (!firstother)
			firstother = $0
		next
	}
	alive++
}
END {
	if (refused != "") {
		printf "double-panic-check: WRONG — the test refused: %s\n", refused
		exit 1
	}
	if (!asked) {
		if (declined != "")
			printf "double-panic-check: NOT ASKED — %s\n", declined
		else
			printf "double-panic-check: NOT ASKED — no -Z test in this log\n"
		exit 2
	}
	if (want == "")
		want = cpus
	if (want == "") {
		printf "double-panic-check: NOT ASKED — the log does not say how many processors were in the scheduler\n"
		exit 2
	}
	if (whole != 1)
		wrong(sprintf("%d whole panic messages, one wanted", whole))
	if (mixed)
		wrong("a panic message names two processors")
	if (cut)
		wrong(sprintf("%d lines start a panic message and do not finish it, the first: %s", cut, firstcut))
	if (nfb != 1 || nring != 1)
		wrong(sprintf("the console final copy came %d and %d times (framebuffer, ring), once each wanted", nfb, nring))
	if (late)
		wrong("the console final copy came after a backtrace had started")
	if (nhdr != want)
		wrong(sprintf("%d whole backtraces for %d processors", nhdr, want))
	if (twice)
		wrong("a processor gave two backtraces")
	if (other)
		wrong(sprintf("%d lines after the message carry a piece of one of its parts and are not it, the first: %s", other, firstother))
	if (bad)
		exit 1
	printf "double-panic-check: PASS — one whole panic message, the console final copy whole after it, %d whole backtraces for %d processors, nothing run together (%d whole lines from processors not yet stopped)\n", nhdr, want, alive + 0
	exit 0
}'
