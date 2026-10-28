#!/usr/bin/env sh
#
# both-accelerators.sh — run one target under TCG and under KVM, and report
# where they disagree (#516).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# Usage:
#   ./scripts/both-accelerators.sh x86-64 [--entry N] [seconds]
#   ./scripts/both-accelerators.sh i386   [seconds] [arguments for smoke-ush.sh]
#
# The seconds are the budget of ONE run, not of the two.
#
# The logs end up in ~/uros-tests/both-<target>-<accel>.log and stay there.
#
# ── Why it exists ─────────────────────────────────────────────────────
#
# 🔑 Every x86-64 result of this port was taken under TCG, and every i386
# result under KVM.  Neither target was ever measured under both, and each has
# seen only what the other never sees.
#
# The divergences known so far are THREE, and they do not all have the same
# shape:
#
#   #477  TCG accepts an `iretq' with SS.RPL != CS.RPL  ⇒ hides a defect
#   #432  TCG accepts a processor's store to its own APIC page
#         as if it were a message                       ⇒ hides a defect
#   #515  TCG does NOT raise #XF for an unmasked SIMD exception
#                                                       ⇒ INVENTS a defect
#
# 🔴 The first two are the permissive emulator: it lets through what the
# hardware would refuse, and the kernel looks sound.  The third is the
# opposite: it does not raise what the hardware raises, and a correct kernel
# looks broken.  The second form is worse for the habit, because it teaches
# ignoring a red arm.
#
# ── What it does NOT do ───────────────────────────────────────────────
#
# 🔴 It does not pick a winner.  A divergence is a finding, and resolving it
# by taking the accelerator that passes means throwing away the one thing this
# comparison produces.  The script prints both versions and stops.
# ⚠️ And neither of the two is the hardware: in #477 and #432 KVM was right,
# which is proof about THOSE two cases and not a ranking.  Only bare metal
# decides.
#
# ⚠️ KVM does not replace TCG.  On a rare defect or an SMP timing one,
# speeding up CHANGES the experiment, and those hunts want TCG.  This adds a
# run, it does not take one away.

set -u

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
OUT=${UROS_TESTS_DIR:-$HOME/uros-tests}
mkdir -p "$OUT"

TARGET=${1:-}
case "$TARGET" in
x86-64|i386)	shift ;;
*)		echo "usage: $0 x86-64|i386 [arguments]" >&2; exit 2 ;;
esac

# A backstop, and ONLY a backstop.
#
# 🔴 The real timeouts live inside the harness that waits: smoke-ush.exp knows
# whether it is waiting for a banner, a prompt or a command, and has a budget
# for each.  This one knows nothing -- it exists for the case where `expect'
# itself hangs -- so it must sit COMFORTABLY ABOVE theirs and never next to
# them.
#
# ⚠️ The first version set it at 1200, just above the boot budget I had raised
# to 900: a second judge that has seen nothing and cuts first.  Had it fired it
# would have truncated the run halfway through the commands and produced the
# very comparison between truncated runs this script prints a warning against.
# Measured afterwards: the boot costs 19 s under TCG and 22 under KVM, so the
# 900 was not needed and the problem was the choice of the number, not the
# number.
BUDGET=1800
case "${1:-}" in
[0-9]*)	BUDGET=$1
	[ "$TARGET" = i386 ] && shift ;;
esac

# ── The two runs ──────────────────────────────────────────────────────
#
# 🔴 WITHOUT `set -e', and that is not an oversight: a run that FAILS is the
# data, not an error of the script.  Stopping at the first red would mean never
# getting to compare the two, which is exactly the case where the comparison
# is needed most.
#
# ⚠️ In sequence and never in parallel: one QEMU at a time.  Two machines
# fighting over the host's CPU would change the timing of both, and the slow
# one of the two is the one that already struggles to fit in the budget.
# 🔴 ON i386 THE SMOKE IS COMPARED, not a bare boot, and the reason is that A
# RUN THAT DOES NOT END CANNOT BE COMPARED.
#
# run-x86_64.sh knows when its run is over: the kernel announces a terminator
# and the script waits for it.  run-qemu.sh does not -- it boots to ush's prompt
# and stays there, so every bare i386 run burns the budget and gets killed,
# under both accelerators.  Tried: 900 s per arm, TCG 124 and KVM 124, and the
# two "agreed" on a TRUNCATED set.
#
# The smoke is the only i386 run that ends by itself, because it is the only
# one that TYPES `shutdown'.  So it is also the only one that two accelerators
# can honestly compare.
TCG_LOG=$OUT/both-$TARGET-tcg.log
KVM_LOG=$OUT/both-$TARGET-kvm.log

echo "=== both-accelerators: $TARGET, TCG first ==="
case "$TARGET" in
x86-64)
	UROS_X86_64_LOG=$TCG_LOG "$SCRIPT_DIR/run-x86_64.sh" "$@"
	TCG_RC=$?
	;;
