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
# A patch with Screen: lines is booted by fbcons-readback.py instead, and what
# that script read off the screen is read the same way (#670).  One with
# Script: lines is run by the script they name, from the arm's tree and with
# its build, and what that script printed is read the same way (#618).
#
# A patch that must go over another says so with `Over: NAME.patch' in its
# header, and is applied over it whether or not it was given: the bases come
# first, each once.  A base is a widen the patch needs to mean anything -- an
# ablation whose Expect matched the fixed tree too without its widen reported
# CAUGHT on every boot when given alone (#659) -- a probe it extends, or a patch
# its diff is written over.  Only the patches given are read for Expect: lines:
# a base that is itself an ablation would otherwise catch the run on its own.
#
# A patch may say with `Premise: ERE' what each fixed boot must show for the
# ablated arm to be asked at all: what a widen holds open, or what the catch
# stands on, such as a window of time the test must still have.  It is read
# from every patch of the run, the bases too, and it brings the fixed arm in
# without --ab.  A fixed boot no line of whose log matches every premise is
# said to be without its premise and not passed, and an ablated boot beside it
# that is not caught is said not askable, not uncaught (#1010: 922-wait-whole-
# arm read NOT CAUGHT for 700 commits after a sleep of 10 ms came to last 20,
# its window with it, #1009).
#
# Usage:
#   ablate.sh [--base REV] [--boots N] [--accel tcg,kvm] [--ab] [--keep] PATCH... [-- RUN-ARGS...]
#   ablate.sh --check [--base REV] PATCH...
#   ablate.sh --check-all [--base REV]
#   ablate.sh --plan [--base REV] [--boots N] [--accel L] [--ab] PATCH... [-- RUN-ARGS...]
#   ablate.sh --make COMMIT PATH...
#
#   --base REV   the tree to take the fix out of (default HEAD)
#   --boots N    boots per accelerator and arm (default 1)
#   --accel L    accelerators, comma-separated, of tcg and kvm (default tcg)
#   --ab         alternate every ablated boot with a boot of the fixed tree:
#                the widen patches, without the ablations
#   --keep       keep the worktrees and their builds afterwards
#   --check      only say whether each patch still applies to REV, over its
#                bases, each on its own
#   --check-all  --check of every patch of this tree's ablations/, and a last
#                line saying whether all of them still apply, or naming the
#                ones that do not: the one answer ci/job.sh and
#                merge-test-x86_64.sh both ask for (#769)
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
# says nothing about the test · 4 no boot was uncaught or failed, and a fixed
# boot was without its premise: the ablation could not be asked there.  Sent SIGTERM, it stops what it is doing, the
# boot it is running or the build, says so -- a stopped boot has a row whose
# verdict is "stopped" -- prints what the boots it made said, removes its
# worktrees and ends by the signal (#951).
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
ALL=0
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
	--check-all)	CHECK=1; ALL=1; shift ;;
	--plan)		PLAN=1; shift ;;
	--)		shift; RUN=("$@"); break ;;
	-*)		die 2 "unknown option $1" ;;
	*)		[ -e "$1" ] || die 2 "no patch $1"
			PATCHES+=("$(realpath "$1")"); shift ;;
	esac
