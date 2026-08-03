#!/bin/sh
# What the x86-64 kernel leaves out of the machine-independent tree, and
# whether the reason still holds (#453).
#
# ── What this used to be, and why it is not that any more ────────────────
#
# It used to compile all 102 machine-independent sources with a hand-written
# copy of the kernel's flags, to find out which ones would build for x86-64
# before any of them was in the build.  That question is answered: they are in
# the build, and `ninja -C uros/build-x86_64' compiles them.  A second
# instrument measuring what the build already measures is not redundancy, it
# is a second opinion that can be wrong -- and this one was, three times:
#
#   * it reported every file that PRINTED anything as a failure, so 8 out of
#     94 became 73 out of 29 when that was fixed;
#   * it looked for generated files in the source tree, giving three false
#     failures that hid a real one;
#   * it compiled with -DAT386=1 -DMP_V1_1=1 -DMACH_MACHINE_ROUTINES on a
#     machine that is not i386 and does not set them, manufacturing a failure
#     the kernel does not have.
#
# All three were the same defect: a copy of the build's configuration, kept by
# hand, next to a build that had moved on.  So the copy is gone.  The flags
# below come from compile_commands.json -- the build's own record of how it
# compiles this target -- and cannot disagree with it.
#
# ── The question that is left ────────────────────────────────────────────
#
# Four machine-independent sources are deliberately NOT in the x86-64 kernel.
# The build cannot tell you whether that is still right, because a file it
# does not compile is a file it knows nothing about.  That is this script's
# job now: compile the excluded ones, and say what happens.
#
# Usage: mi-x86_64-probe.sh

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
UROS=$ROOT/uros
SRC=$UROS/src/mach_kernel
BUILD=$UROS/build-x86_64
CC_JSON=$BUILD/compile_commands.json

[ -f "$CC_JSON" ] || {
	echo "no $CC_JSON -- configure the x86-64 build first:"
	echo "  cmake -S $UROS -B $BUILD -DUROS_TARGET_ARCH=x86_64"
	exit 1
}

# The exclusions, and the reason each is out.  This is the only list kept by
# hand, and the cross-check below is what keeps it honest: a file that is
# neither compiled nor named here is reported, so the two cannot drift apart
# in silence.
excluded_reason() {
	case "$1" in
	device/net_io.c)
		echo "in-kernel network stack: no drivers here, MACH_NET_IN_KERNEL=0" ;;
	device/net_device.c)
		echo "in-kernel network device glue: same" ;;
	device/test_device.c)
		echo "pseudo-device whose only purpose is to feed net_io packets" ;;
	device/device_master.c)
		echo "i386 code in device/: needs ECAM, MSI-X and IOMMU here (#457)" ;;
	device/device_master_server.c)
		echo "its MIG stub: the subsystem is absent, not stubbed -- callers get MIG_BAD_ID (#457)" ;;
	*)	echo "" ;;
	esac
}

# The compile line the build actually uses, borrowed from a file that is in
# it.  Everything except the input and output is what the kernel gets.
BORROW=$(python3 - "$CC_JSON" <<'PY'
import json, shlex, sys
db = json.load(open(sys.argv[1]))
for e in db:
    if e["file"].endswith("/kern/startup.c"):
        argv = shlex.split(e["command"])
        out = []
        skip = False
        for i, a in enumerate(argv[1:]):
            if skip:
                skip = False
                continue
            if a in ("-o", "-c"):
                skip = a == "-o"
                continue
            if a.endswith(".c"):
                continue
            out.append(a)
        print(" ".join(shlex.quote(a) for a in out))
        break
else:
    sys.exit("kern/startup.c is not in the build -- borrow a different file")
PY
) || exit 1

# Which machine-independent files the build compiles.
#
# WARNING: two roots, not one.  Some of them are GENERATED -- the MIG stubs --
# and live in the build tree with no counterpart under src/.  Looking only at
# src/ reports them as missing, which is the exact shape of the second
# measurement error this script made before it was rewritten.  Here it would
# have been caught anyway, because a file that is neither compiled nor
# explained is reported rather than assumed; that is the point of the
# cross-check.
COMPILED=$(python3 - "$CC_JSON" "$SRC" "$BUILD/src/mach_kernel" <<'EOPY'
import json, sys, os
db = json.load(open(sys.argv[1]))
roots = [os.path.realpath(r) for r in sys.argv[2:]]
for e in db:
    f = os.path.realpath(e["file"])
    for r in roots:
        if f.startswith(r + os.sep):
            rel = f[len(r) + 1:]
            if rel.split(os.sep)[0] in ("kern", "ipc", "vm", "device"):
                print(rel)
            break
EOPY
)

echo "### x86-64 exclusions  $(date -Is)"
echo

rc=0
unexplained=0
while read -r f; do
	[ -n "$f" ] || continue
	case "
$COMPILED
" in
	*"
$f
"*)	continue ;;			# in the build: not this script's business
	esac

	why=$(excluded_reason "$f")
	if [ -z "$why" ]; then
		printf '?? %-26s NOT compiled and NOT explained\n' "$f"
		unexplained=$((unexplained + 1))
		rc=1
		continue
	fi

	# Would it compile if it were let in?
	#
	# Generated sources have no counterpart under src/, so look in the
	# build tree when the source tree has none -- and only call it missing
	# when neither does.
	path=$SRC/$f
	[ -f "$path" ] || path=$BUILD/src/mach_kernel/$f
	if [ ! -f "$path" ]; then
		printf 'OUT %-26s NOT GENERATED -- %s\n' "$f" "$why"
		continue
	fi
	err=$(eval "cc $BORROW -fsyntax-only \"$path\"" 2>&1)
	if [ $? -eq 0 ]; then
		printf 'OUT %-26s compiles  -- %s\n' "$f" "$why"
	else
		first=$(printf '%s\n' "$err" | grep -m1 'error:' | sed 's/.*error: //')
		printf 'OUT %-26s DOES NOT  -- %s\n' "$f" "$why"
		printf '    %-26s   first error: %s\n' "" "$first"
	fi
done <<EOF
$(cat "$(dirname "$0")/mi-files.txt")
EOF

echo
echo "### $(printf '%s\n' "$COMPILED" | grep -c .) machine-independent files in the build"
[ $unexplained -eq 0 ] || echo "### $unexplained unexplained -- add a reason or add it to the build"
exit $rc
