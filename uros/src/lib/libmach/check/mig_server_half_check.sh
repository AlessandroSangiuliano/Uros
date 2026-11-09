#!/bin/sh
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# mig_server_half_check.sh NM ARCHIVE... -- libmach carries the user halves of
# the interfaces it speaks and no server half (#931).  A server half calls
# routines only its server writes, so in a library it leaves their names
# undefined -- in libmach_core they were 25 of the 30 names nm -u found, and
# #917's three real omissions hid among them -- and the program that answers
# an interface generates that half in its own build (add_mig_userland_server
# in cmake/mig.cmake).  Every half MIG writes defines
# <subsystem>_server_routine, and nothing else in these archives does: this
# fails, naming each, if one is back.  mach_msg_server.c is libmach's own
# receive loop, and defines no such routine.
#
# mig_server_half_check.sh --must-refuse NM CANARY -- the same check on an
# object made to fail it, every time it runs: a check that has stopped seeing
# the routine, or reads nothing at all, stops the build too.
set -u
refuse=0
if [ "${1:-}" = --must-refuse ]; then
	refuse=1
	shift
fi
NM=${1:?nm}
shift
found=0
for a in "$@"; do
	defs=$("$NM" -A --defined-only "$a" 2>/dev/null) || {
		echo "mig_server_half_check: $NM cannot read $a -- WRONG (#931)"
		exit 1
	}
	if ! printf '%s\n' "$defs" | grep -q ' T '; then
		echo "mig_server_half_check: no function defined in $a was read -- WRONG (#931)"
		exit 1
	fi
	hits=$(printf '%s\n' "$defs" | grep -E ' T [A-Za-z0-9_]+_server_routine$')
	[ -n "$hits" ] || continue
	found=1
	[ $refuse = 1 ] && continue
	printf '%s\n' "$hits" | sed 's/^/mig_server_half_check: /;
		s/$/ -- a MIG server half in a library of user halves -- WRONG (#931)/'
done
if [ $refuse = 1 ]; then
	[ $found = 1 ] && exit 0
	echo "mig_server_half_check: the canary's canary_server_routine was not seen: the check is blind -- WRONG (#931)"
	exit 1
fi
exit $found
