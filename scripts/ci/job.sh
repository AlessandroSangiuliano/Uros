#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ci/job.sh -- one job of the campaign on the epic (#653): the toolchain, the
# x86-64 build from the preset with no warning, then the merge test round after
# round, one CSV row per boot.  Or, with --ablate, an A/B of fixes taken out in
# place of the merge test (#656).
#
# Why.  The short A/B and the merge test run on a batch of branches save hours
# of machine time, and they also find fewer rare races by chance: #651, #519
# and #644 came out of rounds that were looking for something else.  So the
# boots saved are spent here, on GitHub's runners, inside an Arch Linux
# container, because the tree is built with GCC 16 and booted with Arch's qemu
# and the runners' Ubuntu has neither.  Every verdict is a line printed by
# merge-test-x86_64.sh and the entries are its own list, read out of it: a
# second copy of either would be a second half free to disagree with the first.
#
# A defect some runners show and the home machines do not -- #603's, on the AMD
# EPYC 9V45 under KVM -- can only be taken out and put back where it shows, and
# that is what --ablate is for: scripts/ablate.sh --ab on this tree, with the
# patches of ablations/ named, the fixed tree and the tree with the fixes taken
# out booted one after the other, the same build checks before them.
#
# Usage:
#   scripts/ci/job.sh [--entries "N ..."] [--rounds R] [--accel L] [--out DIR]
#                     [--no-install] [--dry-run]
#   scripts/ci/job.sh --ablate "P ..." [--boots N] [--accel L] [--out DIR]
#                     [--no-install] [--dry-run]
#
#   --entries "N ..."  only these merge-test entries (default: its whole list)
#   --rounds R         merge-test rounds, one after the other (default 1)
#   --ablate "P ..."   an A/B instead of the merge test, with these patches of
#                      ablations/, named without the directory or ".patch"
#   --boots N          with --ablate, boots per arm and accelerator (default 1)
#   --accel L          tcg, kvm or tcg,kvm (default: tcg, and kvm as well when
#                      /dev/kvm can be opened)
#   --out DIR          rows, logs and the job's record (default ./ci-out)
#   --no-install       use the tools already here, outside a container
#   --dry-run          print the plan; install, build and boot nothing
#
# The tree is the one this script is in.  GITHUB_RUN_ID and JOB_INDEX name the
# run and the job in every row, and UROS_BUILD_DIR is honoured as by the
# harness.  OUT/boots.csv gets one row per boot, its times in UTC; the logs of
# every WRONG boot are copied to OUT/failed/rN/; OUT/job.txt says what the job
# ran on and with which tools.  With --ablate, OUT/ablate.csv gets the rows
# instead, one per boot of either arm, and every boot's log stays in
# OUT/ablate/, where ablate.sh works.
#
# Exit status: 0 every boot as it should be -- with --ablate, every ablated boot
# caught and every fixed boot passed · 1 at least one boot WRONG, not caught or
# failed · 2 the command line is wrong, or a patch does not apply · 3 the job
# could not boot, or could not read its boots: the packages, the configure or
# the build failed, the build had a warning, a round or the A/B did not report
# every boot it was meant to, or a boot's log was not where the merge test puts
# it.
set -u
# The container has no locale but C, and a run outside it reads the same words:
# under another locale the compilers translate the very `warning:' the build's
# count greps for.
export LC_ALL=C

REPO=$(cd "$(dirname "$0")/../.." && pwd)
MT=$REPO/scripts/merge-test-x86_64.sh
PRESET=$REPO/uros/cmake/presets/x86_64.cmake
HEADER=date,time,run_id,job,round,tree,cpu_model,mhz,accel,entry,smp,opt,verdict,log
AB_HEADER=date,time,run_id,job,tree,cpu_model,mhz,patches,arm,boot,accel,run,harness_status,verdict,log
RUN_ID=${GITHUB_RUN_ID:-local}
JOB=${JOB_INDEX:-0}

