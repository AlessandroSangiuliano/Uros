#!/bin/sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# uros_umbrella_exports_check.sh NM LIBC_SO LIST -- each name of LIST, one a
# line ('#' starts a comment), is defined in LIBC_SO's dynamic symbol table
# (#930).  What exports one of ours from the umbrella is its object reaching
# the link loose: --exclude-libs=ALL makes every archive member's symbols
# local, so a name whose object came by reference out of an archive is in
# libc.so and exported by nobody, and the program that calls it fails at its
# link, or at its start if it was linked before.  Fails naming each.
#
# uros_umbrella_exports_check.sh --must-refuse NM LIBC_SO -- the same check
# for a name made up to be missing, each time it runs: a check that finds
# whatever it is asked for, or reads nothing, stops the build too.
set -u
refuse=0
if [ "${1:-}" = --must-refuse ]; then
	refuse=1
	shift
fi
NM=${1:?nm}
SO=${2:?libc.so}
dyn=$("$NM" -D --defined-only "$SO" 2>/dev/null) || {
	echo "uros_umbrella_exports_check: $NM cannot read $SO's dynamic symbols -- WRONG (#930)"
	exit 1
}
names=$(printf '%s\n' "$dyn" | awk 'NF >= 3 { print $NF }')
if [ -z "$names" ]; then
	echo "uros_umbrella_exports_check: no dynamic symbol read in $SO -- WRONG (#930)"
	exit 1
fi
exported() {	# NAME
	printf '%s\n' "$names" | grep -qxF "$1"
}
if [ $refuse = 1 ]; then
	canary=uros_umbrella_exports_check_canary
	if exported "$canary"; then
		echo "uros_umbrella_exports_check: $SO 'exports' $canary, a name nothing defines: the check is blind -- WRONG (#930)"
		exit 1
	fi
	exit 0
fi
LIST=${3:?list}
missing=0
listed=0
while read -r name rest; do
	case $name in
	''|'#'*) continue ;;
	esac
	listed=$((listed + 1))
	if ! exported "$name"; then
		echo "uros_umbrella_exports_check: $SO does not export $name, which $LIST says it does -- its object must reach the link loose (libmach_export_objs) -- WRONG (#930)"
		missing=1
	fi
done < "$LIST"
if [ $listed = 0 ]; then
	echo "uros_umbrella_exports_check: no name read in $LIST -- WRONG (#930)"
	exit 1
fi
exit $missing
