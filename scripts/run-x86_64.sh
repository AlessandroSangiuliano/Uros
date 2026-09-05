#!/bin/sh
# UrMach x86-64 dev loop (#406+): rebuild the kernel, write the boot disk, boot
# it in QEMU, and say whether the run was good.
#
# The image is ELF64 loaded by GRUB via multiboot2 — qemu's -kernel multiboot1
# loader rejects ELF64, so something has to carry GRUB.  It used to be a rescue
# ISO on -cdrom, with a separate data disk beside it; it is now the disk
# itself, which is the whole of what QEMU is handed (#520).
#
# Usage: run-x86_64.sh [--iommu intel|amd] [--entry N] [seconds] [qemu args...]
#   run-x86_64.sh
#   run-x86_64.sh 30 -smp 4
#   run-x86_64.sh 20 -cpu max -m 2G
#   run-x86_64.sh --iommu intel --entry 16 200      translation ON
#   run-x86_64.sh --iommu intel --entry 14 200      the same board, translation off
#
# Exit status: 0 the run passed · 1 the KERNEL failed · 2 this script refused
# to start (another run is in flight) · 3 QEMU never ran, or ran and produced
# nothing — which says something about the CALLER and nothing about the kernel.
#
# 🔑 Three and one are different findings and must not share a status, for the
# same reason they must not share a sentence.  A rejected command line used to
# come out as "the run was cut short, so the tests after that point did not
# run" -- a statement about a boot that never happened -- and it was believed
# six times in one session (#517).
#
# The pass-through exists for a reason: running qemu by hand to add a flag
# skips writing the disk, and the disk is what boots — every binary in the run
# is inside it. That mistake costs a debugging session chasing a kernel that
# was fixed twenty minutes earlier — so there is no reason to ever invoke qemu
# directly.
#
# ---------------------------------------------------------------- #451
# This script used to print the interesting lines and exit with grep's
# status. 130 self-tests, and no way to fail: reading the lines and forming
# an impression was the whole verification, and what that verified was that
# the boot reached the end.
#
# It had already cost something. The kernel measures its own LAPIC timer,
# decides the answer is wrong, and says so — in the same voice as the 128
# lines that passed, so nobody counted it. Same shape as #448, where the
# coverage map named a test that had never existed: a measurement that is
# taken and never read.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=$REPO/uros/build-x86_64
LOG=${UROS_X86_64_LOG:-$HOME/uros-tests/run-x86_64.log}

# ------------------------------------------------------------------ verdict
#
# Split out so it can judge a log it did not produce:
#
#	run-x86_64.sh --judge <log>
#
# which is how the passing arm gets exercised at all. Every real run of this
# kernel currently trips the timer line, so without this the "exits zero
# otherwise" half of the contract would never once have executed -- and a
# verdict that has only ever said no is not a verdict that has been tested.
# It is also useful on its own: an old log can be re-judged when the list of
# known exceptions changes, instead of re-running the boot.
#
# Known-emulator exceptions. Named, with the reason, and COUNTED in the
# output rather than skipped: a verdict that fails on every run is worth
# exactly as much as one that never fails — both get ignored — and a silent
# skip is how a real regression hides behind a known one.
#
# The TSC one is a property of QEMU, which does not offer an invariant
# timestamp counter even with -cpu max (measured 27/07 on both cpu models).
# #318 was rewritten around that fact.
KNOWN='timestamp counter measured against the 8254'

# There are two kinds of run now, and they end differently (#458).
#
# The `-D' run ends in the double-fault self-test, which breaks the stack
# deliberately so the fault lands on its own IST.  `no handler — halted' is
# its success terminator, not a crash, and checking that it arrived is what
# catches a truncated run -- which produces fewer lines and no failures at
# all, and reads as a pass under any naive "grep for problems" check.
#
# The ordinary run enters the machine-independent kernel, and there
# `no handler' means exactly what it says.  A trap with no handler is how a
# kernel that has never run announces the first thing it got wrong.
#
# ⚠️ This distinction is not decoration.  The first boot into setup_main
# faulted in ipc_hash_init and this script called it PASSED, because the
# terminator it was looking for had arrived -- from a page fault instead of
# from the test that used to produce it.  A verdict that says yes to the run
# it was written to judge, and yes to a run it has never seen, is not a
# verdict.
TERMINATOR='no handler'
MI_ENTRY='entering setup_main'

# Which run announces that `no handler' is where it meant to stop (#426).
#
# ⚠️ The distinction above was drawn and then used in ONE direction: a run that
# reached setup_main and then trapped was judged, and a run that trapped BEFORE
# setup_main fell through the else and was reported as passed -- because the
# terminator it was looking for had arrived, from a trap instead of from the
# test that produces it.  Which is the same sentence the comment above warns
# about, in a branch it did not cover.
#
# It cost a run: an unexpected debug exception halted the machine in the middle
# of the machine-dependent self-tests, the log ended `no handler — halted', and
# the verdict said `passed: reached the end, nothing unexplained' over 82 of the
# 155 lines a good boot prints.
#
# Read from the LOG and not from --entry, so that --judge on an old log reaches
# the same verdict as the run that produced it.
DOUBLE_FAULT='breaking the stack on purpose'