done
# #769: every patch of the tree's own ablations/, which is what a merge rots.
if [ $ALL = 1 ]; then
	[ ${#PATCHES[@]} -eq 0 ] || die 2 "--check-all takes no patch: it asks every one of ablations/"
	for p in "$REPO"/ablations/*.patch; do
		[ -e "$p" ] && PATCHES+=("$(realpath "$p")")
	done
	[ ${#PATCHES[@]} -gt 0 ] || die 2 "no patch in $REPO/ablations"
fi
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

# A patch, after the bases its Over: lines name (theirs first), each once.  A
# base the command line did not name is said: the run is not then the patches
# given alone.
with_bases() {
	local p=$1 b stray

	case " ${EXPANDED[*]} " in
	*" $p "*) return ;;
	esac
	case " ${VISITING[*]} " in
	*" $p "*) die 2 "$(basename "$p") goes over itself, through the Over: lines of its bases" ;;
	esac
	VISITING+=("$p")
	[ -r "$p" ] || die 2 "cannot read $p"
	grep -q '^diff --git ' "$p" || die 2 "$p is not a patch: it has no diff"
	# #935: a header line is a key and its value, or blank.  header() reads
	# keyed lines only, so a second Expect written as a bare line under the
	# first was no expectation at all: 935-start-after-return said NOT CAUGHT
	# of four boots, each stopped by a signature on such a line.  What nothing
	# would read is refused, not dropped.
	stray=$(sed -n -E -e '/^diff --git /q' -e '/^$/d' \
		-e '/^(Kind|Issue|Takes out|Holds open|Over|Run|Expect|Premise|Measure|Accel|Script|Screen|Note|Machine):/d' \
		-e p "$p" | head -n 1)
	[ -z "$stray" ] ||
		die 2 "$(basename "$p") has a header line with no key, which nothing reads: '$stray' -- one Expect: per line"
	for b in $(header "$p" Over); do
		[ -r "$(dirname "$p")/$b" ] ||
			die 2 "$(basename "$p") goes over $b, which is not next to it"
		b=$(realpath "$(dirname "$p")/$b")
		case " ${PATCHES[*]} ${EXPANDED[*]} " in
		*" $b "*) ;;
		*) echo "ablate: $(basename "$p") goes over $(basename "$b"), which comes in first (Over:)" ;;
		esac
		with_bases "$b"
	done
	EXPANDED+=("$p")
}
EXPANDED=()
VISITING=()
for p in "${PATCHES[@]}"; do
	with_bases "$p"
done

# A patch's Accel: line names the accelerators it can show anything under,
# and the run takes only those (#659): under KVM's full irqchip QEMU's plain
# intel-iommu remaps nothing, so 659-kept-mode-eight-bit's ablated tree has
# nothing to print there and was NOT CAUGHT on every KVM boot; and
# fbcons-readback.py's pauses restart KVM's in-kernel 8254 (#661).  A command
# line that leaves a patch none of its accelerators is refused -- but not a
# --check, which boots nothing: with 477-sysret-base's Accel: kvm, a check of
# every patch under the default tcg was refused whole (#643).
if [ $CHECK = 0 ]; then
	for p in "${EXPANDED[@]}"; do
		allow=$(header "$p" Accel | tr ',' ' ')
		[ -n "$allow" ] || continue
		kept=()
		for acc in "${ACCELS[@]}"; do
			case " $allow " in
			*" $acc "*)	kept+=("$acc") ;;
			esac
		done
		[ ${#kept[@]} -gt 0 ] ||
			die 2 "$(basename "$p") shows something only under $allow (its Accel:), and --accel asked for ${ACCELS[*]}"
		[ ${#kept[@]} = ${#ACCELS[@]} ] ||
			echo "ablate: $(basename "$p") is booted under ${kept[*]} only (Accel:)"
		ACCELS=("${kept[@]}")
	done
fi

# Every patch says what it is, and the fixed arm takes only the widen ones.
WIDENS=()
ABLATIONS=0
for p in "${EXPANDED[@]}"; do
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
ROTTED_NAMES=()
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
			ROTTED_NAMES+=("$(basename "$p")")
			bad=1
		fi
	done
	rm -f "$idx" "$err"
	return $bad
}

# Each patch on its own, over its bases: a patch is not checked over the
# unrelated ones given beside it, which may touch the same lines.
if [ $CHECK = 1 ]; then
	bad=0
	for p in "${PATCHES[@]}"; do
		EXPANDED=()
		VISITING=()
		with_bases "$p"
		check_applies "${EXPANDED[@]}" || bad=1
	done
	# #769: the answer, one line, each name once.  A widen is asked on its
	# own and again under every patch over it, and an ablation whose hunks
	# need its widen's lines fails with it and is named beside it, as
	# ci/job.sh named them.
	if [ $ALL = 1 ]; then
		if [ $bad = 0 ]; then
			echo "ablate: all ${#PATCHES[@]} patches of ablations/ still apply to $SHORT"
		else
			rotted=$(printf '%s\n' "${ROTTED_NAMES[@]}" | sort -u)
			echo "ablate: $(wc -l <<< "$rotted") of the ${#PATCHES[@]} patches of ablations/ no longer apply to $SHORT -- regenerate them: $(paste -sd' ' <<< "$rotted")"
		fi
	fi
	exit $((bad * 2))
fi

[ $ABLATIONS -gt 0 ] || die 2 "no ablation among the patches: there is nothing for a test to catch"

# #804: a patch that only a machine's metal can judge -- Intel's counters, which
# no QEMU here offers: victus and pavillion are AMD, and OMEGA's KVM gives no
# virtual PMU on its hybrid processor.  Booted here, both arms would print the
# same refusal and the ablation would read as not caught.  Its Note says how it
# was run there; --check-all still asks whether it applies.
for p in "${EXPANDED[@]}"; do
	machine=$(header "$p" Machine | head -n 1)
	[ -z "$machine" ] ||
		die 2 "$(basename "$p") is judged on $machine's metal (its Machine:), not under QEMU: its Note says how it was run"
done

check_applies "${EXPANDED[@]}" || exit 2

# #670: a test read off the screen.  Screen: lines, one argument of
# fbcons-readback.py each so that none needs quoting, boot every arm with that
# script and its build instead of with run-x86_64.sh, and Expect: is matched
# against what the script printed.  The way to catch an ablation on a machine
# with no UART, which has no serial log to read.
mapfile -t SCREEN < <(for p in "${EXPANDED[@]}"; do header "$p" Screen; done)

# #618: a test that is a script of its own.  Script: lines, the first the
# script's path in the tree and each one after it one argument, run every arm
# with that script of the arm's own tree, its build in UROS_BUILD_DIR and
# --kvm under KVM, and Expect: is matched against what it printed.  For what
# no single run's log can show: a run interrupted, and what it left behind.
mapfile -t SCRIPT < <(for p in "${EXPANDED[@]}"; do header "$p" Script; done)
[ ${#SCREEN[@]} -gt 0 ] && [ ${#SCRIPT[@]} -gt 0 ] &&
	die 2 "the patches have both Screen: and Script: lines: an arm is run one way"

if [ ${#RUN[@]} -eq 0 ] && [ ${#SCREEN[@]} -eq 0 ] && [ ${#SCRIPT[@]} -eq 0 ]; then
	run=$(for p in "${PATCHES[@]}" "${EXPANDED[@]}"; do header "$p" Run; done | head -n 1)
	[ -n "$run" ] || die 2 "no Run: line in the patches, and no RUN-ARGS after --"
	read -r -a RUN <<< "$run"
fi

EXPECT=$(for p in "${PATCHES[@]}"; do
	if [ "$(header "$p" Kind | head -n 1)" = ablation ]; then
		header "$p" Expect
	fi
done)
[ -n "$EXPECT" ] || die 2 "no Expect: line in the ablation patches: nothing would say the test caught it"

# #1010: what each fixed boot must show for the ablated arm to be asked, from
# every patch of the run, the bases too, each once.  A premise is read off a
# fixed boot, so it brings the fixed arm in when --ab was not given.
PREMISE=$(for p in "${EXPANDED[@]}"; do header "$p" Premise; done | awk '!seen[$0]++')
PREMISE_AB=0
if [ -n "$PREMISE" ] && [ $AB = 0 ]; then
	AB=1
	PREMISE_AB=1
fi

if [ $AB = 1 ]; then
	ARMS=(fixed ablated)
else
	ARMS=(ablated)
fi

# Everything a run refuses has been refused by now, before anything is built:
# a plan is the run up to here, and what it would do after (#656).
if [ $PLAN = 1 ]; then
	for p in "${EXPANDED[@]}"; do
		what=$(header "$p" 'Takes out'; header "$p" 'Holds open')
		echo "ablate: plan: $(basename "$p"): $(header "$p" Kind | head -n 1)," \
			"$(header "$p" Issue | head -n 1): ${what:-says nothing of what it does}"
	done
	if [ ${#SCREEN[@]} -gt 0 ]; then
		echo "ablate: plan: fbcons-readback.py is given: ${SCREEN[*]}"
		read_from="what fbcons-readback.py printed"
	elif [ ${#SCRIPT[@]} -gt 0 ]; then
		echo "ablate: plan: ${SCRIPT[0]} is run, given: ${SCRIPT[*]:1}"
		read_from="what ${SCRIPT[0]} printed"
	else
		echo "ablate: plan: run-x86_64.sh is given: ${RUN[*]}"
		read_from="the log"
	fi
	while IFS= read -r e; do
		echo "ablate: plan: caught if a line of $read_from matches: $e"
	done <<< "$EXPECT"
	if [ -n "$PREMISE" ]; then
		while IFS= read -r e; do
			echo "ablate: plan: asked only beside a fixed boot that shows, in $read_from: $e (Premise:)"
		done <<< "$PREMISE"
		[ $PREMISE_AB = 0 ] || echo "ablate: plan: the fixed arm is booted for the premise, though --ab was not given"
	fi
	if [ $AB = 1 ]; then
		echo "ablate: plan: the fixed arm is $SHORT with the ${#WIDENS[@]} widen patch(es), the ablated arm with all ${#EXPANDED[@]}"
	else
		echo "ablate: plan: one arm, $SHORT with all ${#EXPANDED[@]} patch(es)"
	fi
	echo "ablate: plan: $BOOTS boot(s) per arm under each of ${ACCELS[*]}, $((BOOTS * ${#ACCELS[@]} * ${#ARMS[@]})) in all"
	exit 0
fi

RUNDIR="$DIR/runs/$(date +%Y%m%d-%H%M%S)-$SHORT"
mkdir -p "$RUNDIR"
printf '%s\n' "$EXPECT" > "$RUNDIR/expect"
if [ -n "$PREMISE" ]; then
	printf '%s\n' "$PREMISE" > "$RUNDIR/premise"
	[ $PREMISE_AB = 0 ] || echo "ablate: the fixed arm is booted for the premise, though --ab was not given (#1010)"
fi

# #1010: the first premise no line of the log matches, or nothing.
premise_missing() {
	local pr
	[ -s "$RUNDIR/premise" ] || return 0
	while IFS= read -r pr; do
		LC_ALL=C.UTF-8 grep -Eq -- "$pr" "$1" || { printf '%s\n' "$pr"; return 0; }
	done < "$RUNDIR/premise"
}

cleanup() {
	local arm

	[ $KEEP = 1 ] && return
	for arm in "${ARMS[@]}"; do
		[ -d "$RUNDIR/$arm" ] || continue
		git -C "$REPO" worktree remove --force "$RUNDIR/$arm" ||
			echo "ablate: the $arm arm's worktree was not removed: git worktree remove --force $RUNDIR/$arm" >&2
	done
	return 0
}
# 🔴 A SECOND SIGNAL WAITS UNTIL THE WORKTREES ARE GONE (#801), as in
# run-x86_64.sh.  Stopping a unit signals what is left of it twice -- its main
# process, a script with no trap, dies on the first -- and the second ended
# the EXIT trap's git worktree remove: a run stopped in a boot left both arms,
# 2 runs of 2.  So INT, TERM and HUP are ignored while they go, and get their
# default back only for the kill that ends the script; and a removal that
# fails says which, where under set -e it ended the loop before the other arm.
stopped_by() {
	trap - EXIT
	trap '' INT TERM HUP
	if [ -n "$CHILD" ]; then
		kill -TERM "$CHILD" 2>/dev/null
		wait "$CHILD" 2>/dev/null
	fi
	cleanup
	echo "ablate: stopped by SIG$1" >&2
	trap - INT TERM HUP
	kill -s "$1" "$$"
}
# #951: SIGTERM is how ci/job.sh stops an A/B or a sweep at a campaign job's
# time, and it asks more of a run than INT and HUP do: the boot it stops is
# said, in a row of its own, and so is what the boots before it said.  So the
# trap passes it on to what the run is doing, a build or a boot, which is a
# job waited for so that the trap runs during it (#801), and the run stops
# where it is: before its first boot, after the build it was in; in a boot,
# once that boot has stopped; or between two, before the next.
CHILD=""
STOP=""
trap cleanup EXIT
trap 'stopped_by INT' INT
trap 'STOP=TERM; [ -z "$CHILD" ] || kill -TERM "$CHILD" 2>/dev/null' TERM
trap 'stopped_by HUP' HUP
# A long step, run as a job waited for: its status, or that of its stop.  A
# SIGTERM during the wait ends it at once, with 143 whatever the job did, and
# the job's own status is the next wait's; one that came before the job was
# known is passed on here.  The job gets SIGINT and SIGQUIT back, which an
# asynchronous command of a shell without job control starts with ignored: a
# Ctrl-C is its own to stop at, as it was in the foreground.  The boots below
# are started the same way.
waited() {
	local rc=0
	env --default-signal=INT,QUIT "$@" <&0 &
	CHILD=$!
	[ -z "$STOP" ] || kill -TERM "$CHILD" 2>/dev/null
	wait "$CHILD" || rc=$?
	if [ -n "$STOP" ] && [ $rc -gt 128 ]; then
		rc=0
		wait "$CHILD" || rc=$?
	fi
	CHILD=""
	return $rc
}
# A run asked to stop before its first boot ends here, its worktrees removed:
# nothing was booted, so nothing is said but where it stopped.
stop_before_boots() {	# what it was doing
	[ -z "$STOP" ] && return 0
	trap - EXIT
	trap '' INT TERM HUP
	cleanup
	echo "ablate: stopped by SIG$STOP while $1, before its first boot"
	trap - INT TERM HUP
	kill -s "$STOP" "$$"
}

# The build is configured with every OSFMK_* and UROS_* option of the main
# build, because this tree's configuration cannot be reproduced from the
# defaults.
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
		opts+=("-D$n:$t=$v")
	done < <(grep -E '^(OSFMK|UROS)_[A-Z0-9_]*:(BOOL|STRING|UNINITIALIZED)=' "$cache" | grep -v '_LAST:')
	echo "configuring with ${#opts[@]} options of $REF"
	waited cmake -S "$wt/uros" -B "$wt/uros/build-x86_64" -G "$gen" -DCMAKE_BUILD_TYPE="$bt" "${opts[@]}"
}

# One worktree per arm: the ablated one takes every patch, the fixed one only
# the widen patches.
for arm in "${ARMS[@]}"; do
	wt="$RUNDIR/$arm"
	git -C "$REPO" worktree add -q --detach "$wt" "$REV"
	if [ "$arm" = ablated ]; then
		apply=("${EXPANDED[@]}")
	else
		apply=("${WIDENS[@]}")
	fi
	for p in "${apply[@]}"; do
		git -C "$wt" apply "$p" || die 2 "$(basename "$p") does not apply to $SHORT"
	done
	configure "$wt" > "$RUNDIR/$arm-configure.log" 2>&1 ||
		{ stop_before_boots "configuring the $arm arm"; die 3 "$arm: the configuration failed, see $RUNDIR/$arm-configure.log"; }
	stop_before_boots "configuring the $arm arm"
	waited ninja -C "$wt/uros/build-x86_64" > "$RUNDIR/$arm-build.log" 2>&1 ||
		{ stop_before_boots "building the $arm arm"; die 3 "$arm: the tree does not build, see $RUNDIR/$arm-build.log"; }
	stop_before_boots "building the $arm arm"
	# Which kernel the arm built, by its code: the md5 of the whole file
	# changes from one build of the same tree to the next, the md5 of .text
	# does not, so the two arms' lines say whether their kernels differ.
	# #734: and they can say it.  The kernel's __FILE__ strings were absolute
	# paths, fixed/ and ablated/ differ in length, and every string after
	# them moved, so the two md5s differed whatever the patches changed; the
	# build maps the paths to src/... and build/... now.  The same md5 on both
	# lines says the patches left the kernel's code alone: a patch outside the
	# kernel, or a kernel patch under an #if the configuration leaves off.
	kernel="$wt/uros/build-x86_64/export/uros/boot/mach_kernel"
	objcopy -O binary --only-section=.text "$kernel" "$RUNDIR/$arm.text" 2>/dev/null ||
		die 3 "$arm: no kernel text to read at $kernel"
	echo "ablate: $arm tree built ($SHORT + ${#apply[@]} patches), kernel .text md5 $(md5sum < "$RUNDIR/$arm.text" | cut -c1-12)"
	rm -f "$RUNDIR/$arm.text"
done

CSV="$DIR/ablate.csv"
[ -s "$CSV" ] ||
	echo "date,time,host,base,patches,arm,boot,accel,run,clock_mhz,power,harness_status,verdict,log" > "$CSV"
names=$(for p in "${EXPANDED[@]}"; do basename "$p" .patch; done | paste -sd+)
# uname and not hostname: the Arch Linux container the campaign runs in has
# no hostname, which is in inetutils, and this line under set -e ended its
# first A/B with both arms built and no boot (#656).
host=$(uname -n)

caught=0 missed=0 passed=0 failed=0
# #1010: fixed boots without their premise, and the ablated boots beside them
# that were not caught: not asked, rather than uncaught.
unpremised=0 unaskable=0
# #951: the boots made so far, of all the run plans, for the line a stopped run
# ends with.
MADE=0
TOTAL=$(( BOOTS * ${#ACCELS[@]} * ${#ARMS[@]} ))
STOPPED_AT=""
for i in $(seq 1 "$BOOTS"); do
	for acc in "${ACCELS[@]}"; do
		# #1010: what the fixed boot of this pair said of the premise.
		pair_premise=""
		for arm in "${ARMS[@]}"; do
			# #951: asked to stop between two boots, it starts no other.
			if [ -n "$STOP" ]; then
				STOPPED_AT="before boot $((MADE + 1)) of $TOTAL"
				break 3
			fi
			MADE=$((MADE + 1))
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
			# #951: and each is a job waited for, so that a SIGTERM's
			# trap runs during it and passes the signal on; its
			# standard input named, which an asynchronous command
			# would otherwise have from /dev/null.
			set +e
			started=$SECONDS
			if [ ${#SCREEN[@]} -gt 0 ]; then
				# #670: its own build again, read off its screen; the
				# script's exit 2 is a run it could not make, as the
				# harness's is.
				log="$RUNDIR/$arm-$acc-$i.out"
				mkdir -p "$RUNDIR/$arm-$acc-$i"
				env --default-signal=INT,QUIT python3 "$RUNDIR/$arm/scripts/fbcons-readback.py" "${kvm[@]}" \
					--build "$RUNDIR/$arm/uros/build-x86_64" \
					--out "$RUNDIR/$arm-$acc-$i" "${SCREEN[@]}" \
					> "$log" 2>&1 <&0 &
			elif [ ${#SCRIPT[@]} -gt 0 ]; then
				# #618: its own tree's script, with its own build;
				# its exit 2 is a run it could not make.  A boot it
				# makes through run-x86_64.sh with no log of its own
				# writes the log beside this output, not over the
				# default one, which every arm would share (#591).
				log="$RUNDIR/$arm-$acc-$i.out"
				UROS_BUILD_DIR="$RUNDIR/$arm/uros/build-x86_64" \
				UROS_X86_64_LOG="$RUNDIR/$arm-$acc-$i.log" \
					env --default-signal=INT,QUIT "$RUNDIR/$arm/${SCRIPT[0]}" "${kvm[@]}" "${SCRIPT[@]:1}" \
					> "$log" 2>&1 <&0 &
			else
				UROS_BUILD_DIR="$RUNDIR/$arm/uros/build-x86_64" UROS_X86_64_LOG=$log \
					env --default-signal=INT,QUIT "$RUNDIR/$arm/scripts/run-x86_64.sh" "${kvm[@]}" "${RUN[@]}" \
					> "$RUNDIR/$arm-$acc-$i.out" 2>&1 <&0 &
			fi
			CHILD=$!
			[ -z "$STOP" ] || kill -TERM "$CHILD" 2>/dev/null
			wait "$CHILD"
			rc=$?
			if [ -n "$STOP" ] && [ $rc -gt 128 ]; then
				wait "$CHILD"
				rc=$?
			fi
			CHILD=""
			set -e
			# Stopped by the signal, the boot is said to be, and judged no
			# further; one that ended by itself first is judged as any
			# other, and the run stops before the next.
			if [ -n "$STOP" ] && [ $rc -gt 128 ]; then
				verdict=stopped
			else
				# Above 128 the boot's runner was killed by a signal: a
				# run not made, like 2 and 3, and not a verdict on the
				# tree (#661's review, seven rounds).
				case $rc in
				2|3|129|1[3-9][0-9]|2[0-5][0-9])
					die 3 "$arm, $acc, boot $i: the harness could not run it (status $rc), see $RUNDIR/$arm-$acc-$i.out" ;;
				esac
				hit=0
				# In UTF-8 wherever this runs: over ssh a machine can have
				# no LANG at all, and in the C locale the . that a patch
				# writes for an em dash is one byte of three.
				LC_ALL=C.UTF-8 grep -Eqf "$RUNDIR/expect" "$log" && hit=1
				lacks=""
				if [ "$arm" = ablated ]; then
					if [ $hit = 1 ]; then
						verdict=CAUGHT
						caught=$((caught + 1))
					elif [ "$pair_premise" = missing ]; then
						verdict="NOT ASKABLE"
						unaskable=$((unaskable + 1))
					else
						verdict="NOT CAUGHT"
						missed=$((missed + 1))
					fi
				elif [ $rc = 0 ] && [ $hit = 0 ]; then
					lacks=$(premise_missing "$log")
					if [ -n "$lacks" ]; then
						verdict="NO PREMISE"
						unpremised=$((unpremised + 1))
						pair_premise=missing
					else
						verdict=PASSED
						passed=$((passed + 1))
						pair_premise=shown
					fi
				else
					verdict=FAILED
					failed=$((failed + 1))
				fi
			fi
			clock=$(sed -n 's/.*clock in run: median \([0-9]*\)MHz.*/\1/p' "$log" | head -n 1)
			power=$(sed -n 's/.*host start:.* power=\([^ ]*\).*/\1/p' "$log" | head -n 1)
			runcol="${RUN[*]}"
			[ ${#SCREEN[@]} -gt 0 ] && runcol="fbcons-readback.py ${SCREEN[*]}"
			[ ${#SCRIPT[@]} -gt 0 ] && runcol="${SCRIPT[*]}"
			# No comma inside a field: scripts/ci/job.sh and
			# pull-results.sh split a row on every comma, quotes or
			# not, and "-smp 1,sockets=2,..." or "-cpu max,hypervisor=off"
			# made a row of more columns, refused (#659).  The merge
			# test writes its topology the same way, with ';'.
			runcol=${runcol//,/;}
			echo "$(date +%F,%T),$host,$SHORT,$names,$arm,$i,$acc,\"$runcol\",$clock,$power,$rc,$verdict,$log" >> "$CSV"
			if [ "$verdict" = stopped ]; then
				echo "ablate: $arm, $acc, boot $i: stopped (SIG$STOP $((SECONDS - started)) s into the boot, harness status $rc)"
				STOPPED_AT="at boot $MADE of $TOTAL, $arm, $acc, boot $i"
				break 3
			fi
			echo "ablate: $arm, $acc, boot $i: $verdict (harness status $rc, ${clock:-?} MHz, power ${power:-?})"
			[ -z "$lacks" ] || echo "ablate:   no line of it matches the premise: $lacks"
		done
	done
done

# #951: a stopped run says where it stopped, and then what the boots it made
# said, as a whole run does.
[ -z "$STOP" ] || echo "ablate: stopped by SIG$STOP ${STOPPED_AT:-after its last boot, $MADE of $TOTAL}"
echo "ablate: ablated tree caught $caught of $((caught + missed))"
[ $unaskable = 0 ] || echo "ablate: $unaskable ablated boot(s) not askable, not caught beside a fixed boot without its premise (#1010)"
[ $AB = 1 ] && echo "ablate: fixed tree passed $passed of $((passed + failed + unpremised))"
[ $unpremised = 0 ] || echo "ablate: the premise was missing in $unpremised of $((passed + failed + unpremised)) fixed boot(s): the ablation could not be asked there (#1010)"
# And it ends by the signal, its worktrees removed first, with the signals
# that could cut the removal short ignored, as stopped_by() does.
if [ -n "$STOP" ]; then
	trap - EXIT
	trap '' INT TERM HUP
	cleanup
	trap - INT TERM HUP
	kill -s "$STOP" "$$"
fi
[ $missed = 0 ] && [ $failed = 0 ] || exit 1
[ $unpremised = 0 ] || exit 4
exit 0