# What the build and the boots call that the image may not have.  gcc,
# binutils, cmake and ninja build; bison and flex make migcom; python runs the
# build's own checks and xfile-verify.py; git names the tree; grub, e2fsprogs
# and util-linux are make-disk-x86_64.sh's grub-mkimage, mke2fs, debugfs and
# sfdisk; iproute2 is merge-test's look at port 1234, psmisc run-x86_64.sh's
# killall, gdb walks the threads of a run cut short with the stub on, and
# lib32-glibc and lib32-gcc-libs let the host tests build for -m32 as well.
PACKAGES=(gcc binutils cmake ninja bison flex python git qemu-system-x86
	grub e2fsprogs util-linux iproute2 psmisc gdb lib32-glibc lib32-gcc-libs)

die() {
	echo "job: $2" >&2
	exit "$1"
}

ENTRIES=""
ROUNDS=1
ROUNDS_SET=0
ABLATE=""
BOOTS=1
BOOTS_SET=0
ACCEL=auto
OUT=ci-out
INSTALL=1
DRY=0
while [ $# -gt 0 ]; do
	case "$1" in
	--entries|--rounds|--accel|--out|--ablate|--boots)
		[ $# -ge 2 ] || die 2 "$1 needs a value"
		case "$1" in
		--entries)	ENTRIES=$2 ;;
		--rounds)	ROUNDS=$2; ROUNDS_SET=1 ;;
		--ablate)	ABLATE=$2 ;;
		--boots)	BOOTS=$2; BOOTS_SET=1 ;;
		--accel)	ACCEL=$2 ;;
		--out)		OUT=$2 ;;
		esac
		shift 2 ;;
	--no-install)	INSTALL=0; shift ;;
	--dry-run)	DRY=1; shift ;;
	-h|--help)	sed -n '/^# Usage:/,/^# it\.$/p' "$0"; exit 0 ;;
	*)		die 2 "unknown argument '$1' (--help says what it takes)" ;;
	esac
done

case "$ROUNDS" in
''|*[!0-9]*|0)	die 2 "--rounds takes a number of rounds, not '$ROUNDS'" ;;
esac
read -r -a WANT <<< "$(tr ',\n\t' '   ' <<< "$ENTRIES")"
for e in "${WANT[@]}"; do
	case "$e" in
	*[!0-9]*)	die 2 "--entries takes entry numbers, not '$e'" ;;
	esac