# Where the machine-independent kernel currently stops, and why that is not a
# failure: it reaches bootstrap_create() and finds no userland bundle, because
# nothing loads one for this target yet (#422).  Named here so it is EXCUSED
# rather than ignored -- the day a bundle is loaded, this line disappears from
# the log and the check below starts asking for the next thing instead of
# silently continuing to pass.
EXPECTED_END='No bootstrap code loaded with the kernel'

verdict() {
TESTS=$(grep -ac 'UrMach x86-64:' "$LOG" || true)
EXCUSED=$(grep -a 'WRONG' "$LOG" | grep -ac "$KNOWN" || true)
# ⚠️ `^panic\(', not `^panic:'.  This kernel prints `panic(cpu 0): ...' --
# kern/debug.c puts the processor number in parentheses -- so the old pattern
# could never match a single panic this kernel has ever produced.  It was
# checked against nothing for as long as nothing panicked (#458).
# ⚠️ `panic(cpu' matches ANYWHERE in the line, not only at the start (#461).
#
# The pattern was anchored, and the day several processors panicked at once
# that anchor was the difference between a verdict and nothing. Three
# application processors failed identically in the same microsecond, their
# reports came out interleaved character by character, and not one line began
# with `panic'. The run was reported as PASSED.
#
# The interleaving itself is fixed -- x86_64/trap/trap.c serialises the report
# now -- but a verdict that only works while the output is tidy is a verdict
# that fails exactly when the machine is in the worst trouble. Unanchored, it
# survives a shredded log.
BAD=$(grep -aE 'WRONG|FAIL|Assertion failed|^panic[:(]|panic\(cpu|kernel: page fault' "$LOG" \
	| grep -av "$KNOWN" | grep -av "$EXPECTED_END" || true)
NBAD=$(test -n "$BAD" && printf '%s\n' "$BAD" | wc -l || echo 0)

echo
echo "=== verdict: $TESTS self-tests ==="
[ "$EXCUSED" -gt 0 ] && echo "  $EXCUSED excused: known QEMU artifact (TSC not invariant, #318)"

if grep -aq "$MI_ENTRY" "$LOG"; then
	# The machine-dependent self-tests all passed -- setup_main is reached
	# only after them -- so from here the subject is the kernel.
	if grep -aq "$TERMINATOR" "$LOG"; then
		echo "  FAILED: trapped inside the machine-independent kernel with no"
		echo "          handler.  Everything before setup_main passed; what"
		echo "          follows is the first thing this kernel got wrong:"
		awk "/$MI_ENTRY/,0" "$LOG" | sed -n '2,20p' | sed 's/^/    /'
		echo "  log: $LOG"
		exit 1
	fi
	# ⚠️ The excuse that used to live here is GONE, and its removal is the
	# point (#422).  It read "stopped at bootstrap_create -- no userland
	# bundle is loaded for this target yet", and a boot that stops there now
	# is a regression, not a known state.  An excuse outliving the condition
	# it excuses is how a harness reports a failure as a pass.
	if grep -aq "$EXPECTED_END" "$LOG"; then
		echo "  FAILED: stopped at bootstrap_create.  A 64-bit boot image"
		echo "          loads and runs since #422 -- this used to be the"
		echo "          expected end and is now a regression."
		echo "  log: $LOG"
		exit 1
	fi
	# ⚠️ Unless this boot IS the preemption test, on a machine with one
	# processor (#461).
	#
	# There the test is started from load_context() and takes the place of
	# the first thread -- deliberately, because the ordinary first thread
	# reaches bootstrap_create and panics there in less time than the test
	# needs -- so `startup: first thread running' never prints and cannot.
	# Demanding it of that boot is demanding the one thing it was arranged
	# not to do, and the test's own verdict below is its contract instead.
	if ! grep -aq 'startup: first thread' "$LOG" \
	   && ! grep -aq 'preempt_test: starting' "$LOG"; then
		echo "  FAILED: entered setup_main and neither reached the first"
		echo "          thread nor trapped -- it is wedged, which is the case"
		echo "          #428 exists for"
		echo "  log: $LOG"
		exit 1
	fi
elif grep -aq "$DOUBLE_FAULT" "$LOG"; then
	# The double-fault self-test, which is terminal by design: it never
	# reaches setup_main and `no handler' is how it succeeds.
	if ! grep -aq "$TERMINATOR" "$LOG"; then
		echo "  FAILED: the run never reached '$TERMINATOR' — it was cut short, so"
		echo "          the tests after that point did not run and cannot have passed"
		echo "  log: $LOG"
		exit 1
	fi
elif grep -aq "$TERMINATOR" "$LOG"; then
	echo "  FAILED: halted before entering the machine-independent kernel."
	echo "          A trap with no handler, in a run that is not the"
	echo "          double-fault self-test, is the first thing this kernel"
	echo "          got wrong:"
	grep -an "$TERMINATOR" "$LOG" | head -1 | cut -d: -f1 \
		| while read -r n; do
			sed -n "$((n > 24 ? n - 24 : 1)),${n}p" "$LOG" | sed 's/^/    /'
		done
	echo "  log: $LOG"
	exit 1
elif head -1 "$LOG" | grep -q '^qemu-system-x86_64: '; then
	# 🔴 A REFUSAL TO START IS NOT A RESULT, and it used to be reported as
	# one.  QEMU rejects a command line by printing one line and exiting,
	# and every check below reads a log that never had a kernel in it --
	# so the verdict came out as "the run was cut short, so the tests after
	# that point did not run", which is a statement about a boot that never
	# happened.  Six times in one session, each one investigated as though
	# the kernel were at fault (#517).
	#
	# 🔑 The two are different findings and must not share a sentence: one
	# says the kernel is wrong, the other says the CALLER is.  The line
	# that was believed for hours came from a shell that does not
	# word-split unquoted expansions, so `-smp 4' reached qemu as a single
	# argument.  Nothing about the kernel was involved.
	echo "  FAILED: qemu refused to start — this says nothing about the"
	echo "          kernel, and the command line is where to look:"
	sed -n '1,3p' "$LOG" | sed 's/^/    /'
	echo "    (arguments passed through: $RUN_ARGS)"
	echo "  log: $LOG"
	exit 3
elif [ ! -s "$LOG" ]; then
	# Started, said nothing at all.  A third thing again: not a refusal,
	# because qemu printed no complaint, and not a truncated boot, because
	# there is nothing to truncate.
	echo "  FAILED: qemu started and produced no output whatsoever — the"
	echo "          serial console never carried a byte, so the kernel was"
	echo "          never reached"
	echo "  log: $LOG"
	exit 3
else
	echo "  FAILED: the run reached neither setup_main nor an end it announces"
	echo "          — it was cut short, so the tests after that point did not"
	echo "          run and cannot have passed"
	echo "  log: $LOG"
	exit 1
fi

# A test boot has to reach its own verdict (#461).
#
# Entry 5 is the preemption test, and its failure mode is SILENCE by design:
# without preemption the first thread scheduled runs forever and prints
# nothing. That is only a failure if somebody is asking. Nobody was -- a run
# that announced the test and then stopped dead had every other box ticked
# (it entered setup_main, it reached the first thread, it trapped on nothing)
# and was reported as passed.
#
# So: having started, it must finish. This is the general shape -- a boot that
# exists to answer a question fails when the question goes unanswered.
#
# ── One mechanism, not one arm per test (#408) ─────────────────────────────
#
# This was written for preempt_test and then copied for the thread state
# dispatch, and the third test proved the copying was the bug: fpu_stress
# announced "3 threads on processor 1" on a machine that had dropped to
# battery, ran out of watchdog before its verdict, and was reported as PASSED,
# because nobody had copied the arm a third time. A rule that has to be
# restated for each new test is a rule that is missing for the newest one.
#
# So the list is the thing to add to, and it is one line per test.
must_report() {
	# $1 the line that says it started · $2 the line that says it finished
	# $3 what the silence means
	grep -aq "$1" "$LOG" || return 0
	if grep -aq "$2" "$LOG"; then
		echo "  ${1%%:*}: reported"
		return 0
	fi
	echo "  FAILED: '$1' appeared and '$2' never did."
	echo "          $3"
	echo "  log: $LOG"
	exit 1
}

must_report 'preempt_test: starting' 'preempt_test: PASS' \
	'Its failure mode is silence: a processor never taken from a thread that will not yield prints nothing at all (#459/#461).'
must_report 'fpu_stress: 3 threads' 'fpu_stress: PASS' \
	'The threads hold vector state for a second and then report; a boot cut short before that answers nothing (#408).'
must_report 'state_test: the target is parked' 'state_test: PASS — 11' \
	'thread_get_state() on a target that never stops waits for it for ever, and waiting for ever looks exactly like a short run (#408).'
must_report 'ast_test: arming AST_APC' 'ast_test: PASS' \
	'A kernel that takes AST_APC on a ring-0 return panics in the first round; one that hangs instead is the same defect with its quiet face (#463).'
# ⚠️ The terminator matches any count, not `3 of 3'.  A run that reported "2 of
# 3" DID finish and its failing arm is already caught as a WRONG line; asking
# here for the passing count as well would report one defect as two, and would
# report a cut-short run and a failed arm as the same thing.  This line asks
# only whether the program reached its own last sentence (#407).
# ⚠️ The arm COUNT is not in these patterns, deliberately.
#
# 🔥 It was: `act_test: [0-9] of 4 arms passed'.  Adding a fifth arm to
# act_test (#474) left the test passing and this harness unable to see that it
# had finished, so a green run reported "180s elapsed and the kernel never
# reached an end this script recognises".  The test and the harness were two
# halves that had to agree and nothing compared them -- which is the defect
# class this project keeps finding, arriving here in its own tooling.
#
# The count was never being CHECKED by these patterns; it was only part of the
# shape they matched.  Whether the arms passed is decided by the unexplained
# scan below, which reads WRONG lines.  So dropping it costs nothing and makes
# the coupling impossible rather than merely documented.
must_report 'cow_test: started' 'cow_test: [0-9]* of [0-9]* arms passed' \
	'It forks a task with inherit_memory, which is the first thing on this target ever to call vm_map_fork; a kernel that cannot do it dies inside task_create and prints nothing further (#407).'

must_report 'act_test: started' 'act_test: [0-9]* of [0-9]* arms passed' \
	'It was in NEITHER list until #425 -- not the one that ends the run and not the one that fails it for silence -- so it could start, stop dead, and be reported as a pass by omission.'

# 🔥 And one of its arms means less than it looks like on this host.
#
# act_test's sixth arm (#411) points a thread at a non-canonical address and
# expects an exception rather than a dead machine.  The hazard it guards is
# Intel's: SYSRET faults at CPL 0 there, on the caller's stack.  AMD faults in
# ring 3, where a fault is ordinary -- so on an AMD model the arm passes with
# the guard in entry.S present OR ablated, which was measured both ways.
#
# The default here is `-cpu max', which takes the vendor from the host.  On an
# AMD host that arm is confirming the outcome and not the guard, and it says so
# in its own line.  To make it a test, name an Intel model:
#
#	run-x86_64.sh 150 -cpu Skylake-Client
#
# With the guard ablated that run dies in a double fault reporting
# `instruction: sysret' with the kernel standing on a ring-3 stack, which is
# the whole of CVE-2012-0217 on one screen.

must_report 'netname_test: started' 'netname_test: [0-9]* of [0-9]* arms passed' \
	'It is the only client of the name server on this target (#426), so its silence means the RPC surface went quiet rather than that one arm disagreed -- and it runs second in the bundle, before the three programs that fault and kill threads on purpose, precisely so that a failure here cannot be blamed on them.'

# ⚠️ The terminator matches either verdict -- `ALL n TESTS PASSED' or `SOME
# TESTS FAILED' -- and deliberately does not demand the count.  This list asks
# whether the program reached its own last sentence; a failing arm already
# prints its own FAIL line and is caught by the pattern above.  Demanding 23
# here would report one defect as two, and would make a run where an arm was
# skipped look identical to a thread library that stopped dead (#425).
must_report 'pthread_test: starting' 'pthread_test: \(ALL [0-9]* TESTS PASSED\|SOME TESTS FAILED\)' \
	'Twenty-three arms of mutexes, condition variables, joins and thread-local storage (#425).  Its silence is a thread library that stopped, which on this target ends the boot rather than failing an arm -- and unlike a failing arm, that has no line of its own.'

must_report 'fault_test: started' 'fault_test: [0-9]* of [0-9]* arms passed' \
	'It is the last thing a bundle boot does, so it is also what tells this script the run is over (#489) -- a fault_test that starts and says nothing leaves the machine idling until the watchdog, which used to be reported as the run failing rather than as this test not answering.'

# ⚠️ The terminator matches either verdict, like pthread_test's above.
#
# 🔥 cap_test crossed in #495 and was in NEITHER list for exactly one run,
# which was enough: it started, printed two arms, went into a poll for a block
# device server this target does not have, and the run ended and called itself
# finished without it.  The verdict named five programs and cap_test was not
# among them -- passed by omission, which is the thing the note above act_test
# says this file exists to prevent, arriving again the next time the bundle
# grew.  🔑 Adding to the bundle and adding to these lists are one change.
must_report 'cap_test: starting' 'cap_test: \(ALL TESTS PASSED\|SOME TESTS FAILED\)' \
	'Its device arms poll for a block_device_server partition that has not crossed to this target, so its silence is indistinguishable from a run that ended early -- and the arms after them ([6]-[10]) test MIG and kernel refusals that need no device at all (#495).'

# 🔑 Added in the same change as the bundle entry, which is the lesson #495
# left here: cap_test went into the bundle and into neither list, and one run
# was enough for it to stop dead and be reported as a pass by omission.
must_report 'dl_test: starting' 'dl_test: [0-9]* of [0-9]* arms passed' \
	'It loads a shared object through libdl and follows pointers the loader relocated (#423).  A wrong relocation is a plausible pointer, so its failure mode is a fault partway through rather than a WRONG line -- which looks exactly like a run that ended early.'

# 🔑 Only the CHECKER instance is judged, and that is the whole arrangement:
# the bundle names this binary twice, and the first one exists to DIE holding
# the DMA region table so the second can find it given back (#513).  A hog that
# reported arms would be reporting on a fixture.
must_report 'dma_reclaim: started' 'dma_reclaim: [0-9]* of [0-9]* arms passed' \
	'It waits for the slots a dead task was holding, so on a kernel that does not release them it does not print a WRONG line -- it waits out its bound and then says so.  Silence instead means it never got that far.'

# 🔑 Its arm COUNT is not fixed, and that is deliberate (#427).  The sizes are
# checked on any board; the arms about a device above four gigabytes run only
# when one is placed there on purpose -- see the header of the test.  So the
# pattern asks for a verdict and not for a number: a run that reported "9 of 9"
# because the high device was absent is a true statement, and demanding a fixed
# count would turn it into a failure.
must_report 'hal_bar: started' 'hal_bar: [0-9]* of [0-9]* arms passed' \
	'It reads the region records the HAL measured and asks for a rescan.  A HAL that never probed does not fail loudly -- it reports regions of size zero -- so the absence of this verdict means the program did not reach the end, which is a different finding from a wrong size.'


# And the one that must report on EVERY boot that gets far enough, which is a
# different claim: it has no "started" line to pair with, because it runs
# unconditionally from machine_kernel_ready() on the path kern/startup.c must
# take. Silence there means it was not reached, and the reason is upstream of
# the test.
if grep -aq "$MI_ENTRY" "$LOG" && ! grep -aq 'state_test:' "$LOG"; then
	echo "  FAILED: entered setup_main and the thread state dispatch test"
	echo "          never reported.  It runs unconditionally from"
	echo "          machine_kernel_ready(), so silence means it was not"
	echo "          reached (#408)"
	echo "  log: $LOG"
	exit 1
fi

if [ "$NBAD" -gt 0 ]; then
	echo "  FAILED: $NBAD unexplained:"
	printf '%s\n' "$BAD" | sed 's/^/    /'
	echo "  log: $LOG"
	exit 1
fi

echo "  passed: reached the end, nothing unexplained"
}

