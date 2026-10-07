#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ablate.sh -- take a fix out, boot the tree, and say whether its test caught it.
#
# The ablations live in ablations/ as patches, not in the kernel; the format is
# in ablations/README.md.  This applies them to a worktree of the tree asked
# about, builds it with the main build's configuration, boots it with
# run-x86_64.sh, and reads each log for what the ablation says must be there.
#
# Usage:
#   ablate.sh [--base REV] [--boots N] [--accel tcg,kvm] [--ab] [--keep] PATCH... [-- RUN-ARGS...]
#   ablate.sh --check [--base REV] PATCH...
#   ablate.sh --plan [--base REV] [--boots N] [--accel L] [--ab] PATCH... [-- RUN-ARGS...]
#   ablate.sh --make COMMIT PATH...
#
#   --base REV   the tree to take the fix out of (default HEAD)
#   --boots N    boots per accelerator and arm (default 1)
#   --accel L    accelerators, comma-separated, of tcg and kvm (default tcg)
#   --ab         alternate every ablated boot with a boot of the fixed tree:
#                the widen patches, without the ablations
#   --keep       keep the worktrees and their builds afterwards
#   --check      only say whether every patch still applies to REV
#   --plan       say what the same command without --plan would build and
#                boot, and refuse what it would refuse, building nothing
#   --make       print the reverse of COMMIT's changes to the PATHs, under a
#                header to fill in
#   RUN-ARGS     what run-x86_64.sh is given after the accelerator; the
#                default is the patches' Run: line
#
# Exit status: 0 every ablated boot was caught and every fixed boot passed ·
# 1 at least one was not · 2 a patch does not apply, or the command line is
# wrong · 3 a tree did not build, or the harness could not run a boot -- which
# says nothing about the test.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
DIR=${UROS_ABLATE_DIR:-$HOME/uros-tests/ablations}
# The main build, whose configuration each arm takes: the one the harness would
# boot for this tree, so UROS_BUILD_DIR when the caller has set it (#656).
REF=${UROS_ABLATE_REF_BUILD:-${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}}

die() {
	echo "ablate: $2" >&2
	exit "$1"
}

# The values of one header field, one per line.  The header is everything
# before the first `diff --git', which git apply skips.
header() {
	sed -n -e '/^diff --git /q' -e "s/^$2:[[:space:]]*//p" "$1"
}

if [ "${1:-}" = --make ]; then
	shift
	[ $# -ge 2 ] || die 2 "usage: $0 --make COMMIT PATH..."
	commit=$1
	shift
	git -C "$REPO" rev-parse -q --verify "$commit^{commit}" >/dev/null ||
		die 2 "no commit $commit"
	cat <<EOF
Kind: ablation
Issue: #
Takes out: what $(git -C "$REPO" rev-parse --short "$commit") did, in one line
Run: --entry N SECONDS -smp 4
Expect: an extended regular expression a line of the log must match

EOF
	git -C "$REPO" diff "$commit" "$commit^" -- "$@"
	exit 0
fi

BASE=HEAD
BOOTS=1
ACCEL=tcg
AB=0
KEEP=0
CHECK=0
PLAN=0
PATCHES=()
RUN=()
while [ $# -gt 0 ]; do
	case "$1" in
	--base|--boots|--accel)
		[ $# -ge 2 ] || die 2 "$1 needs a value"
		case "$1" in
		--base)		BASE=$2 ;;
		--boots)	BOOTS=$2 ;;
		--accel)	ACCEL=$2 ;;
		esac
		shift 2 ;;
	--ab)		AB=1; shift ;;
	--keep)		KEEP=1; shift ;;
	--check)	CHECK=1; shift ;;
	--plan)		PLAN=1; shift ;;
	--)		shift; RUN=("$@"); break ;;
	-*)		die 2 "unknown option $1" ;;
	*)		[ -e "$1" ] || die 2 "no patch $1"
			PATCHES+=("$(realpath "$1")"); shift ;;
	esac
done
[ ${#PATCHES[@]} -gt 0 ] || die 2 "no patch given"
case "$BOOTS" in
''|*[!0-9]*|0)	die 2 "--boots takes a number of boots, not '$BOOTS'" ;;
esac
IFS=, read -r -a ACCELS <<< "$ACCEL"
for acc in "${ACCELS[@]}"; do
	case "$acc" in
	tcg|kvm) ;;
	*) die 2 "--accel takes tcg and kvm, not '$acc'" ;;
	esac
done

REV=$(git -C "$REPO" rev-parse -q --verify "$BASE^{commit}") || die 2 "no revision $BASE"
SHORT=$(git -C "$REPO" rev-parse --short "$REV")

# Every patch says what it is, and the fixed arm takes only the widen ones.
WIDENS=()
ABLATIONS=0
for p in "${PATCHES[@]}"; do
	[ -r "$p" ] || die 2 "cannot read $p"
	grep -q '^diff --git ' "$p" || die 2 "$p is not a patch: it has no diff"
	kind=$(header "$p" Kind | head -n 1)
	case "$kind" in
	ablation)	ABLATIONS=$((ABLATIONS + 1)) ;;
	widen)		WIDENS+=("$p") ;;
	*)		die 2 "$p: its header has no 'Kind: ablation' or 'Kind: widen' line" ;;
	esac
