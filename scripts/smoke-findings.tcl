#!/usr/bin/env tclsh
#
# smoke-findings.tcl — naming a failed wait instead of labelling it (#544)
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Sourced by scripts/smoke-ush.exp.  Runnable on its own over a captured
# transcript, which is how the detector below is tested:
#
#     ./scripts/smoke-findings.tcl <transcript> <literal>
#
# ── Why this exists ───────────────────────────────────────────────────
#
# Eleven runs of the i386 acceptance smoke, one tree, clock pinned: 8 passed,
# 3 failed.  From the outside all three were one event -- an `expect' step ran
# out of budget and printed `FAIL: no <thing>'.
#
# The issue recorded them as TWO defects, one garbled line and two runs where
# the output stopped mid-line while the work went on.  🔥 THAT WAS WRONG, and
# only a detector could show it: all three have the wanted text PRESENT and cut
# into by another writer.  The decomposition is exact --
#
#     ush: bound ctty [dbegv]_ ipdi=d2= 4to usshi$ d=3
#   = ush: bound ctty dev_id=2 to sid=3  +  [bg] pid=4  +  ush$
#                                                     residue: none
#
# -- and the run that "stopped" goes on to print hello_exec's whole AUXV dump
# after the point it was said to have stopped at.  The truncation class has no
# confirmed members.
#
# 🔑 One label over three instances of one cause is not a cosmetic problem.  It
# produced two confident wrong stories in a row: the failures were blamed on
# the CPU governor, that was withdrawn when the same failure appeared at full
# clock, and the replacement story lasted until somebody pointed a detector at
# the bytes.  Nothing in the output could have contradicted either, because
# nothing in the output said which of the three had happened.
#
# So this does not make the gate pass more often.  It makes a red run say which
# kind of red it is.  Four findings, four sentences:
#
#   NOTHING ARRIVED      the machine stopped producing output
#   STILL PRODUCING      output was flowing when the cap ran out
#   GARBLED              the bytes we wanted are there, interleaved
#   HOST WAS SUSPENDED   this process was not scheduled; nothing was observed
#
# ⚠️ The fourth was not in the plan.  It was added because the first campaign
# run under this harness produced two of it and they were read as the other
# two -- once as a budget set too tight, once as a benchmark that had stalled.
# The kernel journal settled it, and the harness can now settle it by itself.
#
# ⚠️ i386 is a reference instrument, not a target: it is kept because it is the
# only thing that works end to end, and the shared code (libmach, migcom, the
# .defs) has no other end-to-end net.  Serialising its console is deliberately
# NOT being done -- that is engineering on a target being retired.  Telling its
# failures apart is, because a reference signal that cannot be read is not a
# reference.

# ── Is the wanted text present with someone else's bytes inside it? ───
#
# The observed garbling interleaves a character at a time, so the characters we
# want are all present, in order, with foreign ones between them:
#
#     wanted   u  s  h  $ ␣
#     arrived  u g s p u h _ $ ␣          <- `ush$ ' + `gpu_ser'
#
# That is a SUBSEQUENCE inside a short window, and the window is the whole
# point: over a long enough stretch of text any short string is a subsequence
# of it by accident.  `slack' is how many foreign characters are tolerated
# across the whole match -- the case above needed four.
#
# 🔴 This reports EVIDENCE, not a verdict.  It returns the window it found so
# the run prints it and a person sees what the matcher saw.  A detector that
# says "garbled" without showing the bytes is asking to be believed.
proc uros_interleaved_window {hay needle slack} {
	set nl [string length $needle]
	set hl [string length $hay]
	if {$nl == 0 || $hl == 0} { return "" }
	set maxwin [expr {$nl + $slack}]

	# 🔴 The SHORTEST window, not the first one that completes.  The first is
	# whichever start position comes earliest in the text, and on the
	# captured prompt failure that was 74 characters beginning two lines
	# above the damage -- a report that buries the evidence it exists to
	# show.  The tightest window is the one a person can read:
	#
	#     first:    <<ush v0.1.0[smoke] ush banner OK ... ugspuh_$ >>
	#     shortest: <<ugspuh_$ >>
	set best ""
	for {set i 0} {$i < $hl} {incr i} {
		if {[string index $hay $i] ne [string index $needle 0]} { continue }
		set lim [expr {$i + $maxwin}]
		if {$lim > $hl} { set lim $hl }
		if {$best ne "" && $lim > $i + [string length $best]} {
			set lim [expr {$i + [string length $best]}]
		}
		set j 1
		set k [expr {$i + 1}]
		while {$k < $lim && $j < $nl} {
			if {[string index $hay $k] eq [string index $needle $j]} { incr j }
			incr k
		}
		if {$j == $nl} { set best [string range $hay $i [expr {$k - 1}]] }
	}
	return $best
}