if [ "${1:-}" = "--judge" ]; then
	[ -n "${2:-}" ] || { echo "usage: $0 --judge <log>" >&2; exit 2; }
	LOG=$2
	verdict
	exit $?
fi


# --entry N picks the GRUB menu entry, and therefore which kind of run this is
# (#458).  Entry 0 is the ordinary boot, which now goes on into the
# machine-independent kernel; entry 2 is the terminal double-fault self-test,
# which cannot share a boot with it.  Without this the other entries are
# unreachable, because grub.cfg has timeout=0.
#
# ── --iommu intel|amd: put remapping hardware on the board (#432) ──────────
#
# 🔴 IT PUTS THE ENGINE THERE AND DOES NOT TURN TRANSLATION ON.  That is `-I'
# on the kernel command line, which is menu entry 16 -- and keeping the two
# separate is the whole point, because #432 asks for the cost of translation
# to be measured rather than assumed and a measurement needs two runs that
# differ in ONE thing.
#
#	./scripts/run-x86_64.sh --iommu intel --entry 14 200   translation off
#	./scripts/run-x86_64.sh --iommu intel --entry 16 200   translation on
#
# Same board, same devices, same emulated hardware doing the same work per
# DMA; the only difference is whether the kernel programmed the engine.
# Comparing entry 16 against a run with NO vIOMMU device would measure the
# emulator's device model as well, and report the sum as the IOMMU's cost.
#
# 🔴🔴 AND `amd-iommu' DOES NOT REMAP DMA UNLESS ASKED.  Its `dma-remap'
# property defaults to OFF, while `intel-iommu' remaps with no options at all
# -- so the plain device accepts the device table, the command buffer and the
# event log, completes the invalidation commands we queue, and then lets every
# DMA through.  That cost three ablations that were each correct and each
# useless, because all three were ablating a mechanism that was disarmed.  It
# is spelt out here so it is spelt right every time.
#
# ⚠️ q35 is added only when the caller named no machine.  `intel-iommu'
# requires it; a caller who chose i440fx on purpose gets their board and
# qemu's own refusal, which is a clearer answer than this quietly overriding
# them.
# ⚠️ A LOOP AND NOT TWO `if's IN A ROW, because the order the two flags are
# typed in is not something a caller should have to remember.  Written as two
# sequential tests, `--iommu intel --entry 16' would have silently left
# --entry unread and booted the default menu entry -- a run that looks fine and
# answers a different question, which is the failure this file exists to stop.
IOMMU_ARGS=""
IOMMU_NAME="none"

