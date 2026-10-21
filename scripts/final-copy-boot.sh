#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# final-copy-boot.sh -- one boot that ends in a stop, and final-copy-check.sh's
# verdict on it (#702).
#
# The console's final copy, mixed a byte of each processor's copy in turn, is a
# line no Expect: can name, and the harness's GARBLED check saw it in 2 boots
# of 6.  This boots what it is given, with the build in UROS_BUILD_DIR and the
# log in UROS_X86_64_LOG as run-x86_64.sh takes them, and prints the check's
# line, so that a patch can say
#
#   Script: scripts/final-copy-boot.sh
#   Script: --entry
#   Script: 21
#   ...
#   Expect: final-copy-check: WRONG
#
# Usage:  final-copy-boot.sh [--kvm] RUN-ARGS...
#         (run-x86_64.sh's arguments after the accelerator)
#
# Exit status: 0 the boot passed and the check said PASS · 1 either did not,
# or the check said NOT ASKED, which is not a pass either · 2 the boot could
# not be made, which says nothing about the kernel (ablate.sh stops on it).

set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
KVM=()
if [ "${1:-}" = --kvm ]; then
	KVM=(--kvm)
	shift
fi
[ $# -gt 0 ] || { echo "final-copy-boot: no run-x86_64.sh arguments" >&2; exit 2; }
LOG=${UROS_X86_64_LOG:-${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}/final-copy-boot.log}

UROS_X86_64_LOG=$LOG "$HERE/scripts/run-x86_64.sh" "${KVM[@]}" "$@"
rc=$?
case $rc in
0|1)	;;
*)	echo "final-copy-boot: the boot was not made (run-x86_64.sh status $rc)"
	exit 2 ;;
esac

"$HERE/scripts/final-copy-check.sh" "$LOG"
check=$?
[ $rc = 0 ] && [ $check = 0 ]