# How many foreign characters we tolerate inside the wanted text.
#
# 🔴 MEASURED FROM BOTH SIDES, because a threshold defended from one side is
# not defended.  Swept over the eleven transcripts of the campaign that opened
# #544 -- the three failures as signal, and the eight passes cut off just
# before the wanted text first arrives as noise, which is real boot output in
# which the needle genuinely never came:
#
#     slack    captured failures found    false hits on 32 green prefixes
#        8            1 of 3                          0
#       44            3 of 3                          0
#       96            3 of 3                          0
#      128            3 of 3                          8
#      768            3 of 3                         16
#
# 🔑 Signal needs 44, noise starts at 128, and 64 sits between them.  ⚠️ The
# noise side is a presence, not an absence: the sweep was taken up until it DID
# produce false hits, because a false-positive test that has never fired has
# not been shown able to.  The first one it produced was an installer line
# ("[5/6] Assemblaggio partizioni ext2...") swallowing `[bg] pid='.
#
# ⚠️ The live haystack is smaller than those 52 KB prefixes -- only what
# arrived during one wait -- so the noise threshold is further away in practice
# than it is here.  That makes 64 conservative, not lucky.
set UROS_GARBLE_SLACK 64

# A garbled window is only a finding if the plain text is NOT there.  If it
# were, the matcher would have matched it and we would not be here -- so this
# guard is about the standalone mode below, where the caller may hand us a
# transcript containing both.
proc uros_garbled {hay needle} {
	global UROS_GARBLE_SLACK
	if {[string first $needle $hay] >= 0} { return "" }
	return [uros_interleaved_window $hay $needle $UROS_GARBLE_SLACK]
}

# Cut a window out of its surroundings for printing, with the control
# characters made visible -- a report of a garbled line must not itself be
# garbled by the bytes it is reporting.
proc uros_visible {s} {
	return [string map [list "\r" "<CR>" "\n" "<LF>" "\t" "<TAB>"] $s]
}

# ── The finding ───────────────────────────────────────────────────────
#
# 🔴 THE NUMBERS ARE NOT ON THE OUTCOME LINE, and that is deliberate.
# scripts/both-accelerators.sh (#516) compares the lines that assert an outcome
# -- it greps PASS|FAIL|WRONG|SKIPPED|panic -- and it normalises away hex and
# times carrying a unit, but a bare `17s' is neither.  Two accelerators that
# both went quiet, one after 15s and one after 17s, would then be reported as
# DISAGREEING about something they agree on.  So the outcome line carries the
# name of the finding and nothing that varies; the seconds go on a `detail:'
# line underneath, where they are read by a person and not by a comparator.
#
# $silent_for : seconds since the last byte arrived
# $elapsed    : seconds since this wait started
# $max_gap    : the longest silence seen DURING the wait, which is the number
#               that says what a budget would have had to be
proc uros_finding {label missing hay silent_for elapsed max_gap quiet_budget {host_asleep 0}} {
	set out ""

	# Garbled is checked first because it is the only one of the three that
	# is a statement about the BYTES rather than about the clock: when the
	# text is there, "nothing arrived" and "still producing" are both true
	# and both beside the point.
	foreach needle $missing {
		set win [uros_garbled $hay $needle]
		if {$win ne ""} {
			append out "\n\[smoke\] FAIL: $label — GARBLED: the bytes are there with another writer's inside them\n"
			append out "\[smoke\] wanted:  <<[uros_visible $needle]>>\n"
			append out "\[smoke\] arrived: <<[uros_visible $win]>>\n"
			append out "\[smoke\] detail:  after ${elapsed}s; this is not a timing failure — the line was written and cut into\n"
			return $out
		}
	}

	# Which of the wanted texts never turned up, named on every path.  The
	# waits that require two things at once ("it printed hello" AND "the
	# shell reaped it") used to have a separate message per half, and losing
	# that when they were folded into one wait would have traded one kind of
	# blindness for another.
	set named ""
	foreach needle $missing {
		append named " <<[uros_visible $needle]>>"
	}

	# 🔴 THE HOST WAS NOT RUNNING, which is not a fact about the kernel at all.
	#
	# `expect' measures wall time, so a laptop that suspends looks EXACTLY like
	# a machine that has stopped producing output: the silence counter jumps
	# forward by the whole sleep in one step.  Measured against the journal, on
	# a campaign that had three suspends fall inside it:
	#
	#     reported silence 98s    PM: suspend 15:21:34 -> 15:23:03   (89s)
	#     reported silence 1136s  PM: suspend 15:30:53 -> 15:49:47   (1134s)
	#
	# Both were read as findings about Uros -- once as a budget that was too
	# tight, once as a benchmark that had stalled -- and both were the observer
	# being asleep.  🔑 Which is this issue's own defect aimed at the harness
	# instead of the kernel: one label over two causes, and the label named the
	# wrong one.
	#
	# Detected rather than inferred: each poll asks for one second and is timed.
	# A one-second wait that took ninety means this process was not scheduled,
	# and no budget computed across it means anything.
	if {$host_asleep > 0} {
		append out "\n\[smoke\] FAIL: $label — THE HOST WAS SUSPENDED: this run cannot be judged\n"
		append out "\[smoke\] missing:$named\n"
		append out "\[smoke\] detail:  ${host_asleep}s of the ${elapsed}s elapsed passed with this process not scheduled\n"
		append out "\[smoke\] detail:  not a finding about the kernel — re-run it on a machine that stays awake\n"
		return $out
	}

	if {$silent_for >= $quiet_budget} {
		append out "\n\[smoke\] FAIL: $label — NOTHING ARRIVED: the machine stopped producing output\n"
		append out "\[smoke\] missing:$named\n"
		append out "\[smoke\] detail:  silent for ${silent_for}s of ${elapsed}s elapsed (budget ${quiet_budget}s)\n"
		append out "\[smoke\] detail:  a slower machine produces output slowly; this produced none\n"
		return $out
	}

	append out "\n\[smoke\] FAIL: $label — STILL PRODUCING OUTPUT: it was working when we gave up\n"
	append out "\[smoke\] missing:$named\n"
	append out "\[smoke\] detail:  ${elapsed}s elapsed, last byte ${silent_for}s ago, longest silence ${max_gap}s\n"
	append out "\[smoke\] detail:  this is the cap, not a defect in the kernel — the run needed more time\n"
	return $out
}

