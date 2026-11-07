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
# uros_libc_check.sh --must-refuse NM MUSL_ARCHIVE CANARY_1 CANARY_2 CANARY_3 --
# the same check, every time libc-uros.a is made, on parts made to fail it.
# uros_libc_check_canary.c's object goes once into the first and the second,
# and uros_libc_check_canary_pair.c's twice under one member name into the
# second and the third.  On the first two, each part must be named for exactly
# the four musl names the canary holds -- strong, weak object and weak only --
# and the check must say six names of ours are defined twice, each in exactly
# two members: five as pairs across the parts, and the pair's name as two
# members that share a name.  The canary's look-alike of a PIC thunk is not one
# of the six.  On the first alone, only the musl half may speak, and on the
# third alone only the other, and each must fail the check by itself.  A check
# that has stopped reading one of T, B, D, V or W, or a part after the first,
# or a pair across parts, or a member that shares a name, that wants more than
# a pair, that has widened or lost the thunk filter, or with a half that prints
# and no longer fails, gets the canary through, and the build stops.  The
# canary has no R, A, I, i or u symbol: no part defines one today.
#
# The i386 PIC thunks (__x86.get_pc_thunk.*) are in COMDAT groups that the
# linker folds into one, so two definitions of them are not two functions.
if [ "$1" = --must-refuse ]; then
	shift
	nm=$1 musl=$2 c1=$3 c2=$4 c3=$5
	# run PART... -- "status, musl lines naming all four, other musl lines,
	# lines of ours defined twice, those that are the canary's six names in
	# exactly two members", for the check on musl and those parts.
	run() {
		said=$(sh "$0" "$nm" "$musl" "$@" 2>&1 >/dev/null)
		rc=$?
		[ "$rc" = 2 ] && printf '%s\n' "$said" >&2
		printf '%s\n' "$said" | awk -v rc="$rc" '
			/defines what musl defines: / {
				if (/ __errno_location / && / environ / && / malloc / && / strchr /)
					musl++
				else
					other++
			}
			/two of ours define / {
				dup++
				if (/two of ours define (__errno_location|environ|malloc|strchr|uros_libc_check_canary|uros_libc_check_canary_pair) for good:/ &&
				    gsub(/\.a:/, "&") == 2)
					named++
			}
			END { print rc, musl + 0, other + 0, dup + 0, named + 0 }'
	}
	both=$(run "$c1" "$c2")
	musl_half=$(run "$c1")
	ours_half=$(run "$c3")
	case "$both/$musl_half/$ours_half" in
	2\ * | */2\ *)
		echo "uros_libc_check: the check could not run on the canary" >&2
		exit 2 ;;
	esac
	if [ "$both" != "1 2 0 6 6" ] || [ "$musl_half" != "1 1 0 0 0" ] ||
	   [ "$ours_half" != "1 0 0 1 1" ]; then
		echo "uros_libc_check: the canary got through, so the check is blind:" \
		     "both parts $both, musl half $musl_half, half of ours $ours_half" >&2
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
