#!/usr/bin/env bash
#
# xsave-ladder.sh — walk every rung of the XSAVE ladder, one at a time (#561).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# The point is not that a rung works: it is that no rung is merely WRITTEN.  A
# rung is reached by WITHHOLDING the feature of the one above it, so that
# between one arm and the next only the rung differs — not the accelerator, not
# the machine, not the kernel.
#
# Each arm has to do two things, and both are needed:
#   1. ANNOUNCE the rung it took.  If it announces a different one the
#      withholding did not work, and the arm measured something other than what
#      it says.
#   2. pass fpu_stress, which is the only test that can tell a correct compacted
#      header from a lucky one.
#
# ⚠️ THIS LADDER BELONGS TO THE MACHINE IT RAN ON.  The top rung needs a
# processor that offers xsaves, and the fleet does not agree: victus (Ryzen 5
# 5600H, Zen 3) has it, pavillion (Ryzen 5 4600H, Zen 2) has not, and qemu's TCG
# offers it under no model at all.  That disagreement is why the rung went
# unwalked by anybody until #561 was closed on the machine that had it.
#
# ⚠️ core2duo needs no hardware and no extra package: it is a CPU model built
# into qemu, reached by simply not asking for KVM.  It is also the only place
# the two bottom rungs exist, and — since it offers neither SMAP nor XSAVE — the
# machine that found #563.  Budget for it accordingly: a TCG boot costs ~200 s
# against KVM's ~30.
#
# Usage:  ./scripts/xsave-ladder.sh          every arm (~12 min)
#         ./scripts/xsave-ladder.sh kvm      the KVM arms only (fast)
#
# Exit:  0  every rung walked (expected announcement + fpu_stress passed)
#        1  a rung was NOT walked and the reason is unknown — a defect
#        2  a rung was not walked because the kernel DECLINES it by policy
#           (#561), which is a known reason and not a defect — but it is still a
#           rung nobody travels, and this script does not pretend otherwise
#
set -u

REPO=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." >/dev/null 2>&1 && pwd -P)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
OUT=${UROS_LADDER_OUT:-$BUILD/xsave-ladder}
ENTRY=6			# "vector state across preemption" — grub.cfg's -F arm
SECS_KVM=30
SECS_TCG=240

mkdir -p "$OUT" || exit 3

# name · accelerator · qemu arguments · expected rung
#
# 🔑 The order is the ladder's own, top to bottom, and each row withholds one
# feature more than the row above.  The three TCG rows are not duplicates of the
# KVM ones: TCG offers neither xsaves nor xsavec, so it is the only place the
# two bottom rungs exist at all.
ARMS=(
  "KVM, host features|kvm|-cpu max|XSAVES/XRSTORS"
  "KVM, xsaves withheld|kvm|-cpu max,-xsaves|XSAVEOPT/XRSTOR"
  "KVM, xsaves+xsaveopt withheld|kvm|-cpu max,-xsaves,-xsaveopt|XSAVEC/XRSTOR"
  "TCG, -cpu max|tcg|-cpu max|XSAVEOPT/XRSTOR"
  "TCG, xsaveopt withheld|tcg|-cpu max,-xsaveopt|XSAVE/XRSTOR"
  "TCG, core2duo|tcg|-cpu core2duo|FXSAVE/FXRSTOR"
)

only=${1:-all}
failures=0
unclean=0
declined=0
rows=()

