#!/bin/sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# uros_libc_check.sh NM MUSL_ARCHIVE PART_ARCHIVE... -- libc-uros.a holds musl
# and our libraries under it, and in it musl's functions are the ones (#733).
# So no part may define a name that musl defines: a second strchr in
# libmach_core went on for months, and which of the two a program got was the
# order of an archive.  Nor may two members of ours both define one for good,
# a pair with a weak or a common member being one a link settles by itself:
# the first reference would pull whichever the index lists first, and a link
# that came to need the other as well would stop.  Fails, naming each name and
# where it is.
#
# uros_libc_check.sh --must-refuse NM MUSL_ARCHIVE CANARY_1 CANARY_2 -- the same
# check, every time libc-uros.a is made, on two parts made to fail it:
# uros_libc_check_canary.c's object once in each, and in the second
# uros_libc_check_canary_pair.c's twice under one member name.  Each part must
# be named for strchr (T) and environ (B), which musl defines; those two and
# uros_libc_check_canary (D) must each be found in its two members, one a part,
# and uros_libc_check_canary_pair (D) in its two that share a name.  A check
# that has stopped reading one of those types, or a part after the first, or a
# pair across parts, or a member that shares a name, or that wants more than a
# pair, gets the canary through, and the build stops.
#
# The i386 PIC thunks (__x86.get_pc_thunk.*) are in COMDAT groups that the
# linker folds into one, so two definitions of them are not two functions.
if [ "$1" = --must-refuse ]; then
	shift
	said=$(sh "$0" "$@" 2>&1 >/dev/null)
	rc=$?
	if [ "$rc" = 2 ]; then
		echo "uros_libc_check: the check could not run on the canary: $said" >&2
		exit 2
	fi
	seen=$(printf '%s\n' "$said" | awk '
		/defines what musl defines: / && / environ / && / strchr / { musl++ }
		/two of ours define (environ|strchr|uros_libc_check_canary|uros_libc_check_canary_pair) for good:/ {
			if (gsub(/\.a:/, "&") == 2)
				twice++
		}
		END { print musl + 0, twice + 0 }')
	if [ "$rc" != 1 ] || [ "$seen" != "2 4" ]; then
		echo "uros_libc_check: the canary got through, so the check is blind: $said" >&2
		exit 2
	fi
	exit 0
fi

nm=$1
musl=$2
shift 2
if [ $# -eq 0 ]; then
	echo "uros_libc_check: no parts to check" >&2
	exit 2
fi
# nm -A puts the archive's path in front of each line it prints, and a path
# with a space in it would split that line into other fields.
tab=$(printf '\t')
for a in "$musl" "$@"; do
	case $a in
	*" "* | *"$tab"*)
		echo "uros_libc_check: $a has a space in its path, which nm -A's lines cannot carry" >&2
		exit 2 ;;
	esac
done
t=$(mktemp -d) || exit 2
trap 'rm -rf "$t"' EXIT

# defs ARCHIVE OUT -- every global ARCHIVE defines, "name type archive:member"
# a line.  An archive nm cannot read, or one that reads as nothing, would pass
# every check, so either stops the build.
defs() {
	if ! "$nm" -g --defined-only -A "$1" > "$t/nm" 2> "$t/err"; then
		echo "uros_libc_check: $nm could not read $1: $(head -n 1 "$t/err")" >&2
		exit 2
	fi
	awk -v lib="$(basename "$1")" '
		NF >= 3 && $2 ~ /^[ABCDIRTVWiu]$/ && $3 !~ /^__x86\.get_pc_thunk\./ {
			n = split($1, f, ":")
			print $3, $2, lib ":" (n >= 3 ? f[n - 1] : "?")
		}' "$t/nm" > "$2"
	if [ ! -s "$2" ]; then
		echo "uros_libc_check: no names read from $1" >&2
		exit 2
	fi
}

defs "$musl" "$t/musl"
cut -d ' ' -f 1 "$t/musl" | sort -u > "$t/musl.names"
: > "$t/ours"
bad=0
for part in "$@"; do
	defs "$part" "$t/part"
	cat "$t/part" >> "$t/ours"
	dup=$(cut -d ' ' -f 1 "$t/part" | sort -u | comm -12 - "$t/musl.names" |
		tr '\n' ' ')
	if [ -n "$dup" ]; then
		echo "uros_libc_check: $(basename "$part") defines what musl defines: $dup" >&2
		bad=1
	fi
done

# Strong is everything but weak (W, V) and common (C), which merges.  One line
# a member and a name, so two members with one name count as two: the i386
# libmach.a holds two crt0.c.o.
awk '$2 ~ /^[ABDIRTiu]$/ { print $1, $3 }' "$t/ours" |
	awk '{ n[$1]++; where[$1] = where[$1] " " $2 }
	     END { for (s in n) if (n[s] > 1) print s ":" where[s] }' |
	sort > "$t/twice"
while read -r line; do
	echo "uros_libc_check: two of ours define ${line%%:*} for good:${line#*:}" >&2
	bad=1
done < "$t/twice"
exit $bad