done

mkdir -p "$DIR"

# Whether the patches apply to REV, in order, each on top of the ones before:
# checked on a scratch index, so nothing is checked out.  A patch that no
# longer applies has rotted, and is the moment to look at the test again.
check_applies() {
	local idx="$DIR/.index.$$" err="$DIR/.apply.$$" p bad=0

	GIT_INDEX_FILE=$idx git -C "$REPO" read-tree "$REV"
	for p in "$@"; do
		if GIT_INDEX_FILE=$idx git -C "$REPO" apply --cached --check "$p" 2>"$err"; then
			GIT_INDEX_FILE=$idx git -C "$REPO" apply --cached "$p"
			echo "ablate: $(basename "$p") applies to $SHORT"
		else
			echo "ablate: $(basename "$p") NO LONGER APPLIES to $SHORT -- regenerate it, and check that the test still catches the defect" >&2
			sed 's/^/    /' "$err" >&2
			bad=1
		fi
	done
	rm -f "$idx" "$err"
	return $bad
}

if [ $CHECK = 1 ]; then
	check_applies "${PATCHES[@]}" || exit 2
	exit 0
fi

[ $ABLATIONS -gt 0 ] || die 2 "no ablation among the patches: there is nothing for a test to catch"
check_applies "${PATCHES[@]}" || exit 2