for arm in "${ARMS[@]}"; do
	IFS='|' read -r name accel qargs expected <<<"$arm"

	if [ "$only" = kvm ] && [ "$accel" != kvm ]; then
		continue
	fi

	if [ "$accel" = kvm ]; then
		extra="-enable-kvm"; secs=$SECS_KVM
	else
		extra=""; secs=$SECS_TCG
	fi

	slug=$(printf '%s' "$name" | tr -c 'A-Za-z0-9' '-' | tr -s '-')
	log="$OUT/$slug.log"

	echo "── $name  ($accel, $qargs)" >&2
	UROS_X86_64_LOG="$log" timeout $((secs * 3)) \
		"$REPO/scripts/run-x86_64.sh" --entry "$ENTRY" "$secs" \
		$extra $qargs -smp 4 >"$OUT/$slug.stdout" 2>&1
	rc=$?

	# The rung taken, as fpu_stress itself names it in its own parentheses.
	announced=$(grep -aoE '(XSAVES/XRSTORS|XSAVEOPT/XRSTOR|XSAVEC/XRSTOR|XSAVE/XRSTOR|FXSAVE/FXRSTOR)' "$log" 2>/dev/null | head -1)
	[ -n "$announced" ] || announced="(no announcement)"

	if grep -aq 'fpu_stress: PASS' "$log" 2>/dev/null; then
		stress=PASS
	else
		stress=NO
	fi

	# 🔑 TWO QUESTIONS, TWO ANSWERS, AND THEY MUST NOT BE ADDED TOGETHER.
	#
	# "the rung was walked" = it announced the EXPECTED one and passed
	# fpu_stress.  Announcing a different rung means the withholding failed:
	# the arm walked the one above twice, and the rung it claimed to exercise
	# went unwalked.
	#
	# "the boot was clean" = rc 0, meaning no OTHER self-test complained.
	# That is a different question and it is kept apart: summing them either
	# hides a real defect inside a ladder arm, or — worse — declares unwalked
	# a rung that was walked perfectly well.  Measured: with -cpu core2duo the
	# FXSAVE rung is correct and passes, and the boot used to exit 1 for the
	# #468 SMAP self-test, which on a processor WITHOUT SMAP could not tell
	# "the window did not open" from "this machine has no windows" (#563).
	#
	# ⚠️ AND A FOURTH OUTCOME: the rung is not broken, it is DECLINED.
	# Since #561 `FPU_ALLOW_XSAVES' is 0 — the processor offers xsaves and the
	# kernel refuses it, having measured it indistinguishable from XSAVEOPT —
	# so in that build the top rung is UNREACHABLE, and calling it broken
	# would accuse the kernel of a defect that is a decision.
	#
	# 🔑 But it is not "ok" either, and this is the whole point of the script:
	# a rung nobody walked stays unwalked whatever the reason.  So it gets an
	# outcome of its own, it is visible, and this script NEVER says "all
	# walked" when one was not.
	#
	# The two cases are distinguishable because the kernel says them
	# differently: only when it has SEEN xsaves and refused it does it add the
	# parenthesis.  A bare XSAVEOPT on a machine that has xsaves would be
	# broken detection instead, and that rightly stays a failure.
	if [ "$announced" = "$expected" ] && [ "$stress" = PASS ]; then
		outcome=ok
	elif [ "$expected" = "XSAVES/XRSTORS" ] && [ "$stress" = PASS ] \
	     && grep -aq 'XSAVES present, measured and not chosen' "$log" 2>/dev/null; then
		outcome="declined (#561: knob at 0)"
		declined=$((declined + 1))
	else
		outcome=BROKEN
		failures=$((failures + 1))
	fi

	if [ $rc -eq 0 ]; then
		boot=clean
	else
		boot="rc=$rc ⚠️"
		unclean=$((unclean + 1))
	fi

	rows+=("$(printf '%-30s %-18s %-18s %-5s %-9s %s' \
		"$name" "$expected" "$announced" "$stress" "$boot" "$outcome")")
done

echo
echo "=== XSAVE ladder (#561) — $(hostname -s), $(date '+%Y-%m-%d %H:%M') ==="
printf '%-30s %-18s %-18s %-5s %-9s %s\n' arm expected announced test boot outcome
for r in "${rows[@]}"; do echo "$r"; done
echo
if [ $unclean -gt 0 ]; then
	echo "⚠️ $unclean boot(s) not clean: a self-test UNRELATED to the ladder"
	echo "   complained.  That is a finding of its own, not a missed rung — $OUT."
fi
if [ $declined -gt 0 ]; then
	echo "🔴 $declined rung(s) NOT walked because the kernel DECLINES them."
	echo "   Not a defect, it is #561's decision — but it is still a rung nobody"
	echo "   travels in this build.  To walk it: FPU_ALLOW_XSAVES to 1 in"
	echo "   x86_64/thread/fpu.c, rebuild, run again."
	exit 2
fi
if [ $failures -eq 0 ]; then
	echo "every rung WALKED: expected announcement and fpu_stress passed."
	exit 0
fi
echo "$failures arm(s) did not walk their own rung — see $OUT"
exit 1
