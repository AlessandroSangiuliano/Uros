#!/usr/bin/awk -f
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# rejoin-lines.awk — the console's lines as their writers wrote them (#757).
#
# The kernel no longer splices a line in silence: a writer that prints into
# another writer's open line first closes it with " <-cut [W]", and W's rest
# starts a line of its own behind "[W]-> " (kern/printf.c).  This puts each such
# line back together, marks removed, where its rest arrives -- so a check that
# looks for a whole line finds it.  A rest cut again is rejoined again.  Lines
# with no mark pass as they are.
#
# W is a thread ("t:" and its address in hex), a processor in an interrupt
# ("cpuN:irq"), or "raw" (the debugger, a nested printf).
#
# ⚠️ A cut whose rest never came -- the boot ended, the machine stopped -- is
# printed at the end, mark and all: unfinished, and visibly so.  And a rest
# whose start is not in this log (it was before the log began) is printed
# without its mark.
#
# Usage:  awk -f scripts/rejoin-lines.awk LOG > LOG.joined
{
	line = $0
	cr = ""
	if (sub(/\r$/, "", line))
		cr = "\r"

	if (match(line, /^\[(t:[0-9a-f]+|cpu[0-9]+:irq|raw)\]-> /)) {
		w = substr(line, 2, RLENGTH - 5)
		rest = substr(line, RLENGTH + 1)
		if (w in pending) {
			line = pending[w] rest
			delete pending[w]
		} else
			line = rest
	}

	if (match(line, / <-cut \[(t:[0-9a-f]+|cpu[0-9]+:irq|raw)\]$/)) {
		w = substr(line, RSTART + 8, RLENGTH - 9)
		pending[w] = substr(line, 1, RSTART - 1)
		next
	}

	print line cr
}
END {
	for (w in pending)
		print pending[w] " <-cut [" w "]"
}