if [ ${#RUN[@]} -eq 0 ]; then
	run=$(for p in "${PATCHES[@]}"; do header "$p" Run; done | head -n 1)
	[ -n "$run" ] || die 2 "no Run: line in the patches, and no RUN-ARGS after --"
	read -r -a RUN <<< "$run"
fi

EXPECT=$(for p in "${PATCHES[@]}"; do
	if [ "$(header "$p" Kind | head -n 1)" = ablation ]; then
		header "$p" Expect
	fi
done)
[ -n "$EXPECT" ] || die 2 "no Expect: line in the ablation patches: nothing would say the test caught it"

if [ $AB = 1 ]; then
	ARMS=(fixed ablated)
else
	ARMS=(ablated)
fi

# Everything a run refuses has been refused by now, before anything is built:
# a plan is the run up to here, and what it would do after (#656).
if [ $PLAN = 1 ]; then
	for p in "${PATCHES[@]}"; do
		what=$(header "$p" 'Takes out'; header "$p" 'Holds open')
		echo "ablate: plan: $(basename "$p"): $(header "$p" Kind | head -n 1)," \
			"$(header "$p" Issue | head -n 1): ${what:-says nothing of what it does}"
	done
	echo "ablate: plan: run-x86_64.sh is given: ${RUN[*]}"
	while IFS= read -r e; do
		echo "ablate: plan: caught if a line of the log matches: $e"
	done <<< "$EXPECT"
	if [ $AB = 1 ]; then
		echo "ablate: plan: the fixed arm is $SHORT with the ${#WIDENS[@]} widen patch(es), the ablated arm with all ${#PATCHES[@]}"
	else
		echo "ablate: plan: one arm, $SHORT with all ${#PATCHES[@]} patch(es)"
	fi
	echo "ablate: plan: $BOOTS boot(s) per arm under each of ${ACCELS[*]}, $((BOOTS * ${#ACCELS[@]} * ${#ARMS[@]})) in all"
	exit 0
fi

RUNDIR="$DIR/runs/$(date +%Y%m%d-%H%M%S)-$SHORT"
mkdir -p "$RUNDIR"
printf '%s\n' "$EXPECT" > "$RUNDIR/expect"

cleanup() {
	local arm

	[ $KEEP = 1 ] && return
	for arm in "${ARMS[@]}"; do
		[ -d "$RUNDIR/$arm" ] && git -C "$REPO" worktree remove --force "$RUNDIR/$arm"
	done
	return 0
}
trap cleanup EXIT

# The build is configured with every OSFMK_* and UROS_* option of the main
# build, because this tree's configuration cannot be reproduced from the
# defaults -- except the in-kernel ablations and widenings, which stay off.
configure() {
	local wt=$1 cache="$REF/CMakeCache.txt" gen bt l n rest t v opts=()

	[ -r "$cache" ] || { echo "no $cache to take the configuration from"; return 1; }
	gen=$(sed -n 's/^CMAKE_GENERATOR:INTERNAL=//p' "$cache")
	bt=$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$cache")
	while IFS= read -r l; do
		n=${l%%:*}
		rest=${l#*:}
		t=${rest%%=*}
		v=${rest#*=}
		case "$n" in
		UROS_ABLATE_*|UROS_WIDEN_*)
			[ "$v" = ON ] && echo "note: $n is ON in $REF, and is left off here"
			continue ;;
		esac
		opts+=("-D$n:$t=$v")
	done < <(grep -E '^(OSFMK|UROS)_[A-Z0-9_]*:(BOOL|STRING|UNINITIALIZED)=' "$cache" | grep -v '_LAST:')
	echo "configuring with ${#opts[@]} options of $REF"
	cmake -S "$wt/uros" -B "$wt/uros/build-x86_64" -G "$gen" -DCMAKE_BUILD_TYPE="$bt" "${opts[@]}"
}

# One worktree per arm: the ablated one takes every patch, the fixed one only
# the widen patches.
for arm in "${ARMS[@]}"; do
	wt="$RUNDIR/$arm"
	git -C "$REPO" worktree add -q --detach "$wt" "$REV"
	if [ "$arm" = ablated ]; then
		apply=("${PATCHES[@]}")
	else
		apply=("${WIDENS[@]}")
	fi
	for p in "${apply[@]}"; do
		git -C "$wt" apply "$p" || die 2 "$(basename "$p") does not apply to $SHORT"
	done
	configure "$wt" > "$RUNDIR/$arm-configure.log" 2>&1 ||
		die 3 "$arm: the configuration failed, see $RUNDIR/$arm-configure.log"
	ninja -C "$wt/uros/build-x86_64" > "$RUNDIR/$arm-build.log" 2>&1 ||
		die 3 "$arm: the tree does not build, see $RUNDIR/$arm-build.log"
	# Which kernel the arm built, by its code: the md5 of the whole file
	# changes from one build of the same tree to the next, the md5 of .text
	# does not, so the two arms' lines say whether their kernels differ.
	kernel="$wt/uros/build-x86_64/export/uros/boot/mach_kernel"
	objcopy -O binary --only-section=.text "$kernel" "$RUNDIR/$arm.text" 2>/dev/null ||
		die 3 "$arm: no kernel text to read at $kernel"
	echo "ablate: $arm tree built ($SHORT + ${#apply[@]} patches), kernel .text md5 $(md5sum < "$RUNDIR/$arm.text" | cut -c1-12)"
	rm -f "$RUNDIR/$arm.text"
done

CSV="$DIR/ablate.csv"
[ -s "$CSV" ] ||
	echo "date,time,host,base,patches,arm,boot,accel,run,clock_mhz,power,harness_status,verdict,log" > "$CSV"
names=$(for p in "${PATCHES[@]}"; do basename "$p" .patch; done | paste -sd+)
host=$(hostname)

caught=0 missed=0 passed=0 failed=0
for i in $(seq 1 "$BOOTS"); do
	for acc in "${ACCELS[@]}"; do
		for arm in "${ARMS[@]}"; do
			log="$RUNDIR/$arm-$acc-$i.log"
			kvm=()
			[ "$acc" = kvm ] && kvm=(--kvm)
			# 🔴 EACH ARM BOOTS ITS OWN BUILD (#656).  run-x86_64.sh takes
			# UROS_BUILD_DIR from the environment when it finds one there, and
			# the caller's environment reaches both arms.  scripts/ci/job.sh
			# exports it, so both arms would boot the one build it names: an
			# A/B of a tree against itself, which no log would show, because a
			# log does not say which tree it booted (#587).  So every boot is
			# handed its arm's build here.
			set +e
			UROS_BUILD_DIR="$RUNDIR/$arm/uros/build-x86_64" UROS_X86_64_LOG=$log \
				"$RUNDIR/$arm/scripts/run-x86_64.sh" "${kvm[@]}" "${RUN[@]}" \
				> "$RUNDIR/$arm-$acc-$i.out" 2>&1
			rc=$?
			set -e
			case $rc in
			2|3)	die 3 "$arm, $acc, boot $i: the harness could not run it (status $rc), see $RUNDIR/$arm-$acc-$i.out" ;;
			esac
			hit=0
			grep -Eqf "$RUNDIR/expect" "$log" && hit=1
			if [ "$arm" = ablated ]; then
				if [ $hit = 1 ]; then
					verdict=CAUGHT
					caught=$((caught + 1))
				else
					verdict="NOT CAUGHT"
					missed=$((missed + 1))
				fi
			elif [ $rc = 0 ] && [ $hit = 0 ]; then
				verdict=PASSED
				passed=$((passed + 1))
			else
				verdict=FAILED
				failed=$((failed + 1))
			fi
			clock=$(sed -n 's/.*clock in run: median \([0-9]*\)MHz.*/\1/p' "$log" | head -n 1)
			power=$(sed -n 's/.*host start:.* power=\([^ ]*\).*/\1/p' "$log" | head -n 1)
			echo "$(date +%F,%T),$host,$SHORT,$names,$arm,$i,$acc,\"${RUN[*]}\",$clock,$power,$rc,$verdict,$log" >> "$CSV"
			echo "ablate: $arm, $acc, boot $i: $verdict (harness status $rc, ${clock:-?} MHz, power ${power:-?})"
		done
	done
done

echo "ablate: ablated tree caught $caught of $((caught + missed))"
[ $AB = 1 ] && echo "ablate: fixed tree passed $passed of $((passed + failed))"
[ $missed = 0 ] && [ $failed = 0 ]
