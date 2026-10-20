#!/bin/bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# profile-builds.sh -- both kernels built with the trap phase profile on, so
# that it cannot rot behind a switch no build turns on (#622).
#
# UROS_SYSCALL_PROFILE is off by default, and the kernel's CMakeLists gives
# its i386 arm a reason: the facility is "COMPILED on both targets and cannot
# rot behind a switch only one of them can turn on".  It rotted all the same.
# #648 gave syscall_profile_return_cycles() a third argument on x86-64, and
# i386's definition kept two: "conflicting types", from 15/10 until #622 met
# it on 20/10, because nothing built i386 with the profile on, and the merge
# build and the campaign build neither target with it.
#
# This builds the kernel, and only the kernel, of each target with
# UROS_SYSCALL_PROFILE=ON, each in a build directory of its own under DIR, and
# refuses any warning, as the campaign refuses one in its own build.  A
# compiler that cannot build for -m32 at all leaves i386 NOT ASKED, said, and
# not failed: that is the host's answer, not the tree's.
#
# Usage: profile-builds.sh DIR
# The exit status is the number of targets that failed to build or warned.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
DIR=${1:?usage: profile-builds.sh DIR}
mkdir -p "$DIR" || exit 2
FAILED=0

can_m32() {
	echo 'int x;' | cc -m32 -ffreestanding -c -x c - -o /dev/null 2>/dev/null
}

build() {	# target, extra cmake options...
	local t=$1 b warn
	b="$DIR/build-$t-profile"
	shift
	if ! cmake -S "$REPO/uros" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
	    -DUROS_TARGET_ARCH="$t" -DUROS_SYSCALL_PROFILE=ON "$@" \
	    > "$DIR/configure-$t.log" 2>&1; then
		tail -n 20 "$DIR/configure-$t.log"
		echo "profile-builds: $t: the configure failed: $DIR/configure-$t.log"
		FAILED=$((FAILED + 1))
		return
	fi
	if ! ninja -C "$b" mach_kernel > "$DIR/build-$t.log" 2>&1; then
		grep -m 20 -B 2 'error' "$DIR/build-$t.log"
		echo "profile-builds: $t: the kernel did not build with UROS_SYSCALL_PROFILE on: $DIR/build-$t.log"
		FAILED=$((FAILED + 1))
		return
	fi
	warn=$(grep 'warning:' "$DIR/build-$t.log" | grep -v 'parser.y.*conflicts')
	if [ -n "$warn" ]; then
		printf '%s\n' "$warn" | head -n 20
		echo "profile-builds: $t: $(printf '%s\n' "$warn" | wc -l) warning(s) with UROS_SYSCALL_PROFILE on: $DIR/build-$t.log"
		FAILED=$((FAILED + 1))
		return
	fi
	echo "profile-builds: $t: the kernel built with UROS_SYSCALL_PROFILE on, without a warning"
}

build x86_64
if can_m32; then
	build i386 -DUROS_NCPUS=64
else
	echo "profile-builds: i386: NOT ASKED -- this compiler cannot build for -m32"
fi
exit $FAILED
