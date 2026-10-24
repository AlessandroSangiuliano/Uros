#!/usr/bin/awk -f
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# profile-dumps-check.awk — every dump of the IPC profile contiguous and whole (#757).
#
# kern/syscall_profile.c prints a dump as one block: a header, the routes line,
# the ruler line, and a table for every shape with two samples or more -- its
# category line and its phases.  Two dumps printed at the same moment used to
# interleave, and a reader going by order gave one dump's phases to the other's
# header (on omen's metal, 43 of intra's 52 dumps a boot).  So between one
# header and the next, this asks for exactly one routes line, one ruler line,
# as many category lines as the header has shapes to print, and phases under
# every category.  Interleaving cannot keep all four.
#
# Read a log with its cut lines rejoined (scripts/rejoin-lines.awk): a line cut
# by another writer is still the line it was.  Lines that are not the profile's
# -- an interrupt's, which a block does not keep out -- are not counted.
#
# Prints a WRONG line per dump that fails, and one line of verdict; exits 1 if
# any failed.
#
# Usage:  awk -f scripts/rejoin-lines.awk LOG | awk -f scripts/profile-dumps-check.awk
function close_dump(	bad) {
	if (!open)
		return
	dumps++
	if (phases == 0)
		empty = 1
	bad = ""
	if (routes != 1)
		bad = bad ", routes lines " routes
	if (rulers != 1)
		bad = bad ", ruler lines " rulers
	if (cats != want)
		bad = bad ", category lines " cats " of " want
	if (empty)
		bad = bad ", a category with no phase under it"
	if (bad != "") {
		wrong++
		sub(/^, /, "", bad)
		print "profile-dumps: the dump at line " hline " (" hdesc "): " \
		    bad " -- WRONG (#757)"
	}
	open = 0
}
/^syscall_profile trap [0-9]+ thread 0x[0-9a-f]+ dump [0-9]+, window / {
	close_dump()
	open = 1
	hline = FNR
	routes = rulers = cats = empty = 0
	phases = -1
	match($0, /thread 0x[0-9a-f]+ dump [0-9]+/)
	hdesc = substr($0, RSTART, RLENGTH)
	want = 0
	if (match($0, /shapes: [0-9]+ hand-off, [0-9]+ blocked, [0-9]+ awake/)) {
		split(substr($0, RSTART + 8, RLENGTH - 8), f, /[^0-9]+/)
		for (i = 1; i <= 3; i++)
			if (f[i] + 0 >= 2)
				want++
	}
	next
}
!open { next }
/^syscall_profile   routes kernel-wide:/ { routes++; next }
/^syscall_profile   each column below includes ONE pair/ { rulers++; next }
/^syscall_profile  \[/ {
	if (phases == 0)
		empty = 1
	cats++
	phases = 0
	next
}
/^syscall_profile    / {
	if (phases >= 0)
		phases++
	next
}
END {
	close_dump()
	printf "profile-dumps: %d dumps, %d not contiguous and whole -- %s (#757)\n", \
	    dumps, wrong, (dumps > 0 && wrong == 0) ? "PASS" : (dumps == 0 ? "NOT ASKED, no dump" : "WRONG")
	exit (wrong > 0)
}