done
ENTRIES="${WANT[*]}"
# The patches of an A/B, by name, out of this tree's ablations/ and nowhere
# else: the name comes from a workflow input, and a path would reach outside.
PATCHES=()
if [ -n "$ABLATE" ]; then
	[ $ROUNDS_SET = 0 ] && [ -z "$ENTRIES" ] ||
		die 2 "--entries and --rounds choose the merge test's boots, and --ablate boots what its patches' Run: line says: give one or the other"
	case "$BOOTS" in
	''|*[!0-9]*|0)	die 2 "--boots takes a number of boots, not '$BOOTS'" ;;
	esac
	read -r -a NAMES <<< "$(tr ',\n\t' '   ' <<< "$ABLATE")"
	for n in "${NAMES[@]}"; do
		case "$n" in
		.*|*[!A-Za-z0-9._-]*)	die 2 "--ablate takes the names of patches in ablations/, not '$n'" ;;
		esac
		n=${n%.patch}
		[ -f "$REPO/ablations/$n.patch" ] || die 2 "this tree has no ablations/$n.patch"
		PATCHES+=("$REPO/ablations/$n.patch")
	done
	[ ${#PATCHES[@]} -gt 0 ] || die 2 "--ablate names no patch"
elif [ $BOOTS_SET = 1 ]; then
	die 2 "--boots counts the boots of an A/B, and goes with --ablate"
fi
case "$OUT" in
/*)	;;
*)	OUT=$PWD/$OUT ;;
esac
OUT=${OUT%/}
CSV=$OUT/boots.csv
[ -z "$ABLATE" ] || CSV=$OUT/ablate.csv

# The merge test's own list, one boot per line and accelerator.  Empty means its
# ENTRIES block changed shape under this reader, which is a refusal and not a
# plan with nothing in it.  An A/B boots what its patches say instead.
if [ -z "$ABLATE" ]; then
	LIST=$(sed -n '/^ENTRIES="$/,/^"$/{/^[0-9]/p}' "$MT")
	[ -n "$LIST" ] || die 3 "read no entries out of $MT: its ENTRIES block has changed shape, and this script reads it"
	PLAN=$LIST
	if [ -n "$ENTRIES" ]; then
		for e in "${WANT[@]}"; do
			awk -v e="$e" '$1 == e { f = 1 } END { exit !f }' <<< "$LIST" ||
				die 2 "entry $e is not in the merge test's list; merge-test-x86_64.sh says why the entries it leaves out are not run"
		done
		PLAN=$(awk -v want=" $ENTRIES " 'index(want, " " $1 " ")' <<< "$LIST")
	fi
	NLINES=$(wc -l <<< "$PLAN")
fi

# Opened and not only looked at: as root, test -w says yes to a node the
# container's device list refuses, and a qemu that cannot open it turns every
# KVM boot into a refusal to start.
if [ -c /dev/kvm ] && ( exec 3<>/dev/kvm ) 2>/dev/null; then
	KVM="can be opened"
else
	KVM="cannot be opened"
fi
AUTO=0
if [ "$ACCEL" = auto ]; then
	AUTO=1
	ACCEL=tcg
	[ "$KVM" = "can be opened" ] && ACCEL=tcg,kvm
fi
read -r -a A <<< "${ACCEL//,/ }"
HAS_TCG=0
HAS_KVM=0
for a in "${A[@]}"; do
	case "$a" in
	tcg)	HAS_TCG=1 ;;
	kvm)	HAS_KVM=1 ;;
	*)	die 2 "--accel takes tcg and kvm, not '$a'" ;;
	esac
done
case $HAS_TCG$HAS_KVM in
10)	MT_ACC=tcg; ACCS=tcg ;;
01)	MT_ACC=kvm; ACCS=kvm ;;
11)	MT_ACC=both; ACCS="tcg kvm" ;;
*)	die 2 "--accel names no accelerator" ;;
esac
[ $HAS_KVM = 0 ] || [ "$KVM" = "can be opened" ] ||
	die 3 "kvm was asked for and /dev/kvm cannot be opened here"
[ -n "$ABLATE" ] || PER_ROUND=$((NLINES * (HAS_TCG + HAS_KVM)))

# The packages before anything reads the tree: git is one of them, and the
# archlinux image has none -- #653's first run stopped on that very line.  The
# keyring first: an image older than the key a package was signed with refuses
# the package, and the archlinux image is rebuilt only now and then.  A dry run
# installs nothing and writes nothing.
if [ $DRY = 0 ]; then
	[ -e "$CSV" ] && die 2 "$CSV already holds a job's rows: give another --out"
	mkdir -p "$OUT" || die 3 "cannot create $OUT"
	if [ $INSTALL = 1 ]; then
		[ "$(id -u)" = 0 ] || die 3 "installing needs root: run this in the container, or with --no-install"
		echo "job: installing the packages ($OUT/install.log)"
		# The x86-64 build also builds host tests for -m32 (pci_bar_test_m32,
		# both ABIs on purpose), so it needs 32-bit glibc and libgcc: they come
		# from [multilib], which the archlinux image leaves commented out.
		sed -i '/^#\[multilib\]$/{s/^#//;n;s/^#//}' /etc/pacman.conf
		if ! { pacman -Sy --noconfirm --needed archlinux-keyring &&
		       pacman -Su --noconfirm --needed "${PACKAGES[@]}"; } > "$OUT/install.log" 2>&1; then
			tail -n 30 "$OUT/install.log"
			die 3 "pacman failed: $OUT/install.log"
		fi
	fi
fi

# The checkout belongs to the runner's user and the container runs as root, and
# git will not read a repository somebody else owns unless it is named safe.
if ! git -C "$REPO" rev-parse -q --verify HEAD >/dev/null 2>&1 &&
   [ "$(stat -c %u "$REPO")" != "$(id -u)" ]; then
	git config --global --add safe.directory "$REPO"
fi
TREE=$(git -C "$REPO" rev-parse --short=8 HEAD 2>/dev/null) ||
	die 3 "git cannot read the checkout at $REPO"
git -C "$REPO" diff --quiet HEAD -- 2>/dev/null || TREE=$TREE-dirty
# Commas would split the column; a model name has none, but nothing promises.
CPU=$(sed -n 's/^model name[[:space:]]*: *//p' /proc/cpuinfo | head -n 1 | tr ',' ' ')
CPU=${CPU:-?}
NPROC=$(nproc)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
export UROS_BUILD_DIR=$BUILD

# What an A/B will build and boot, said by ablate.sh itself and refused here if
# it would refuse it, before anything is built: on twenty runners, a patch that
# no longer applies is twenty builds for nothing.  Its scratch index goes.
if [ -n "$ABLATE" ]; then
	scratch=$(mktemp -d) || die 3 "cannot create a scratch directory"
	AB_PLAN=$(UROS_ABLATE_DIR=$scratch "$REPO/scripts/ablate.sh" --plan --ab \
		--boots "$BOOTS" --accel "$ACCEL" "${PATCHES[@]}" 2>&1)
	rc=$?
	rm -rf "$scratch"
	if [ $rc != 0 ]; then
		printf '%s\n' "$AB_PLAN" >&2
		[ $rc = 2 ] && die 2 "ablate.sh refuses this A/B"
		die 3 "ablate.sh could not plan this A/B (status $rc)"
	fi
fi

echo "job: tree $TREE, run $RUN_ID, job $JOB; here: $CPU, $NPROC processors, /dev/kvm $KVM"
if [ $INSTALL = 1 ]; then
	echo "job: install: pacman -Sy archlinux-keyring, then pacman -Su --needed ${PACKAGES[*]}"
else
	echo "job: install: nothing (--no-install)"
fi
echo "job: configure: cmake -G Ninja -C $PRESET -S $REPO/uros -B $BUILD"
echo "job: build: ninja -C $BUILD, refused on any 'warning:' line but bison's parser.y conflicts"
# The plan of a job started elsewhere, as the workflow's is, was made on a
# machine whose /dev/kvm may not be that job's.
[ $AUTO = 0 ] || echo "job: accelerators: tcg, and kvm wherever /dev/kvm can be opened; here it $KVM"
if [ -n "$ABLATE" ]; then
	echo "job: an A/B instead of the merge test: scripts/ablate.sh --ab --boots $BOOTS --accel $ACCEL ${PATCHES[*]##*/}"
	sed 's/^/job:   /' <<< "$AB_PLAN"
	echo "job: rows: $CSV; every boot's log: $OUT/ablate/runs/"
else
	echo "job: $ROUNDS round(s) of $MT -a $MT_ACC -o $OUT/logs/rN${ENTRIES:+ -e \"$ENTRIES\"}"
	echo "job: $PER_ROUND boots a round ($NLINES of the merge test's lines x $ACCS), $((PER_ROUND * ROUNDS)) in all, each at the merge test's -smp unless it names one:"
	while read -r e v opt cpus; do
		[ "$opt" = - ] && opt=""
		echo "job:   entry $e${opt:+ (${opt//_/ })}${cpus:+ -smp $cpus}, judged by '$v'"
	done <<< "$PLAN"
	echo "job: rows: $CSV; logs of WRONG boots: $OUT/failed/"
fi
if [ $DRY = 1 ]; then
	echo "job: dry run: nothing installed, built or booted"
	exit 0
fi


# ----------------------------------------------------------------- toolchain
#

# What the rows were produced with, kept beside them: an image that moved under
# the campaign is otherwise a change nobody can date.
{
	echo "tree:     $TREE"
	echo "run:      $RUN_ID, job $JOB"
	echo "cpu:      $CPU, $NPROC processors"
	echo "kvm:      /dev/kvm $KVM; accelerators: $ACCS"
	echo "gcc:      $(gcc --version 2>&1 | head -n 1)"
	echo "binutils: $(ld --version 2>&1 | head -n 1)"
	echo "qemu:     $(qemu-system-x86_64 --version 2>&1 | head -n 1)"
	echo "cmake:    $(cmake --version 2>&1 | head -n 1)"
	echo "ninja:    $(ninja --version 2>&1 | head -n 1)"
	echo "grub:     $(grub-mkimage --version 2>&1 | head -n 1)"
} > "$OUT/job.txt"
sed 's/^/job: /' "$OUT/job.txt"

# --------------------------------------------------------------------- build

FRESH=1
[ -f "$BUILD/build.ninja" ] && FRESH=0
echo "job: configuring ($OUT/configure.log)"
if ! cmake -G Ninja -C "$PRESET" -S "$REPO/uros" -B "$BUILD" > "$OUT/configure.log" 2>&1; then
	tail -n 30 "$OUT/configure.log"
	die 3 "the configure failed: $OUT/configure.log"
fi
echo "job: building ($OUT/build.log)"
if ! ninja -C "$BUILD" > "$OUT/build.log" 2>&1; then
	tail -n 40 "$OUT/build.log"
	die 3 "the build failed: $OUT/build.log"
fi
WARN=$(grep 'warning:' "$OUT/build.log" | grep -v 'parser.y.*conflicts')
if [ -n "$WARN" ]; then
	printf '%s\n' "$WARN" | head -n 40
	die 3 "$(printf '%s\n' "$WARN" | wc -l) warning(s) in the build, which has none at home: $OUT/build.log"
fi
# A count of zero is worth something only if the grep could have matched.
# Every fresh build prints bison's conflicts on migcom's parser.y as a
# `warning:' line; a fresh log without it is a log the grep cannot read.
# An incremental build rebuilds only what changed and prints the warnings
# of nothing else, and that is said rather than counted as zero.
if [ $FRESH = 1 ]; then
	grep -q 'parser.y.*warning:.*conflicts' "$OUT/build.log" ||
		die 3 "a fresh build and no bison conflicts line in $OUT/build.log: the warning count cannot be trusted"
	echo "job: build: 0 warnings"
else
	echo "job: build: $BUILD was already configured, so this build was incremental and its 0 warnings cover only what it recompiled"
fi

# --------------------------------------------------------------------- boots

# The clock a boot read, from the conditions block its log ends with: the
# median run-x86_64.sh sampled during the run where it could sample one, and
# otherwise `N cpuinfo', the cpu MHz /proc/cpuinfo gave at the boot's start.
# A runner has no cpufreq, so there it is the second, and on a virtual machine
# that is the rate the hypervisor reports, which nobody measured; the word says
# so in the row instead of passing it off as the first.
clock_of() {	# full-log
	local m
	m=$(sed -n 's/^  clock in run: median \([0-9][0-9]*\)MHz.*/\1/p' "$1" 2>/dev/null | head -n 1)
	if [ -n "$m" ]; then
		echo "$m"
		return
	fi
	m=$(sed -n 's/^  host start: .*cpu=\([0-9][0-9]*\)MHz.*/\1/p' "$1" 2>/dev/null | head -n 1)
	if [ -n "$m" ]; then
		echo "$m cpuinfo"
	else
		echo "?"
	fi
}

# ------------------------------------------------------------------- the A/B
#
# ablate.sh builds both arms from this build's configuration -- UROS_BUILD_DIR,
# exported above, is its main build -- and boots each arm from its own build.
# Its rows become the job's, with the run, the job, the tree and the processor
# a runner's row needs, and are counted: an A/B that stopped short, or changed
# the shape of its rows, would otherwise pass as fewer boots with nothing wrong.
if [ -n "$ABLATE" ]; then
	echo "job: the A/B ($OUT/ablate.out)"
	UROS_ABLATE_DIR=$OUT/ablate "$REPO/scripts/ablate.sh" --ab --boots "$BOOTS" \
		--accel "$ACCEL" "${PATCHES[@]}" 2>&1 | tee "$OUT/ablate.out"
	rc=${PIPESTATUS[0]}
	echo "$AB_HEADER" > "$CSV"
	n=0
	if [ -f "$OUT/ablate/ablate.csv" ]; then
		while IFS=, read -r d t _host _base names arm boot a run _clock _power hs verdict log; do
			echo "$d,$t,$RUN_ID,$JOB,$TREE,$CPU,$(clock_of "$log"),$names,$arm,$boot,$a,$run,$hs,$verdict,${log#"$OUT"/}" >> "$CSV"
			n=$((n + 1))
		done < <(tail -n +2 "$OUT/ablate/ablate.csv")
	fi
	want=$((BOOTS * (HAS_TCG + HAS_KVM) * 2))
	{
		echo "ablate:   ${PATCHES[*]##*/}, $BOOTS boot(s) per arm and accelerator, $n of $want boots reported, ablate.sh status $rc"
		grep '^ablate: \(ablated tree caught\|fixed tree passed\)' "$OUT/ablate.out" | sed 's/^ablate: /result:   /'
	} >> "$OUT/job.txt"
	echo "job: the A/B reported $n of $want boots, ablate.sh status $rc -- tree $TREE, $CPU, $ACCS"
	case $rc in
	0|1)	[ $n = $want ] || die 3 "the A/B reported $n of the $want boots it was meant to: $OUT/ablate.out"
		exit $rc ;;
	2)	exit 2 ;;
	*)	exit 3 ;;
	esac