# ── Standalone: run the detector over a captured transcript ───────────
#
# This is how done-when 2 and 3 are answered.  A detector that has only ever
# been exercised by the runs that happen to trip it has not been tested, and a
# detector nobody has pointed at a GREEN log has not been shown to stay quiet.
if {[info exists argv0] && [file tail $argv0] eq "smoke-findings.tcl"} {

	# ── --self-test: each of the three findings, driven on purpose ────
	#
	# 🔴 The specimen below is the real one, kept verbatim from the run that
	# opened #544 -- not a line invented to match the detector.  A detector
	# tested against text written to please it has been tested against
	# itself.
	if {[lindex $argv 0] eq "--self-test"} {
		set SPECIMEN "ush v0.1.0\[smoke\] ush banner OK\n — Uros shell (#275.5)\nugspuh_\$ server: text_puts(147): ush: setsid -> sid=3 pid=3\n"
		set CLEAN    "ush: bound ctty dev_id=2 to sid=3\nush\$ /hello_exec &\n\[bg\] pid=2\n"
		set failures 0

		proc check {name got want} {
			if {[string first $want $got] >= 0} {
				puts "  ok    $name"
				return 0
			}
			puts "  FAIL  $name"
			puts "        wanted <<$want>> in:"
			foreach l [split [string trim $got] "\n"] { puts "        | $l" }
			return 1
		}

		puts "smoke-findings --self-test"

		# 1. the bytes are there, cut into by another writer
		incr failures [check "garbled is named" \
			[uros_finding "ush prompt" {{ush$ }} $SPECIMEN 0 31 2 20] \
			"GARBLED"]
		incr failures [check "garbled shows the tight window" \
			[uros_finding "ush prompt" {{ush$ }} $SPECIMEN 0 31 2 20] \
			"<<ugspuh_\$ >>"]

		# 2. the machine stopped producing output
		incr failures [check "silence is named" \
			[uros_finding "bg fork" {{[bg] pid=}} "boot output that never mentions it\n" 20 31 3 20] \
			"NOTHING ARRIVED"]

		# 3. output was still flowing when the cap ran out
		incr failures [check "still-working is named" \
			[uros_finding "bg fork" {{[bg] pid=}} "boot output that never mentions it\n" 1 31 3 20] \
			"STILL PRODUCING OUTPUT"]

		# 4. 🔑 the one that must NOT fire.  The wanted text is plainly
		#    present, so whatever else went wrong, interleaving did not:
		#    calling this garbled would be the detector inventing the
		#    defect it was written to find.
		incr failures [check "plain text present is not garbling" \
			[uros_finding "bg fork" {{[bg] pid=}} $CLEAN 20 31 3 20] \
			"NOTHING ARRIVED"]

		# 5. and the three name what they were waiting for
		incr failures [check "the missing text is named" \
			[uros_finding "hello_world" {hello exit=0x0} "nothing here\n" 20 31 3 20] \
			"missing: <<hello>> <<exit=0x0>>"]

		# 6. 🔴 A WEAK LITERAL HIDES A GARBLING, and this is the real
		#    window from campaign run 7 that proved it.  `hello' lives
		#    inside `hello_dyn_world', so the plain-text guard stood
		#    down and the finding came out NOTHING ARRIVED -- true of
		#    the idle machine afterwards, false about why the step
		#    failed.  `text_hpelutlos(' minus `hello' is exactly
		#    `text_puts('.
		#
		#    Both directions are asserted: the discriminating literal
		#    must find it, and the weak one must be seen NOT to, so the
		#    case cannot quietly start passing for the wrong reason.
		set RUN7 "exec: /hello_dyn_world -> TASK_CAP_PORT=0x2103\r\ngpu_server: text_hpelutlos(\r\n61): cap: provisioned task\r\n"
		incr failures [check "a line-final literal finds the garbling" \
			[uros_finding "hello_dyn_world" [list "hello\r"] $RUN7 20 31 3 20] \
			"GARBLED"]
		incr failures [check "and the bare word would have missed it" \
			[uros_finding "hello_dyn_world" {hello} $RUN7 20 31 3 20] \
			"NOTHING ARRIVED"]

		# 7. 🔴 The observer was asleep.  Both sets of numbers are the
		#    real ones, taken from a campaign and checked against the
		#    kernel journal's own suspend records:
		#
		#      silence 98s   vs  PM: suspend 15:21:34 -> 15:23:03   (89s)
		#      silence 1136s vs  PM: suspend 15:30:53 -> 15:49:47  (1134s)
		#
		#    Before this existed, the first was read as a budget set too
		#    tight and the second as a benchmark that had stalled.
		incr failures [check "a suspend is not a stopped machine (89s)" \
			[uros_finding "flipc_bench" {{flipc_bench: done}} "niente\n" 98 100 4 90 89] \
			"THE HOST WAS SUSPENDED"]
		incr failures [check "a suspend is not a stopped machine (1134s)" \
			[uros_finding "ush banner" {{ush v0.1.0}} "niente\n" 1136 1147 4 40 1134] \
			"THE HOST WAS SUSPENDED"]

		# 8. ⚠️ And garbling still outranks it: bytes that arrived
		#    interleaved are an observation about the OUTPUT, and remain
		#    true whatever the host did with its own clock afterwards.
		incr failures [check "garbling outranks a suspend" \
			[uros_finding "ush prompt" {{ush$ }} $SPECIMEN 1136 1147 4 40 1134] \
			"GARBLED"]

		if {$failures} {
			puts "smoke-findings --self-test: $failures FAILED"
			exit 1
		}
		puts "smoke-findings --self-test: all 11 passed"
		exit 0
	}

	if {[llength $argv] < 2} {
		puts stderr "uso: smoke-findings.tcl --self-test"
		puts stderr "     smoke-findings.tcl <transcript> <literal> \[literal...\]"
		exit 2
	}
	set path [lindex $argv 0]
	set f [open $path r]
	fconfigure $f -translation binary
	set hay [read $f]
	close $f

	set hits 0
	foreach needle [lrange $argv 1 end] {
		set plain [expr {[string first $needle $hay] >= 0}]
		set win [uros_interleaved_window $hay $needle $UROS_GARBLE_SLACK]
		set inter [expr {$win ne "" && !$plain}]
		if {$inter} { incr hits }
		puts [format "%-24s plain=%-3s interleaved=%-3s %s" \
			<<[uros_visible $needle]>> \
			[expr {$plain ? "yes" : "no"}] \
			[expr {$inter ? "YES" : "no"}] \
			[expr {$inter ? "<<[uros_visible $win]>>" : ""}]]
	}
	puts "interleaved-but-not-plain: $hits"
	exit [expr {$hits > 0 ? 3 : 0}]
}
