#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# stop-unit-check.sh -- a boot stopped with its systemd unit, and whether
# run-x86_64.sh gave its lock back (#801).
#
# Long runs here are started with systemd-run, and stopped with systemctl: a
# script without a trap of its own runs run-x86_64.sh, so the unit's main
# process dies on the first SIGTERM, and systemd signals what is left of the
# unit a second time.  This makes that shape -- the run under a wrapper, with
# the build in UROS_BUILD_DIR and the log in UROS_X86_64_LOG as run-x86_64.sh
# takes them -- stops the unit once the guest is up, and then asks three
# things: the lock is gone, the run said it was stopped, and no qemu is left.
# On 31 October the lock stayed, and the next run refused to start until the
# directory was removed by hand.
#
#   Script: scripts/stop-unit-check.sh
#   Expect: stop-unit-check: WRONG
#
# Usage:  stop-unit-check.sh [--kvm] [SECONDS]
#
# Exit status: 0 the three hold · 1 WRONG · 2 not asked: another run or qemu
# was in flight, there is no user manager, or the guest never came up -- which
# says nothing about run-x86_64.sh.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
RUN=$HERE/scripts/run-x86_64.sh
LOCK=/tmp/uros-x86_64-run.lock
KVM=()
if [ "${1:-}" = --kvm ]; then
	KVM=(--kvm)
	shift
fi
SECS=${1:-300}
LOG=${UROS_X86_64_LOG:-${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}/stop-unit-check.log}
OUT=$LOG.run
UNIT=uros-stop-unit-check-$$
SAID="run-x86_64.sh: stopped by SIG"

not_asked() {
	echo "stop-unit-check: NOT ASKED -- $* (#801)"
	exit 2
}

# The lock's path and the line are run-x86_64.sh's: read there, not assumed.
grep -qx "LOCK=$LOCK" "$RUN" || not_asked "run-x86_64.sh's lock is not $LOCK"
grep -qF "echo \"$SAID" "$RUN" || not_asked "run-x86_64.sh prints no \"$SAID\" line"
[ -d "$LOCK" ] && not_asked "another run holds $LOCK"
pidof qemu-system-x86_64 qemu-system-i386 > /dev/null && not_asked "a qemu is running"
systemctl --user show-environment > /dev/null 2>&1 || not_asked "no systemd user manager"

# Two commands, so that bash cannot exec the run in its own place: the wrapper
# stays the unit's main process, with no trap, as a campaign's script does.
rm -f "$OUT"
systemd-run --user --quiet --collect --unit="$UNIT" \
	--setenv=UROS_X86_64_LOG="$LOG" --setenv=STOP_UNIT_OUT="$OUT" \
	${UROS_BUILD_DIR:+--setenv=UROS_BUILD_DIR="$UROS_BUILD_DIR"} \
	/bin/bash -c '"$@" > "$STOP_UNIT_OUT" 2>&1; echo "stop-unit-check: the run ended on its own, status $?" >> "$STOP_UNIT_OUT"' \
	bash "$RUN" "${KVM[@]}" --entry 14 "$SECS" -smp 1 ||
	not_asked "systemd-run could not start the run"

# Up means the lock taken, a qemu, and the kernel's first line in the log.
up=0
for _ in $(seq 1 1200); do
	if [ -d "$LOCK" ] && pidof qemu-system-x86_64 > /dev/null &&
	   grep -q "^UrMach x86-64" "$LOG" 2>/dev/null; then
		up=1
		break
	fi
	systemctl --user is-active --quiet "$UNIT" || break
	sleep 0.5
done
if [ $up = 0 ]; then
	systemctl --user stop "$UNIT" 2> /dev/null
	not_asked "the guest never came up under $UNIT (see $OUT)"
fi

systemctl --user stop "$UNIT"
for _ in $(seq 1 60); do
	systemctl --user is-active --quiet "$UNIT" || break
	sleep 0.5
done

bad=0
if [ -d "$LOCK" ]; then
	# No run can hold it now: the unit is gone, and none was in flight
	# before it.  Left there, it would refuse every run after this one.
	rmdir "$LOCK"
	echo "stop-unit-check: WRONG -- $LOCK was left by a run whose unit was stopped, and is removed here (#801)"
	bad=1
fi
if ! grep -q "$SAID" "$OUT" 2>/dev/null; then
	echo "stop-unit-check: WRONG -- the run never said it was stopped: its output ends $(tail -n 1 "$OUT" 2>/dev/null) (#801)"
	bad=1
fi
left=$(pidof qemu-system-x86_64)
if [ -n "$left" ]; then
	# Its own, by the same argument as the lock: none ran before it.
	# shellcheck disable=SC2086
	kill $left 2> /dev/null
	echo "stop-unit-check: WRONG -- the stopped run left qemu $left running, stopped here (#801)"
	bad=1
fi
if [ $bad = 0 ]; then
	echo "stop-unit-check: a run stopped with its unit gave its lock back and said so: $(grep -m 1 "$SAID" "$OUT") (#801)"
fi
exit $bad
