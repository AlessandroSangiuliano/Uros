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
# Usage: suite-i386.sh [--battery] [-o DIR] [-r "RUN..."]
#        suite-i386.sh --judge LOG     judge a bundle boot's log already written
#   -o DIR      where the logs go (default ~/uros-tests/suite-i386/<time>-<tree>)
#   -r RUNS     these runs instead of the eight
#   --battery   start on battery.  Without it a laptop off its charger refuses:
#               the supply moves the clock, and with it which races open, so
#               boots on battery are not comparable with boots on mains.  A
#               machine that lists no mains supply is a desktop, on mains.
#
# One CSV row per boot goes to ~/uros-tests/suite-i386.csv.  Besides the
# verdict it holds the host's clock during the boot (the lowest, median and
# highest of a sample every two seconds), the supply, how many times the host
# suspended, the kernel's md5, and ipc_bench's x4 RPC and x16 fault-stress
# times, so that boots of different days and machines can be put side by side.
#
# Exit status: 0 every run passed · 1 at least one did not · 2 refused to start
# (another qemu is running, on battery, or no such run) · 3 a run could not be
# started at all.
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
#
# QUIET must outlast the longest stretch in which a healthy boot prints
# nothing, and that is ipc_bench's fault-stress x32, which reports only at its
# end: on a 1.4 GHz host, up to 75 s under KVM and 304 s under TCG at four
# processors.  So QUIET is 300 s under KVM and 900 s under TCG, and CAP, at
# 1200 s and 3600 s, is over twice the longest healthy boot seen: 250 s and
# 1645 s.
set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build}
TREE=$(git -C "$REPO" rev-parse --short HEAD)
OUT=$HOME/uros-tests/suite-i386/$(date +%Y%m%d-%H%M%S)-$TREE
RUNS=(bundle-kvm-4 bundle-kvm-4 bundle-kvm-4 bundle-kvm-4 bundle-kvm-4 bundle-kvm-4
	bundle-kvm-1 bundle-tcg-4)
JUDGE="" BATTERY=0
usage() {
	echo "usage: $0 [--battery] [-o DIR] [-r \"RUN...\"]   or   $0 --judge LOG" >&2
	exit 2
}
while [ $# -gt 0 ]; do
	case $1 in
	--judge)	[ -r "${2:-}" ] || usage; JUDGE=$2; shift 2 ;;
	--battery)	BATTERY=1; shift ;;
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

mhz() {	# the host's clock now, the mean over its processors, in MHz
	awk -F: '/^cpu MHz/ {s += $2; n++} END {if (n) printf "%d\n", s / n}' /proc/cpuinfo
}

supply() {	# AC or battery; a machine that lists no mains supply is a desktop, on mains
	local s p=AC
	for s in /sys/class/power_supply/*; do
		[ "$(cat "$s/type" 2>/dev/null)" = Mains ] || continue
		p=battery
		if [ "$(cat "$s/online" 2>/dev/null)" = 1 ]; then p=AC; break; fi
	done
	echo $p
}

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
if [ "$(supply)" = battery ] && [ $BATTERY = 0 ]; then
	echo "suite-i386: on battery -- not starting (--battery to start anyway)" >&2
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

cap=$(sort -n /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq 2>/dev/null | tail -1)
[ -s "$CSV" ] || echo "date,time,host,tree,run,verdict,seconds,cap_khz,mhz_min,mhz_med,mhz_max,power,suspends,kernel_md5,x4_ns,x16_ns,log" > "$CSV"

passed=0 n=0
for run in "${RUNS[@]}"; do
	n=$((n + 1))
	power=$(supply)
	if [ $power = battery ] && [ $BATTERY = 0 ]; then
		echo "suite-i386: the host went on battery -- stopping before run $n of ${#RUNS[@]}" >&2
		break
	fi
	IFS=- read -r kind accel smp <<< "$run"
	tcg=""
	[ "$accel" = tcg ] && tcg=--tcg
	log=$OUT/$n-$run.log
	m=()
	t0=$(date +%s)
	case $kind in
	smoke)
		(cd "$REPO" && UROS_BUILD_DIR=$BUILD exec ./scripts/smoke-ush.sh $tcg --smp "$smp" --log "$log") \
			> "$OUT/$n-$run.out" 2>&1 < /dev/null &
		pid=$!
		while kill -0 $pid 2>/dev/null; do
			sleep 2
			m+=($(mhz))
		done
		wait $pid
		if [ $? = 0 ]; then verdict=PASSED; else verdict=FAILED; fi
		why=$(grep -aE 'FAIL|timed out|TIMEOUT' "$OUT/$n-$run.out" | head -3 | sed 's/^/    /')
		;;
	bundle)
		quiet=300 capsec=1200
		[ "$accel" = tcg ] && { quiet=900; capsec=3600; }
		(cd "$REPO" && UROS_BUILD_DIR=$BUILD exec ./scripts/run-qemu.sh --fresh-disk $tcg \
			--smp "$smp" -display none -serial mon:stdio > "$log" 2>&1 < /dev/null) &
		pid=$!
		cut="" size=-1 still=0
		while kill -0 $pid 2>/dev/null; do
			sleep 2
			m+=($(mhz))
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
	t1=$(date +%s)
	secs=$((t1 - t0))
	p1=$(supply)
	[ $p1 = $power ] || power=$power-then-$p1
	read -r mlo mmed mhi <<< "$(printf '%s\n' "${m[@]}" | sort -n |
		awk 'NF {a[++k] = $1} END {if (k) print a[1], a[int((k + 1) / 2)], a[k]}')"
	# A host that slept stopped the guest with it, and the boot looks wedged.
	if s=$(journalctl -k -q --since "@$t0" --until "@$t1" 2>/dev/null); then
		suspends=$(grep -c 'PM: suspend entry' <<< "$s")
	else
		suspends='?'
	fi
	if [ "$suspends" != 0 ] && [ "$suspends" != '?' ]; then
		why="${why:+$why
}    the host suspended $suspends time(s) during this boot: a stop may be the host's"
	fi
	md5=$(md5sum < "$BUILD/export/uros/boot/mach_kernel" 2>/dev/null | cut -c1-12)
	x4=$(clean "$log" 2>/dev/null | grep -ao 'concurrent same-space x4 *[0-9]* RPCs in [0-9]* ns' |
		head -1 | grep -o '[0-9]* ns$' | grep -o '[0-9]*')
	x16=$(clean "$log" 2>/dev/null | grep -ao 'fault-stress x16 .* in [0-9]* ns' |
		head -1 | grep -o '[0-9]* ns$' | grep -o '[0-9]*')
	echo "$(date +%F,%T),$(hostname),$TREE,$run,$verdict,$secs,$cap,$mlo,$mmed,$mhi,$power,$suspends,$md5,$x4,$x16,$log" >> "$CSV"
	echo "suite-i386: $n of ${#RUNS[@]}, $run: $verdict in ${secs}s"
	[ -n "$why" ] && echo "$why"
	[ "$verdict" = PASSED ] && passed=$((passed + 1))
done

echo "suite-i386: $passed of ${#RUNS[@]} runs passed (tree $TREE, cap ${cap:-?} kHz)"
[ $passed = ${#RUNS[@]} ]
