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
	if grep -aq "$EXPECTED_END" "$LOG"; then
		echo "  excused: stopped at bootstrap_create -- no userland bundle"
		echo "           is loaded for this target yet (#422)"
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
elif ! grep -aq "$TERMINATOR" "$LOG"; then
	echo "  FAILED: the run never reached '$TERMINATOR' — it was cut short, so"
	echo "          the tests after that point did not run and cannot have passed"
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
if grep -aq 'preempt_test: starting' "$LOG"; then
	if ! grep -aq 'preempt_test: PASS' "$LOG"; then
		echo "  FAILED: the preemption test started and never reported."
		echo "          Its failure mode is silence: a processor that is"
		echo "          never taken from a thread that will not yield"
		echo "          prints nothing at all (#459/#461)"
		echo "  log: $LOG"
		exit 1
	fi
	echo "  preemption test: PASS"
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

SECS=${1:-15}
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
echo "=== booting uros-x86_64.iso (${SECS}s watchdog) $* ==="

# The whole stream goes to the log; the console keeps seeing the lines it
# always showed. The verdict reads the log, not the filtered view, because a
# failure that the filter drops is exactly the one worth catching.
# A CPU model, unless the caller named one (#459).
#
# The default without -cpu is qemu64, which does NOT advertise the TSC
# deadline timer -- so the kernel would silently fall back to the LAPIC
# one-shot backend on every run and the deadline path would be collaudated by
# nobody.  `max' advertises it, and KVM emulates it in its in-kernel LAPIC
# even on hosts whose silicon lacks it (verified on this Zen 2 machine, which
# has no tsc_deadline_timer flag at all).
#
# Named explicitly rather than left to the default so that which backend ran
# is a property of the command line, not of whatever qemu picks this year.
case " $* " in
*" -cpu "*)	CPU_ARGS="" ;;
*)		CPU_ARGS="-cpu max" ;;
esac

# shellcheck disable=SC2086
timeout "$SECS" qemu-system-x86_64 $CPU_ARGS "$@" \
	-cdrom "$BUILD/uros-x86_64.iso" \
	-nographic -serial mon:stdio -no-reboot > "$LOG" 2>&1 || true
grep -a "UrMach\|fault\|error\|Error\|panic\|^  " "$LOG" || true

verdict
