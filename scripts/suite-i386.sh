#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# suite-i386.sh -- the i386 boots a change to shared code must pass, and their
# verdicts.
#
# i386 is the mature target, so for the code both targets share it is the only
# net.  A regression round boots the full bundle eight times: six times under
# KVM at four processors, once under KVM at one, once under TCG at four.  The
# six repeat because the failures this net exists for are intermittent: #544's
# struck a quarter of the runs.  Until this script the set lived outside the
# repository, in a campaign script each round copied, which judged a boot only
# by whether pthread_test, cap_test and hal_bar_test ended and ipc_bench
# completed.  It is this script's default, and each boot is judged as
# run-x86_64.sh judges x86-64's:
#
#   bundle-kvm-4   six times
#   bundle-kvm-1
#   bundle-tcg-4
#
# Other runs are asked for by name with -r, in order, a name as many times as
# wanted: bundle-tcg-1; and smoke-kvm-1 and smoke-tcg-1, the ush smoke, which
# boots the minimal bundle, where no self-test runs, and asks only that the
# shell comes up, runs a program in the background and powers off.
#
# Usage: suite-i386.sh [-o DIR] [-r "RUN..."]
#        suite-i386.sh --judge LOG     judge a bundle boot's log already written
#   -o DIR    where the logs go (default ~/uros-tests/suite-i386/<time>-<tree>)
#   -r RUNS   these runs instead of the eight
#
# Exit status: 0 every run passed · 1 at least one did not · 2 refused to start
# (another qemu is running, or no such run) · 3 a run could not be started at
# all.
#
# ── How a bundle run is judged ─────────────────────────────────────────
#
# As run-x86_64.sh judges x86-64's (#425): the bundle starts its tasks in order
# and they run at the same time, so no single line can mark the end.  Every
# test that announced itself must also have concluded; pthread_test, cap_test
# and ipc_bench must have spoken, so that a boot that stopped before the tests
# is not passed for having started none; and no line may say WRONG, FAIL or
# panic.  A run that stops printing for QUIET seconds, or goes on past CAP, is
# stopped and said to be so.
set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build}
TREE=$(git -C "$REPO" rev-parse --short HEAD)
OUT=$HOME/uros-tests/suite-i386/$(date +%Y%m%d-%H%M%S)-$TREE
RUNS=(bundle-kvm-4 bundle-kvm-4 bundle-kvm-4 bundle-kvm-4 bundle-kvm-4 bundle-kvm-4
	bundle-kvm-1 bundle-tcg-4)
JUDGE=""
usage() {
	echo "usage: $0 [-o DIR] [-r \"RUN...\"]   or   $0 --judge LOG" >&2
	exit 2
}
while [ $# -gt 0 ]; do
	case $1 in
	--judge)	[ -r "${2:-}" ] || usage; JUDGE=$2; shift 2 ;;
	-o)		[ -n "${2:-}" ] || usage; OUT=$2; shift 2 ;;
	-r)		[ -n "${2:-}" ] || usage; RUNS=($2); shift 2 ;;
	*)		usage ;;
	esac
done
for run in "${RUNS[@]}"; do
	if ! [[ $run =~ ^(smoke|bundle)-(kvm|tcg)-[0-9]+$ ]]; then
		echo "suite-i386: no run called $run" >&2
		exit 2
	fi
done
CSV=$HOME/uros-tests/suite-i386.csv


