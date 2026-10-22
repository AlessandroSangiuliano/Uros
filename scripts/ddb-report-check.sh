#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ddb-report-check.sh -- #750: a panic that opens the debugger leaves its
# backtrace and its processors in the log, before the prompt.
#
# With -r a panic calls Debugger() instead of halt_cpu(), and the prompt waits
# for an operator that an unattended boot does not have, so the log ends
# there.  Until #750 it ended at the registers, and #749's panic, one boot in
# 255,691, could not say who had called it.  An entry through Debugger() now
# prints before its first prompt what `t', `p' and `P n' would
# (report_before_prompt() in x86_64/ddb/ddb.c).  This reads the log
# run-x86_64.sh wrote, from the first "ddb: panic" to the first prompt after
# it, and asks:
#
#   1. the backtrace's header and at least one frame after it, and, when a
#      function is named, a frame in it -- the one the panic came from;
#   2. the processors' header, the processor at the prompt with the thread it
#      runs, and every processor in the scheduler listed: at the prompt,
#      parked, or said not to have parked;
#   3. for each parked processor, its own header and its registers, and at
#      least one frame when it was parked in the kernel; one parked in ring 3,
#      as its cs says, the line that its rbp is the task's and is not walked,
#      and no frame at all -- a frame there is the task's rbp walked as the
#      kernel's chain, which in run 39781890422 read 0x3040 through the task
#      at the prompt and stopped the report (#750).
#
# Prints one line, PASS, WRONG or NOT ASKED; exits 0, 1 or 2.  A log in which
# no panic opened the debugger is NOT ASKED.
#
#   ./scripts/ddb-report-check.sh LOG [function]
#
# The processor count is the kernel's own ("startup: N processors in the
# scheduler").
set -u
LOG=${1:?usage: $0 LOG [function]}
[ -r "$LOG" ] || { echo "ddb-report-check: cannot read $LOG" >&2; exit 2; }
WANT=${2:-}

tr -d '\r' < "$LOG" | awk -v want="$WANT" '
function wrong(why) {
	printf "ddb-report-check: WRONG — %s (#750)\n", why
	exit 1
}
state == 0 && /^startup: [0-9]+ processors in the scheduler$/ { ncpu = $2 + 0 }
state == 0 && $0 == "ddb: panic" { state = 1; next }
state != 1 { next }
/^ddb> / { state = 2; next }
/^ddb: the backtrace, as .t. prints it \(#750\)$/ { sect = "t"; have_t = 1; next }
/^ddb: the processors, as .p. prints them$/ { sect = "p"; have_p = 1; next }
/^ddb: cpu [0-9]+, as .P [0-9]+. prints it$/ {
	sect = "P"
	pcpu = $3
	sub(/,$/, "", pcpu)
	psect[pcpu] = 1
	next
}
/^  0x[0-9a-f]+( <[^>]+>)?$/ {
	if (sect == "t") {
		tframes++
		if (want != "" && (index($0, "<" want "+") || index($0, "<" want ">")))
			twant = 1
	} else if (sect == "P") {
		pframes[pcpu]++
	}
	next
}
sect == "P" && /^  rip    0x[0-9a-f]+/ { pregs[pcpu] = 1; next }
# The ring is the requested privilege level in cs, its low two bits -- which
# the last hex digit decides alone, 16 being a multiple of 4 -- as
# trace_frame() asks it in the kernel.  (No apostrophe anywhere in this awk
# program: it sits inside single quotes.)
sect == "P" && /^  cs     0x[0-9a-f]+/ {
	d = index("0123456789abcdef", substr($2, length($2), 1)) - 1
	if (d % 4 == 3)
		pring3[pcpu] = 1
	next
}
sect == "P" && /^  in ring 3: / { psaid3[pcpu] = 1; next }
sect == "p" && /^  cpu [0-9]+  at the prompt$/ {
	atprompt++
	me = $2
	listed[$2] = 1
	last = "prompt"
	next
}
sect == "p" && /^  cpu [0-9]+  parked at 0x/ { parked[$2] = 1; listed[$2] = 1; last = "parked"; next }
sect == "p" && /^  cpu [0-9]+  DID NOT PARK/ { listed[$2] = 1; last = "not parked"; next }
sect == "p" && last == "prompt" && /^  thread 0x[0-9a-f]+/ { promptthread = 1; next }
END {
	if (state == 0) {
		print "ddb-report-check: NOT ASKED — no panic opened the debugger in this log"
		exit 2
	}
	if (state == 1)
		wrong("the debugger opened at a panic and its prompt never came")
	if (!have_t)
		wrong("the debugger opened at a panic and printed no backtrace before its prompt")
	if (tframes == 0)
		wrong("the backtrace before the prompt has no frame")
	if (want != "" && !twant)
		wrong("no frame of the backtrace before the prompt is in " want "()")
	if (!have_p)
		wrong("the debugger opened at a panic and listed no processors before its prompt")
	if (atprompt != 1)
		wrong(atprompt " processors at the prompt in the list before it, where one is")
	if (!promptthread)
		wrong("cpu " me " was listed at the prompt without the thread it runs")
	n = 0
	for (c in listed)
		n++
	if (ncpu > 0 && n != ncpu)
		wrong(ncpu " processors in the scheduler, " n " listed before the prompt")
	np = 0
	nu = 0
	for (c in parked) {
		if (!psect[c] || !pregs[c])
			wrong("cpu " c " was parked, and its registers were not printed before the prompt")
		if (pring3[c]) {
			if (pframes[c])
				wrong("cpu " c " was parked in ring 3, and its rbp was walked as a chain of the kernel: " pframes[c] " frames before the prompt")
			if (!psaid3[c])
				wrong("cpu " c " was parked in ring 3, and the report did not say its rbp belongs to the task and is not walked")
			nu++
		} else if (!pframes[c])
			wrong("cpu " c " was parked in the kernel, and its backtrace was not printed before the prompt")
		np++
	}
	printf "ddb-report-check: PASS — before its first prompt the debugger said the backtrace, %d frames%s, and %d processors: cpu %s at the prompt with its thread, %d parked with their registers and backtraces%s (#750)\n", tframes, (want != "" ? " through " want "()" : ""), n, me, np, (nu ? ", " nu " of them in ring 3, said not walked, the rbp of the task" : "")
	exit 0
}'
