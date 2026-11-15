#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# umbrella-relink-check.sh [BUILD_DIR] -- the programs linked against the
# umbrella libc.so are linked again when libc.so is (#930).
#
# Their links are commands of their own, and each used to wait for the
# musl_umbrella target, which orders the build and gives no file to depend on:
# a libc.so relinked without a name they called left their old links in place,
# and only a build from nothing said so (#917 met it: libc.so had stopped
# exporting mach_error and the build passed).  Each now depends on the stamp
# the umbrella's link touches, ${UROS_MUSL_UMBRELLA_STAMP} in uros/CMakeLists.txt.
#
# Which programs, ninja says: every command that waits for musl_umbrella and
# links with -dynamic-linker.  The bundle and the ISO wait for the umbrella too,
# for the order alone -- libc.so goes on the disk, not into them -- and are not
# links.  Then two things, for each:
#
#   - it depends on the stamp's FILE, not only on the target;
#   - with the build up to date and the stamp made its newest file, a build
#     links it again: its file comes out newer than the stamp.
#
# Not `ninja -n': the globs with CONFIGURE_DEPENDS make every run check its
# directories first, and a dry run stops there and plans nothing after it.
#
# Exit status: 0 every program holds · 1 one does not · 2 the build directory
# cannot be used, or no program was found to check.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-${UROS_BUILD_DIR:-$HERE/uros/build-x86_64}}
UMBRELLA=src/contrib/musl_umbrella
STAMP_REL=src/contrib/musl-umbrella.stamp
STAMP=$BUILD/$STAMP_REL

die() {	# STATUS MESSAGE
	echo "umbrella-relink-check: $2" >&2
	exit "$1"
}

nj() {
	LC_ALL=C ninja -C "$BUILD" "$@" 2>/dev/null
}

# The names under `outputs:' in `ninja -t query NODE', relative ones only:
# ninja lists each twice, by its path in the build and by its absolute path.
dependents() {	# NODE
	nj -t query "$1" | sed -n '/^  outputs:/,$p' | sed -n 's/^    \([^/].*\)$/\1/p'
}

[ -f "$BUILD/build.ninja" ] || die 2 "no build.ninja in $BUILD"
nj > /dev/null || die 2 "the build does not pass as it is: nothing to measure"
[ -f "$STAMP" ] || die 2 "no umbrella stamp in $BUILD after a build"

customs=$(nj -t targets all | awk -F': ' '$2 == "CUSTOM_COMMAND" { print $1 }')
on_stamp=$(dependents "$STAMP_REL")
programs=()
bad=0
for out in $(dependents "$UMBRELLA"); do
	printf '%s\n' "$customs" | grep -qxF "$out" || continue
	nj -t commands -s "$out" | grep -q -- '-dynamic-linker' || continue
	programs+=("$out")
	if printf '%s\n' "$on_stamp" | grep -qxF "$out"; then
		echo "umbrella-relink-check: $out links against libc.so and depends on its stamp"
	else
		echo "umbrella-relink-check: $out links against libc.so and waits for $UMBRELLA without depending on $STAMP_REL -- WRONG (#930)"
		bad=1
	fi
done
[ ${#programs[@]} -gt 0 ] ||
	die 2 "no command that waits for $UMBRELLA and links with -dynamic-linker: the check found nothing to check"

sleep 1
touch "$STAMP"
nj > /dev/null || die 2 "the build failed after the stamp was touched"
for out in "${programs[@]}"; do
	if [ "$BUILD/$out" -nt "$STAMP" ]; then
		echo "umbrella-relink-check: $out was linked again"
	else
		echo "umbrella-relink-check: $out is older than libc.so's stamp after the build: not linked again -- WRONG (#930)"
		bad=1
	fi
done
exit $bad