# The started/finished pairs.  The first block is run-x86_64.sh's list for the
# tests the two targets share; a test that never starts on this target is
# never waited for.  The second is i386's own.
PAIRS=(
	'pthread_test: starting'	'pthread_test: \(ALL [0-9]* TESTS PASSED\|SOME TESTS FAILED\)'
	'msg_server_test: started'	'msg_server_test: [0-9]* of [0-9]* arms passed'
	'cap_test: starting'		'cap_test: \(ALL TESTS PASSED\|SOME TESTS FAILED\)'
	'dma_reclaim: started'		'dma_reclaim: [0-9]* of [0-9]* arms passed'
	'hal_bar: started'		'hal_bar: [0-9]* of [0-9]* arms passed'
	'io_claim_race: started'	'io_claim_race: [0-9]* of [0-9]* arms passed'
	'=== ext2 filesystem server'	"ext2: ready, entering message loop\\|ext_server' task terminated"
	'xfile_read: started'		'xfile_read: \([0-9]* of [0-9]* arms passed\|NOT ASKED\)'
	'xfile_write: started'		'xfile_write: \([0-9]* of [0-9]* arms passed\|NOT ASKED\)'
	'=== kernel242_test ('		'=== kernel242_test: [0-9]* PASS, [0-9]* FAIL ==='
	'=== sig_test ('			'=== sig_test: [0-9]* PASS, [0-9]* FAIL ==='
	'fperr_test: x87 arm starting'	'fperr_test: x87 arm \(PASS\|SKIPPED\|FAIL\)'
	'fperr_test: SIMD arm starting'	'fperr_test: SIMD arm \(PASS\|SKIPPED\|FAIL\)'
	'irq_claim_test: '		'irq_claim_test: [0-9]* of 6 line arms passed'
)
# What every full boot must have said: the tests that are always in the bundle.
MUST=(
	'pthread_test: \(ALL [0-9]* TESTS PASSED\|SOME TESTS FAILED\)'
	'cap_test: \(ALL TESTS PASSED\|SOME TESTS FAILED\)'
	'^=== Benchmark complete ==='
)
# A line that fails the run wherever it appears -- among them a task killed by
# an exception nobody catches, which a normal boot never prints (#617's i386
# face ends ipc_bench that way).  A panic is matched by its own form, panic(cpu
# N) or a <panic+0x...> frame for a first line that arrived in pieces (#578):
# the word alone is in a test's "freed 256 ports without panic".  gpu_server
# echoes the console
# into its own lines, which would count each failure twice (as the #599
# campaigns found), so its echo is left out of every search.
FAIL_RE='WRONG|SOME TESTS FAILED|[1-9][0-9]* FAIL ===| arm FAIL$|panic\(cpu|<panic\+0x|Assertion failed|double fault|exception_no_server: terminating task'

clean() { grep -av 'gpu_server: text_puts' "$1"; }