fi

WRONGS=()
UNFOUND=0
# One boot's row.  The verdict is the merge test's words, its kind of judgement
# included.  A WRONG boot's logs are copied where the artifact keeps them.
row() {	# round entry accel smp opt result kind path-a-WRONG-line-names
	local r=$1 e=$2 a=$3 n=$4 opt=$5 res=$6 kind=$7 named=$8
	local rdir=$OUT/logs/r$r tag h f log mhz x
	# The name merge-test-x86_64.sh gives a boot's logs.  A WRONG line names
	# its file, and that name wins over this one; a passing line names none,
	# and a log missing under this name is said in the row, never guessed,
	# and fails the job: the names have changed and the clock went unread.
	# The -smp value's commas are underscores there, as in the merge test.
	tag="e$e-$a${n//,/_}${opt:+-${opt##* }}"
	h=$rdir/$tag.log
	if [ -n "$named" ] && [ "$named" != "$h" ]; then
		echo "job: merge-test named $named where this script looked for $h: its log names have changed" >&2
		h=$named
	fi
	f=${h%.log}-full.log
	if [ -f "$f" ]; then
		mhz=$(clock_of "$f")
		log=${f#"$OUT"/}
	else
		mhz="?"
		log="missing: ${f#"$OUT"/}"
		UNFOUND=$((UNFOUND + 1))
	fi
	if [ "$res" = WRONG ]; then
		mkdir -p "$OUT/failed/r$r"
		for x in "$h" "$f" "$f.walk.txt"; do
			[ -f "$x" ] && cp -p "$x" "$OUT/failed/r$r/"
		done
		[ -f "$f" ] && log=failed/r$r/${f##*/}
		WRONGS+=("round $r, entry $e $a -smp $n${opt:+ ($opt)}: $log")
	fi
	echo "$(date -u '+%F,%T'),$RUN_ID,$JOB,$r,$TREE,$CPU,$mhz,$a,$e,${n//,/;},${opt//,/ },$res (${kind//,/ }),$log" >> "$CSV"
}

# The processor count is a whole -smp value where an entry gives one -- entry
# 14's machine with APIC ids 0 and 64 runs at -smp 1,sockets=2,cores=64,
# maxcpus=128 (#663) -- so it is a token, not a number: read as a number, those
# two boots were not counted, every round came out 86 of 88, and every job
# stopped after its first.
VERDICT_RE='^merge-test: entry ([0-9]+) (tcg|kvm) -smp ([0-9][^ ]*) (\(([^)]*)\) )?-- (as it should be|WRONG) \(([^)]*)\)(: (.*))?$'
END_RE='^merge-test: ([0-9]+) wrong$'

echo "$HEADER" > "$CSV"
BOOTS=0
WRONG=0
SHORT=0
DONE_ROUNDS=0
for r in $(seq 1 "$ROUNDS"); do
	args=(-a "$MT_ACC" -o "$OUT/logs/r$r")
	[ -n "$ENTRIES" ] && args+=(-e "$ENTRIES")
	got=0
	bad=0
	said=""
	# Each row is written as its line arrives, so a job stopped by its time
	# limit keeps every boot it finished.
	while IFS= read -r line; do
		printf '%s\n' "$line"
		if [[ $line =~ $VERDICT_RE ]]; then
			m=("${BASH_REMATCH[@]}")
			row "$r" "${m[1]}" "${m[2]}" "${m[3]}" "${m[5]}" "${m[6]}" "${m[7]}" "${m[9]}"
			got=$((got + 1))
			[ "${m[6]}" = WRONG ] && bad=$((bad + 1))
		elif [[ $line =~ $END_RE ]]; then
			said=${BASH_REMATCH[1]}
		fi
	done < <("$MT" "${args[@]}" 2>&1 | tee "$OUT/merge-r$r.out")
	BOOTS=$((BOOTS + got))
	WRONG=$((WRONG + bad))
	# The census: a round counts only if it reported every boot it planned
	# and its own total agrees with the lines read.  A merge test that refused
	# to start, died, or changed the shape of its lines would otherwise pass
	# as a short round with nothing wrong in it.
	if [ -z "$said" ] || [ "$got" -ne "$PER_ROUND" ] || [ "$said" -ne "$bad" ]; then
		echo "job: round $r reported $got of $PER_ROUND boots, $bad WRONG, and the merge test's own total was ${said:-never printed}: $OUT/merge-r$r.out" >&2
		SHORT=1
		break
	fi
	DONE_ROUNDS=$r
done

{
	echo "boots:    $BOOTS in $DONE_ROUNDS of $ROUNDS round(s), $WRONG WRONG"
	for w in "${WRONGS[@]}"; do
		echo "WRONG:    $w"
	done
	[ $SHORT = 0 ] || echo "short:    round $((DONE_ROUNDS + 1)) did not report every boot"
	[ $UNFOUND = 0 ] || echo "unfound:  $UNFOUND boot(s) left no log under the name merge-test gives it"
} >> "$OUT/job.txt"
echo "job: $BOOTS boots in $DONE_ROUNDS of $ROUNDS round(s), $WRONG WRONG -- tree $TREE, $CPU, $ACCS"
for w in "${WRONGS[@]}"; do
	echo "job:   WRONG: $w"
done
[ $UNFOUND = 0 ] || echo "job: $UNFOUND boot(s) left no log under the name merge-test gives it: see the rows' log column" >&2
[ $WRONG = 0 ] || exit 1
[ $SHORT = 0 ] && [ $UNFOUND = 0 ] || exit 3
exit 0