while :; do
	case "${1:-}" in
	--entry)
		[ -n "${2:-}" ] || { echo "usage: $0 [--iommu intel|amd] [--entry <n>] [seconds] [qemu args]" >&2; exit 2; }
		UROS_X86_64_BOOT_ENTRY=$2
		export UROS_X86_64_BOOT_ENTRY
		shift 2 ;;
	--iommu)
		case "${2:-}" in
		intel)	IOMMU_ARGS="-device intel-iommu" ;;
		amd)	IOMMU_ARGS="-device amd-iommu,dma-remap=on" ;;
		*)	echo "usage: $0 [--iommu intel|amd] [--entry <n>] [seconds] [qemu args]" >&2
			exit 2 ;;
		esac
		IOMMU_NAME=$2
		shift 2 ;;
	*)
		break ;;
	esac
done

# ── The seconds are a BACKSTOP, not the instrument (#408) ──────────────────
#
# This used to be `timeout $SECS qemu ...', so how long the caller guessed
# decided the verdict.  It decided one: fpu_stress announced its three threads,
# the machine dropped to battery and ran at 1.40 GHz instead of 3.99, the boot
# took three times as long, the watchdog cut it off before the answer and the
# run was reported as PASSED.
#
# Losing performance to the governor is expected.  Losing the ANSWER to it is
# not, and a verdict that depends on the processor's clock is not a verdict.
#
# So the run now ends when the KERNEL says it has ended, and the seconds only
# stop a machine that is genuinely wedged.  A slower processor makes the run
# take longer and changes nothing else -- which is the property the number
# never had.
#
# ⚠️ The terminators are exhaustive on purpose, and each one is a real end:
# the ordinary boot stops at bootstrap_create (#422), the double-fault boot
# halts, and each test boot prints its own verdict.  A run whose end is not on
# this list falls through to the deadline and is reported as CUT SHORT rather
# than judged -- silence about a run nobody watched to the end is the failure
# this whole file exists to stop.
# ⚠️ A BUNDLE BOOT ENDS HERE TOO, AND FOR A WHILE IT DID NOT (#489).
#
# `No bootstrap code loaded with the kernel' below is the end of a boot that
# found no userland to run, and the comment on EXPECTED_END says what happens
# the day one appears: this list "starts asking for the next thing".  #422
# brought that day, the line stopped being printed, and the next thing was not
# added -- so every bundle boot did all of its work, went idle, and was killed
# by the watchdog and reported as a failure.  A verdict that says no to a run
# that did everything asked of it teaches its reader to stop reading it.
#
# cow_test ends the run, because it is the LAST ENTRY IN THE BUNDLE.
#
# ❌ This was `fault_test' for a day, and getting it wrong is the useful part.
# The choice was made by reading one boot's log, where `fault_test: 3 of 3'
# happened to appear three lines after `cow_test: 3 of 3' -- and that is the
# order two concurrent tasks FINISHED in on that particular run, not the order
# the bundle starts them.  Adding pthread_test in front changed the timing,
# fault_test finished first, DONE_RE fired, and act_test and cow_test were cut
# off mid-run -- reported as `passed', which is precisely the failure this file
# exists to prevent (#425).
#
# 🔑 The marker must be the last thing the bundle STARTS, which is a property
# of bootstrap.conf and does not move with timing.  An interleaving is not an
# order.
#
# 🔑 And the rule about the two lists: EVERY test in must_report, which fails a
# run for staying silent, and only the last-started one here, which ends it.
# act_test was in neither until #425 -- it could stop dead and be passed by
# omission -- which is what a list nobody re-reads does.
DONE_RE='boot_probe: the 64-bit boot image is running|No bootstrap code loaded with the kernel|no handler|preempt_test: (PASS|WRONG)|fpu_stress: halting the machine|fpu_stress: [0-9]+ of|state_test: [0-9]+ of|ast_test: (PASS|WRONG)|cow_test: [0-9]+ of [0-9]+ arms passed|Assertion failed|panic\(cpu'

