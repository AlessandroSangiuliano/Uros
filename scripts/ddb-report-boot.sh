#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ddb-report-boot.sh -- one boot of GRUB entry 55, and ddb-report-check.sh's
# verdict on it (#750).
#
# Entry 55 (-rt) panics on purpose with the debugger armed, so the boot ends
# at a prompt nobody answers, and what it says is in what came before that
# prompt: scripts/ddb-report-check.sh reads the log and says the answer.  An
# ablation of the report could not name a line of the log in Expect:, since
# what it takes out is lines.  This boots the entry once, with the build in
# UROS_BUILD_DIR and the log in UROS_X86_64_LOG as run-x86_64.sh takes them,
# and prints the check's line, so that a patch can say
#
#   Script: scripts/ddb-report-boot.sh
#   Expect: ddb-report-check: WRONG
#
# -t's panic is raised in clock_event_tick(), so the backtrace must have a
# frame there.
#
# Usage:  ddb-report-boot.sh [--kvm] [SECONDS]
#
# Exit status: 0 the check said PASS · 1 it said WRONG, or NOT ASKED, which is
# not a pass either · 2 the boot could not be made, which says nothing about
# the kernel (ablate.sh stops on it).

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
KVM=()
if [ "${1:-}" = --kvm ]; then
	KVM=(--kvm)
	shift
fi
SECS=${1:-600}
LOG=${UROS_X86_64_LOG:-${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}/ddb-report-boot.log}

# The entry ends in a panic, which run-x86_64.sh calls the kernel failing: 0
# and 1 are both a boot that was made.
UROS_X86_64_LOG=$LOG "$HERE/scripts/run-x86_64.sh" "${KVM[@]}" --entry 55 "$SECS" -smp 4
rc=$?
case $rc in
0|1)	;;
*)	echo "ddb-report-boot: the boot was not made (run-x86_64.sh status $rc)"
	exit 2 ;;
esac

"$HERE/scripts/ddb-report-check.sh" "$LOG" clock_event_tick
[ $? = 0 ]
