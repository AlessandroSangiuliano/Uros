#!/bin/sh
# UrMach x86-64 dev loop (#406+): rebuild kernel + ISO, boot in QEMU, and say
# whether the run was good.
#
# The image is ELF64 loaded by GRUB via multiboot2 — qemu's -kernel
# multiboot1 loader rejects ELF64, so we boot a GRUB rescue ISO with -cdrom.
#
# Usage: run-x86_64.sh [seconds] [extra qemu args...]
#   run-x86_64.sh
#   run-x86_64.sh 30 -smp 4
#   run-x86_64.sh 20 -cpu max -m 2G
#
# Exit status: 0 the run passed · 1 the run failed · 2 refused to start.
#
# The pass-through exists for a reason: running qemu by hand to add a flag
# skips the ISO rebuild, and the ISO is what boots. That mistake costs a
# debugging session chasing a kernel that was fixed twenty minutes earlier —
# so there is no reason to ever invoke qemu directly.
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
must_report 'cow_test: started' 'cow_test: [0-9] of 3 arms passed' \
	'It forks a task with inherit_memory, which is the first thing on this target ever to call vm_map_fork; a kernel that cannot do it dies inside task_create and prints nothing further (#407).'

must_report 'netname_test: started' 'netname_test: [0-9] of 2 arms passed' \
	'It is the only client of the name server on this target (#426), so its silence means the RPC surface went quiet rather than that one arm disagreed -- and it runs second in the bundle, before the three programs that fault and kill threads on purpose, precisely so that a failure here cannot be blamed on them.'

must_report 'fault_test: started' 'fault_test: [0-9] of 3 arms passed' \
	'It is the last thing a bundle boot does, so it is also what tells this script the run is over (#489) -- a fault_test that starts and says nothing leaves the machine idling until the watchdog, which used to be reported as the run failing rather than as this test not answering.'

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
if [ "${1:-}" = "--entry" ]; then
	[ -n "${2:-}" ] || { echo "usage: $0 --entry <n> [seconds] [qemu args]" >&2; exit 2; }
	UROS_X86_64_BOOT_ENTRY=$2
	export UROS_X86_64_BOOT_ENTRY
	shift 2
fi

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
# fault_test is the bundle's last line of work, so it is what ends the run.
#
# 🔴 cow_test is deliberately NOT here, and the reason is the direction of the
# two lists.  It finishes BEFORE fault_test -- `cow_test: 3 of 3' at log line
# 225, `fault_test: 3 of 3' at 228 -- so ending the run on it would cut the
# boot off three lines early and lose the result of the test after it.  That is
# the exact failure this file exists to stop, arrived at from the other side.
#
# 🔑 So the rule is not "both tests in both lists".  It is: EVERY test must be
# in must_report, which fails a run for staying silent, and only the LAST one
# may be in this list, which ends it.  Being in must_report and not here is
# correct for cow_test and was the defect for fault_test -- the same asymmetry
# reads as a bug or as the design depending on which test finishes last, which
# is why it needs saying rather than pattern-matching.
DONE_RE='boot_probe: the 64-bit boot image is running|No bootstrap code loaded with the kernel|no handler|preempt_test: (PASS|WRONG)|fpu_stress: halting the machine|fpu_stress: [0-9]+ of|state_test: [0-9]+ of|ast_test: (PASS|WRONG)|fault_test: [0-9]+ of [0-9]+ arms passed|Assertion failed|panic\(cpu'

SECS=${1:-90}
[ $# -gt 0 ] && shift

ninja -C "$BUILD" uros_iso >/dev/null

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
echo "=== booting uros-x86_64.iso (watchdog ${SECS}s, a backstop only) $* ==="

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

: > "$LOG"

# shellcheck disable=SC2086
qemu-system-x86_64 $CPU_ARGS "$@" \
	-cdrom "$BUILD/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot > "$LOG" 2>&1 &
QPID=$!

# Watch the log, not the clock.
#
# A grace second after the terminator, because the last lines of a panic or of
# a test verdict are still on their way out of the serial port when the line
# that matched arrives -- killing on the instant would truncate exactly the
# output somebody needs.
DEADLINE=$(( $(date +%s) + SECS ))
CUT_SHORT=0
while kill -0 "$QPID" 2>/dev/null; do
	if grep -aqE "$DONE_RE" "$LOG"; then
		sleep 1
		break
	fi
	if [ "$(date +%s)" -ge "$DEADLINE" ]; then
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