SECS=${1:-90}
[ $# -gt 0 ] && shift

# What the caller actually handed to qemu, kept for the verdict.
#
# ⚠️ In a variable and not read as "$*" where it is printed: the verdict runs
# inside a function, where "$*" is the FUNCTION's arguments and comes out
# empty.  Which is how the first version of the refusal message named no
# arguments at all -- in the one line whose whole job is to show the caller
# what it typed.
RUN_ARGS="$*"

# What the disk image is made of.  Not `uros_iso': that target builds these
# five and then spends a second and forty megabytes producing a CD that no
# longer boots anything.  The ISO still exists as a target, for the bare-metal
# and Bochs paths that have no disk.
ninja -C "$BUILD" mach_kernel boot_probe name_server_bin bootstrap_server \
	bootstrap_bundle >/dev/null

# One run at a time, enforced rather than remembered.
#
# The killall below is how this script guarantees a clean machine, and it is
# also how two of these destroy each other: each kills the other's qemu
# mid-boot, and both report processors that never arrived. That happened,
# and produced twelve consecutive "failures" of a kernel that was fine.
#
# So take a lock and refuse rather than interleave. A refusal is a fact; two
# runs fighting is a lie with a plausible shape.
LOCK=/tmp/uros-x86_64-run.lock
if ! mkdir "$LOCK" 2>/dev/null; then
	echo "another run-x86_64.sh is in flight ($LOCK) — refusing to interleave" >&2
	exit 2
fi
trap 'rmdir "$LOCK" 2>/dev/null' EXIT INT TERM

mkdir -p "$(dirname "$LOG")"
killall qemu-system-x86_64 2>/dev/null || true
echo "=== booting disk-x86_64.img (watchdog ${SECS}s, a backstop only) $* ==="

# The whole stream goes to the log; the console keeps seeing the lines it
# always showed. The verdict reads the log, not the filtered view, because a
# failure that the filter drops is exactly the one worth catching.
# A CPU model, unless the caller named one (#459).
#
# The default without -cpu is qemu64, which does NOT advertise the TSC
# deadline timer -- so the kernel would silently fall back to the LAPIC
# one-shot backend on every run and the deadline path would be collaudated by
# nobody.  `max' advertises it under either accelerator.
#
# Named explicitly rather than left to the default so that which backend ran
# is a property of the command line, not of whatever qemu picks this year.
case " $* " in
*" -cpu "*)	CPU_ARGS="" ;;
*)		CPU_ARGS="-cpu max" ;;
esac

