#!/bin/sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# uros_libc_check.sh NM MUSL_ARCHIVE PART_ARCHIVE... -- libc-uros.a holds musl
# and our libraries under it, and in it musl's functions are the ones (#733).
# So no part may define a name that musl defines: a second strchr in
# libmach_core went on for months, and which of the two a program got was the
# order of an archive.  Fails, naming each part and each name.
#
# The i386 PIC thunks (__x86.get_pc_thunk.*) are in COMDAT groups that the
# linker folds into one, so two definitions of them are not two functions.
nm=$1
musl=$2
shift 2
defs() {
	"$nm" -g --defined-only "$1" 2>/dev/null |
		awk 'NF >= 3 && $2 ~ /^[TDBRWVuiC]$/ { print $3 }' |
		grep -v '^__x86\.get_pc_thunk\.' | sort -u
}
m=$(mktemp) || exit 2
trap 'rm -f "$m"' EXIT
defs "$musl" > "$m"
[ -s "$m" ] || { echo "uros_libc_check: no names read from $musl" >&2; exit 2; }
bad=0
for part in "$@"; do
	dup=$(defs "$part" | comm -12 - "$m" | tr '\n' ' ')
	if [ -n "$dup" ]; then
		echo "uros_libc_check: $(basename "$part") defines what musl defines: $dup" >&2
		bad=1
	fi
done
exit $bad