# Each started test with no closing line, one per line; nothing when all closed.
open_reports() {
	local log=$1 i
	for ((i = 0; i < ${#PAIRS[@]}; i += 2)); do
		if clean "$log" | grep -aq "${PAIRS[i]}" && ! clean "$log" | grep -aq "${PAIRS[i+1]}"; then
			echo "${PAIRS[i]%% (}"
		fi
	done
}

over() {
	local log=$1 m
	for m in "${MUST[@]}"; do
		clean "$log" | grep -aq "$m" || return 1
	done
	[ -z "$(open_reports "$log")" ]
}

judge_bundle() {	# judge_bundle LOG CUT -> prints the reasons; status 0 = passed
	local log=$1 cut=$2 bad=0 m
	if [ -n "$cut" ]; then echo "    $cut"; bad=1; fi
	clean "$log" | grep -aE "$FAIL_RE" | head -8 | sed 's/^/    unexplained: /' | grep . && bad=1
	for m in "${MUST[@]}"; do
		clean "$log" | grep -aq "$m" || { echo "    never said: $m"; bad=1; }
	done
	open_reports "$log" | sed 's/^/    started and never reported: /' | grep . && bad=1
	return $bad
}

if [ "$JUDGE" ]; then
	judge_bundle "$JUDGE" ""
	if [ $? = 0 ]; then echo "suite-i386: $JUDGE: PASSED"; exit 0; fi
	echo "suite-i386: $JUDGE: FAILED"; exit 1
fi

if pidof qemu-system-i386 qemu-system-x86_64 > /dev/null || [ -d /tmp/uros-x86_64-run.lock ]; then
	echo "suite-i386: another qemu, or an x86-64 run, is in flight -- not starting" >&2
	exit 2
fi
mkdir -p "$OUT"

# The build before any clock (#592): run-qemu.sh builds before each boot, and
# the first boot would otherwise spend its budget compiling.
echo "suite-i386: tree $TREE, build $BUILD, logs in $OUT"
if ! (cd "$REPO" && UROS_BUILD_DIR=$BUILD ./scripts/run-qemu.sh --build-only > "$OUT/build.log" 2>&1); then
	echo "suite-i386: the build failed, see $OUT/build.log" >&2
	exit 3
fi

power=battery
for s in /sys/class/power_supply/*; do
	[ "$(cat "$s/type" 2>/dev/null)" = Mains ] || continue
	[ "$(cat "$s/online" 2>/dev/null)" = 1 ] && power=AC
done
cap=$(sort -n /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq 2>/dev/null | tail -1)
[ -s "$CSV" ] || echo "date,time,host,tree,run,cap_khz,power,verdict,seconds,log" > "$CSV"

passed=0 failed=0 n=0
for run in "${RUNS[@]}"; do
	n=$((n + 1))
	IFS=- read -r kind accel smp <<< "$run"
	tcg=""
	[ "$accel" = tcg ] && tcg=--tcg
	log=$OUT/$n-$run.log
	t0=$(date +%s)
	case $kind in
	smoke)
		(cd "$REPO" && UROS_BUILD_DIR=$BUILD ./scripts/smoke-ush.sh $tcg --smp "$smp" --log "$log") \
			> "$OUT/$n-$run.out" 2>&1
		if [ $? = 0 ]; then verdict=PASSED; else verdict=FAILED; fi
		why=$(grep -aE 'FAIL|timed out|TIMEOUT' "$OUT/$n-$run.out" | head -3 | sed 's/^/    /')
		;;
	bundle)
		quiet=180 capsec=1200
		[ "$accel" = tcg ] && { quiet=300; capsec=2400; }
		(cd "$REPO" && UROS_BUILD_DIR=$BUILD exec ./scripts/run-qemu.sh --fresh-disk $tcg \
			--smp "$smp" -display none -serial mon:stdio > "$log" 2>&1 < /dev/null) &
		pid=$!
		cut="" size=-1 still=0
		while kill -0 $pid 2>/dev/null; do
			sleep 2
			if over "$log"; then
				break
			fi
			if clean "$log" | grep -aqE 'panic\(cpu|<panic\+0x'; then
				sleep 5		# the backtraces come after the first line
				break
			fi
			now=$(stat -c %s "$log" 2>/dev/null || echo 0)
			if [ "$now" = "$size" ]; then still=$((still + 2)); else still=0; size=$now; fi
			if [ $still -ge $quiet ]; then cut="NOTHING ARRIVED for ${quiet}s: the machine stopped"; break; fi
			if [ $(( $(date +%s) - t0 )) -ge $capsec ]; then cut="still printing after ${capsec}s: cut"; break; fi
		done
		if ! kill -0 $pid 2>/dev/null && ! over "$log" && [ -z "$cut" ]; then
			cut="qemu ended before every test reported"
		fi
		kill $pid 2>/dev/null
		wait $pid 2>/dev/null
		if [ "$(wc -c < "$log")" -lt 1000 ]; then
			echo "suite-i386: $run produced almost nothing -- see $log" >&2
			exit 3
		fi
		why=$(judge_bundle "$log" "$cut")
		if [ $? = 0 ]; then verdict=PASSED; else verdict=FAILED; fi
		;;
	esac
	secs=$(( $(date +%s) - t0 ))
	echo "$(date +%F,%T),$(hostname),$TREE,$run,$cap,$power,$verdict,$secs,$log" >> "$CSV"
	echo "suite-i386: $n of ${#RUNS[@]}, $run: $verdict in ${secs}s"
	[ -n "$why" ] && echo "$why"
	if [ "$verdict" = PASSED ]; then passed=$((passed + 1)); else failed=$((failed + 1)); fi
done

echo "suite-i386: $passed of $((passed + failed)) runs passed (tree $TREE, cap ${cap:-?} kHz, $power)"
[ $failed = 0 ]