# 🔥 And WHICH ACCELERATOR, said out loud (#477).
#
# This script has never passed -enable-kvm, so every x86-64 run this port has
# ever made was TCG -- while the comment above used to reason about what "KVM
# emulates in its in-kernel LAPIC", which is a sentence about a thing that was
# not happening.  Nobody noticed because nothing said.
#
# It matters beyond tidiness: TCG does not perform every check the hardware
# does.  It accepted an iretq whose SS.RPL did not match its CS.RPL, so a
# kernel that could not have entered ring 3 twice on a real processor booted
# here for months (#477).  A run under one is not evidence about the other.
#
# Still opt-in rather than the default: on a rare or SMP-timing defect,
# accelerating CHANGES the experiment, and those hunts want TCG.  What changes
# here is only that the answer is on the screen instead of in someone's head.
case " $* " in
*" -enable-kvm "*|*" -accel "*)	ACCEL="KVM (asked for on the command line)" ;;
*)				ACCEL="TCG (no -enable-kvm; add it to use the host)" ;;
esac
echo "=== accelerator: $ACCEL ==="

# The board, when --iommu asked for an engine and the caller named none.
#
# ⚠️ Only then.  `intel-iommu' requires q35 and the default `pc' is a 1996
# i440FX; a caller who named a machine on purpose gets theirs, and qemu's own
# refusal, which says more than this silently overriding them would.
if [ -n "$IOMMU_ARGS" ]; then
	case " $* " in
	*" -machine "*|*" -M "*)	: ;;
	*)				IOMMU_ARGS="-machine q35 $IOMMU_ARGS" ;;
	esac
