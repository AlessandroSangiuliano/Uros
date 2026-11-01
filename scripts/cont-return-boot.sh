#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# cont-return-boot.sh -- one -l boot, and cont-return-check.sh's verdict on it
# (#771).
#
# -l (GRUB entry 58, a continuation that returns) ends in a panic on purpose,
# which run-x86_64.sh calls the kernel failing, so its fixed tree could never
# pass an ablate.sh A/B on the harness's status.  This boots the entry once,
# with the build in UROS_BUILD_DIR and the log in UROS_X86_64_LOG as
# run-x86_64.sh takes them, and prints the check's line, so that a patch can
# say
#
#   Script: scripts/cont-return-boot.sh
#   Expect: cont-return-check: WRONG
#
# Usage:  cont-return-boot.sh [--kvm] [SECONDS]
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
LOG=${UROS_X86_64_LOG:-${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}/cont-return-boot.log}
# -l ends in a panic, which run-x86_64.sh calls the kernel failing: 0 and 1
# are both a boot that was made.
UROS_X86_64_LOG=$LOG "$HERE/scripts/run-x86_64.sh" "${KVM[@]}" --entry 58 "$SECS" -smp 1
rc=$?
case $rc in
0|1)	;;
*)	echo "cont-return-boot: the boot was not made (run-x86_64.sh status $rc)"
	exit 2 ;;
esac
"$HERE/scripts/cont-return-check.sh" "$LOG"
[ $? = 0 ]