i386)
	timeout "$BUDGET" "$SCRIPT_DIR/smoke-ush.sh" --tcg "$@" \
		</dev/null >"$TCG_LOG" 2>&1
	TCG_RC=$?
	;;
esac

echo "=== both-accelerators: $TARGET, now KVM ==="
case "$TARGET" in
x86-64)
	# 🔴 --kvm, AND NOT `-enable-kvm' AT THE END (#567).  Everything after the
	# first positional argument goes to qemu, and the first positional is the
	# budget: with no seconds in "$@" the flag ended up IN THE BUDGET'S SLOT.
	# run-x86_64.sh accepted it, printed `budget: -enable-kvms', ran for
	# zero seconds and failed with "NOTHING ARRIVED" -- a sentence about a
	# kernel, for a qemu that was never started.  So this script's KVM arm
	# NEVER ran under KVM, and the comparison it printed was between an
	# accelerator and nothing.  Now the flag is an option and cannot end up
	# in any slot.
	UROS_X86_64_LOG=$KVM_LOG "$SCRIPT_DIR/run-x86_64.sh" --kvm "$@"
	KVM_RC=$?
	;;
i386)
	timeout "$BUDGET" "$SCRIPT_DIR/smoke-ush.sh" "$@" \
		</dev/null >"$KVM_LOG" 2>&1
	KVM_RC=$?
	;;
esac

# ── What is compared, and what is normalized ──────────────────────────
#
# The lines that STATE AN OUTCOME are compared, not the whole logs: two boots
# of the same image differ in addresses, cycles and microseconds without any
# of those differences being a divergence between accelerators.
#
# 🔴 And as LITTLE as possible is normalized, because normalization is the
# place where a comparison can lie.  Only two things go, both volatile by
# construction:
#
#   - hexadecimal addresses, which change between any two boots;
#   - numbers that carry a unit of time (us/op, ns/op, cycles, ms), because
#     KVM and TCG differ there by two orders of magnitude AND THAT IS
#     EXPECTED -- that is not the divergence this script looks for.
#
# ⚠️ BARE integers stay.  `code 16' and `code 19' differ by a number and are
# two different faults: deleting them would make the tool blind to exactly the
# shape of divergence #515 found.
findings() {
	grep -aE 'PASS|FAIL|WRONG|SKIPPED|panic|Assertion failed' "$1" 2>/dev/null \
	| sed -e 's/0x[0-9a-fA-F][0-9a-fA-F]*/0xX/g' \
	      -e 's/[0-9][0-9]*\( *\)\(us\/op\|ns\/op\|cycles\|ms\|us\)/N\1\2/g' \
	      -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//' \
	| sort -u
}

findings "$TCG_LOG" > "$OUT/.both-tcg.$$"
findings "$KVM_LOG" > "$OUT/.both-kvm.$$"

ONLY_TCG=$(comm -23 "$OUT/.both-tcg.$$" "$OUT/.both-kvm.$$")
ONLY_KVM=$(comm -13 "$OUT/.both-tcg.$$" "$OUT/.both-kvm.$$")
rm -f "$OUT/.both-tcg.$$" "$OUT/.both-kvm.$$"

echo
echo "=== both-accelerators: $TARGET ==="
echo "  TCG: exit $TCG_RC   log $TCG_LOG"
echo "  KVM: exit $KVM_RC   log $KVM_LOG"

# ⚠️ 124 is the code `timeout' kills with: that run did not finish, and its
# findings are what it managed to print.  Said here because a comparison between
# a whole run and a truncated one finds divergences that are only the
# truncation.
for _rc in "TCG:$TCG_RC" "KVM:$KVM_RC"; do
	case "$_rc" in
	*:124)	echo "  ⚠️ ${_rc%%:*} hit the ${BUDGET}s budget and was killed —"
		echo "     its findings are only what it printed before that, so a"
		echo "     disagreement below may be the truncation and not the"
		echo "     accelerator" ;;
	esac
done

if [ -z "$ONLY_TCG" ] && [ -z "$ONLY_KVM" ]; then
	# ⚠️ "Agree" and not "pass": the two can agree on a failure, and that is
	# a kernel defect both of them see -- the case easiest to read and the
	# easiest to confuse with a green.
	echo "  the two accelerators AGREE on every outcome line"
	echo "  (which says nothing about whether those outcomes were good)"
	exit 0
fi

echo
echo "  🔴 THE TWO ACCELERATORS DISAGREE — reported, not resolved"
[ -n "$ONLY_TCG" ] && { echo "  only under TCG:"; printf '%s\n' "$ONLY_TCG" | sed 's/^/    /'; }
[ -n "$ONLY_KVM" ] && { echo "  only under KVM:"; printf '%s\n' "$ONLY_KVM" | sed 's/^/    /'; }
echo
echo "  Neither side is the answer.  Both are emulators of a machine that is"
echo "  not here, and only bare metal settles which one was right."
exit 3