fi

# 🔑 Said out loud for the same reason the accelerator is (#477): a run whose
# board is not on the screen is a result somebody will later have to guess the
# conditions of.  "none" is an answer -- entry 16 on a board with no engine
# builds tables and enables nothing, which is a real thing to want and a
# terrible thing to mistake for translation being on.
echo "=== iommu on the board: $IOMMU_NAME ==="

: > "$LOG"

# shellcheck disable=SC2086
# ── The boot disk (#520) ──────────────────────────────────────────────
#
# 🔑 IT IS THE ONLY THING QEMU IS GIVEN.  GRUB lives in this image's MBR and
# reads the kernel and the bundle out of its one ext2 partition; the block
# device server then reads /mach_servers off the SAME partition, through
# virtio_blk.so, once the bundle has started it.  There is no -cdrom, no
# -kernel and no -initrd: nothing outside the disk takes part in the boot.
#
# ⚠️ Which is why `-boot d' is gone.  It was here because the disk had an MBR
# with no boot code and QEMU, preferring the hard disk, would stop at "Booting
# from Hard Disk..." having never touched the ISO.  The MBR has boot code now,
# and that stop is the desired behaviour.
#
# Rebuilt every run, not on demand.  --entry N is written INTO the image, as
# /boot/grub/entry.cfg, so a disk that is merely up to date with the binaries
# can still be selecting yesterday's menu entry.  It costs about a second.
"$REPO/scripts/make-disk-x86_64.sh" >&2

# 🔴 `bootindex' on both, because with two disks the boot device stops being
# obvious and starts being whatever SeaBIOS enumerates first.  Only one of
# them has GRUB in its MBR, and which one boots is not a thing to leave to
# enumeration order.
#
# The second disk is on ich9-ahci, the same controller i386 has used since
# #224, and it is here for #432: `virtio-blk-pci' is a transitional device and
# QEMU refuses `iommu_platform=on' on anything but a modern-only one, so the
# legacy virtio driver cannot be placed behind the IOMMU at all.  AHCI is an
# ordinary bus master with nothing to negotiate.
DISK_ARGS="-drive file=$BUILD/disk-x86_64.img,if=none,id=urosdisk,format=raw
	-device virtio-blk-pci,drive=urosdisk,bootindex=0
	-device ich9-ahci,id=ahci0
	-drive file=$BUILD/disk-x86_64-ahci.img,if=none,id=ahcidisk0,format=raw
	-device ide-hd,drive=ahcidisk0,bus=ahci0.0,bootindex=1
	-drive file=$BUILD/disk-x86_64-ahci2.img,if=none,id=ahcidisk1,format=raw
	-device ide-hd,drive=ahcidisk1,bus=ahci0.1,bootindex=2"

# shellcheck disable=SC2086
qemu-system-x86_64 $CPU_ARGS $DISK_ARGS $IOMMU_ARGS "$@" \
	-nographic -serial mon:stdio -no-reboot > "$LOG" 2>&1 &
QPID=$!

# Watch the log, not the clock.
#
# A grace second after the terminator, because the last lines of a panic or of
# a test verdict are still on their way out of the serial port when the line
# that matched arrives -- killing on the instant would truncate exactly the
# output somebody needs.
# ── The watchdog measures PROGRESS, not wall time (#425) ───────────────────
#
# 🔴 It measured seconds, and that made the verdict a function of the CPU
# governor.  The same kernel and the same images were reported FAILED at
# 1397 MHz and passed at 3988: the machine had gone onto battery between the
# two runs.  A verdict that changes with the clock rate is not a verdict, and
# #408 had already paid for this once -- fpu_stress announced its threads,
# ran out of watchdog before its answer, and was reported as PASSED.
#
# The question a watchdog should ask is not "have N seconds passed" but "is it
# still getting anywhere".  A slow machine makes progress slowly; a wedged one
# makes none.  So the deadline is pushed forward every time the log gains a
# line that MEANS something, and only a run that has gone quiet hits it.
#
# ⚠️ "Means something" has to exclude the idle chatter, or this never fires:
# quiet_census prints forever once the machine has nothing to do, so counting
# any output as progress would keep a wedged boot alive until the hard cap.
# What is excluded is named here rather than pattern-matched loosely -- an
# exclusion that grows silently is how a watchdog stops being one.
IDLE_CHATTER='quiet_census:'

