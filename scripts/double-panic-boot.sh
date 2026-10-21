#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# double-panic-boot.sh -- one -Z boot, and double-panic-check.sh's verdict on
# it (#702).
#
# -Z (GRUB entry 24, two processors panic at once, #599) does not answer with
# a line of its log: scripts/double-panic-check.sh reads the log and says the
# answer.  So an ablation of the panic's console could not name a line in
# Expect:, and 599-halt-order.patch says Measure: and was judged by hand.
# This boots the entry once, with the build in UROS_BUILD_DIR and the log in
# UROS_X86_64_LOG as run-x86_64.sh takes them, and prints the check's line,
# so that a patch can say
#
#   Script: scripts/double-panic-boot.sh
#   Expect: double-panic-check: WRONG
#
# Usage:  double-panic-boot.sh [--kvm] [SECONDS]
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
LOG=${UROS_X86_64_LOG:-${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}/double-panic-boot.log}

# -Z ends in a panic, which run-x86_64.sh calls the kernel failing: 0 and 1
# are both a boot that was made.
UROS_X86_64_LOG=$LOG "$HERE/scripts/run-x86_64.sh" "${KVM[@]}" --entry 24 "$SECS" -smp 4
rc=$?
case $rc in
0|1)	;;
*)	echo "double-panic-boot: the boot was not made (run-x86_64.sh status $rc)"
	exit 2 ;;
esac

"$HERE/scripts/double-panic-check.sh" "$LOG"
[ $? = 0 ]
