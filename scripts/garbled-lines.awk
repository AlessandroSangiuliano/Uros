#!/usr/bin/awk -f
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# garbled-lines.awk — lines on the console that carry two programs' output (#578, #544).
#
# Prints every line in which a known program's "name: " begins in the MIDDLE,
# glued to the text before it: `cap_test: ALchar_test: [1] this line left ...'.
# That is two writers inside one line, whatever the cause -- a printf that
# left in two writes (#578 B), a forwarder that sent half a line (#578 A), two
# writers on one wire (#544).
#
# 🔴 EVERY OCCURRENCE IS LOOKED AT, NOT THE LEFTMOST.  The first version used
# match(), which tries only the leftmost candidate: in `cap_test: ALchar_test:'
# it found `ap_test:' inside cap_test, rejected it, and never looked further --
# it flagged NOTHING, not even the case it was written for.
#
# ⚠️ What is NOT a garbled line, and is excluded by shape: a name after a
# separator (space, quote, slash, bracket, `=', `_'), a column of numbers with
# no newline followed by a line (`  10428bootstrap: ...' -- an unfinished line,
# not two writers inside one), and terminal escape sequences.
#
# The names are the prefixes that start lines in the x86-64 boots, taken from
# the logs rather than remembered.  Checked over 105 boots before it was wired
# in: it flags the 133 glued lines found by hand and nothing else.
#
# Usage:  awk -f scripts/garbled-lines.awk <log>     (prints "LINE: text")
BEGIN {
	n = split("blk bootstrap cap_test hal cap device bundle dma_reclaim ahci pci_scan device_io_port irq_claim_test cow_test clock_event hal_bar act_test virtio fault_test fperr_test dl_test console io_claim_race netname_test char_server char_test startup pmap pthread_test mig uart char state_test log_forwarder fpu_stress boot_probe", names, " ")
}
/^=== run conditions/ { exit }
{
	line = $0; hit = 0
	if (line ~ /\033/)
		next
	for (i = 1; i <= n && !hit; i++) {
		key = names[i] ": "; off = 0; rest = line
		while ((p = index(rest, key)) > 0) {
			at = off + p
			if (at > 1) {
				c = substr(line, at - 1, 1)
				pre = substr(line, 1, at - 1)
				if (c !~ /[ \t'"\/(\[{=_]/ && pre !~ /^[ 0-9]*$/) {
					hit = 1
					break
				}
			}
			off = at + length(key) - 1
			rest = substr(line, off + 1)
		}
	}
	if (hit)
		print FNR ": " line
}