# The hard cap, which is still wall time and still needed: a livelock that
# keeps printing meaningful lines forever is progress by this measure and has
# to end somehow.  Ten times the asked-for seconds, so it is never the thing
# that decides an ordinary run.
HARD_DEADLINE=$(( $(date +%s) + SECS * 10 ))
DEADLINE=$(( $(date +%s) + SECS ))

# How many meaningful lines the log holds.
#
# 🔴 `|| true', never `|| echo 0'.  grep -c EXITS 1 when the count is zero and
# PRINTS the zero anyway, so the fallback appended a SECOND number and the
# variable became "0\n0".  Here that happened on every boot -- the log is empty
# until qemu writes its first line -- and it poisoned LAST_PROGRESS for the
# whole run: every `-gt' below then failed with "integer expected", the
# deadline was never pushed forward, and the watchdog silently degraded back to
# the fixed number of seconds it exists to replace.
#
# ⚠️ It said so, ~35 times a run, in every log since this watchdog was written,
# and nobody read it.  A guard that is always broken is worse than no guard,
# because the runs that do not need it still pass and hide that it is gone.
progress_count() {
	_n=$(grep -avc "$IDLE_CHATTER" "$LOG" 2>/dev/null || true)
	[ -n "$_n" ] || _n=0
	echo "$_n"
}
LAST_PROGRESS=$(progress_count)
CUT_SHORT=0
# ── Every test that STARTED must have reported (#425) ──────────────────────
#
# 🔑 A single end marker cannot be right here, and two days were spent picking
# better ones before that became clear.  The bundle starts its tasks in order
# and they run CONCURRENTLY: which one prints its last line first is a
# property of the run, not of the configuration.  `fault_test' was chosen
# because it finished last in one log; adding pthread_test in front made
# cow_test finish last; making cow_test the marker then cut off act_test,
# which was still going.  An interleaving is not an order, and no amount of
# choosing better markers fixes a question asked of the wrong thing.
#
# So do not choose.  A run is over when everything that announced itself has
# also concluded -- which is the same set must_report judges afterwards, asked
# a few seconds earlier so the machine is not killed mid-sentence.
all_reported() {
	# $1 the "started" pattern · $2 the "finished" pattern, in pairs
	while [ $# -ge 2 ]; do
		if grep -aq "$1" "$LOG" && ! grep -aq "$2" "$LOG"; then
			return 1
		fi
		shift 2
	done
	return 0
}

while kill -0 "$QPID" 2>/dev/null; do
	# ⚠️ Same patterns as must_report above, and the counts are out of these
	# for the same reason -- see the note there.  🔥 That they are written
	# TWICE in this file is its own hazard: the first copy was corrected for
	# act_test's fifth arm and this one would have gone on waiting.
	if grep -aqE "$DONE_RE" "$LOG" \
	   && all_reported \
		'netname_test: started' 'netname_test: [0-9]* of [0-9]* arms passed' \
		'pthread_test: starting' 'pthread_test: \(ALL [0-9]* TESTS PASSED\|SOME TESTS FAILED\)' \
		'fault_test: started'   'fault_test: [0-9]* of [0-9]* arms passed' \
		'act_test: started'     'act_test: [0-9]* of [0-9]* arms passed' \
		'cap_test: starting'    'cap_test: \(ALL TESTS PASSED\|SOME TESTS FAILED\)' \
		'dl_test: starting'     'dl_test: [0-9]* of [0-9]* arms passed' \
		'dma_reclaim: started'  'dma_reclaim: [0-9]* of [0-9]* arms passed' \
		'hal_bar: started'      'hal_bar: [0-9]* of [0-9]* arms passed' \
		'cow_test: started'     'cow_test: [0-9]* of [0-9]* arms passed'; then
		sleep 1
		break
	fi
	NOW_PROGRESS=$(progress_count)
	if [ "$NOW_PROGRESS" -gt "$LAST_PROGRESS" ]; then
		LAST_PROGRESS=$NOW_PROGRESS
		DEADLINE=$(( $(date +%s) + SECS ))
	fi
	if [ "$(date +%s)" -ge "$DEADLINE" ] \
	   || [ "$(date +%s)" -ge "$HARD_DEADLINE" ]; then
		CUT_SHORT=1
		break
	fi
	sleep 0.2
done

kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

grep -a "UrMach\|fault\|error\|Error\|panic\|^  " "$LOG" || true

# Said here rather than left to the verdict, because it is a fact about the
# OBSERVATION and not about the kernel: nothing was judged, because nothing was
# watched to the end.
if [ "$CUT_SHORT" = 1 ]; then
	echo
	echo "=== verdict ==="
	echo "  FAILED: ${SECS}s elapsed and the kernel never reached an end this"
	echo "          script recognises.  That is a run nobody watched to the"
	echo "          end, not a run that passed -- if the machine is simply"
	echo "          slow (check the governor: this one drops to 1.4 GHz on"
	echo "          battery) give it more seconds; if it is wedged, that is"
	echo "          the bug."
	echo "  log: $LOG"
	exit 1
fi

verdict
