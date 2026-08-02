#!/bin/sh
# Probe: compile the machine-independent kernel sources with x86-64 flags and
# classify the FIRST error of each.  This is a MEASUREMENT, not a build --
# it deliberately borrows i386 header directories so that we can see what lies
# *underneath* the missing configuration headers.
#
#   pass A  (default)  x86-64 include path only         -> what the tree does today
#   pass B  (-b)       + i386/AT386 + generated MIG hdr -> the real 64-bit inventory
#
# Usage: mi-x86_64-probe.sh [-b] > log

set -u
UROS=$(cd "$(dirname "$0")/../uros" && pwd)
SRC=$UROS/src/mach_kernel
GCCINC=$(cc -m64 -print-file-name=include)
PASS=A
[ "${1:-}" = "-b" ] && PASS=B

# Exactly the x86-64 target's KERNEL_DEFINES_BARE, and nothing else (#453).
#
# It used to carry -DAT386=1 -DEISA=1 -DHIMEM=1 -DMP_V1_1=1 -DNPCI=1 as
# well -- i386 hardware knobs, on a machine that is not i386 -- and
# -DMACH_MACHINE_ROUTINES, which this target's configuration now sets to 0.
# A probe that compiles with different flags from the build is not measuring
# the build: it can hide a failure the kernel would hit, and manufacture one
# it would not.  MACH_MACHINE_ROUTINES was doing the second, keeping
# kern/ipc_kobject.c asking for a <machine/machine_routines.h> that this
# machine has decided not to have.
#
# The rest arrive from the generated configuration headers on the include
# path below, which is also where the build gets them.
DEFINES="-DKERNEL -DMACH_KERNEL -DKERNEL_PRIVATE -DKERNEL_SERVER=1
 -DNCPUS=64 -DMACH_ASSERT=1 -DMACH_DEBUG=1 -DMACH_KDB=1 -DMACH_HOST=1
 -DSTAT_TIME -DTASK_SWAPPER=1 -D__NO_UNDERSCORES__ -Dmig_internal="

FLAGS="-m64 -mcmodel=kernel -mno-red-zone -mgeneral-regs-only -fcf-protection=none
 -ffreestanding -fno-pic -fno-pie -fno-stack-protector -fno-builtin -nostdinc
 -fno-omit-frame-pointer -isystem $GCCINC -std=gnu11 -Wall -fsyntax-only"

# The x86-64 target's own include path, in the target's own order:
# uapi, the machine/ links, x86_64/, then the generated ODE knobs (#450).
INC_A="-I$UROS/uapi -I$UROS/build-x86_64/arch-include -I$SRC/x86_64
 -I$SRC -I$SRC/include -I$UROS/build-x86_64/config-include
 -I$UROS/build-x86_64/src/mach_kernel -I$UROS/build-x86_64/src/mach_kernel/mach
 -I$UROS/build-x86_64/src/mach_kernel/mach_debug
 -I$UROS/build-x86_64/src/mach_kernel/device
 -I$UROS/export/include"
INC_B="$INC_A -I$SRC/i386/AT386 -I$SRC/i386
 -I$UROS/build/src/mach_kernel -I$UROS/build/src/mach_kernel/mach
 -I$UROS/build/src/mach_kernel/mach_debug -I$UROS/build/src/mach_kernel/device"

if [ "$PASS" = A ]; then INC=$INC_A; else INC=$INC_B; fi

echo "### pass $PASS  $(date -Is)"
ok=0; bad=0
while read -r f; do
	# Some of these are generated, not written: the MIG stubs live in the
	# build directory and there is nothing under $SRC to compile.  Reported
	# as "File o directory non esistente", they looked for weeks like three
	# more sources that did not build, when in fact they existed and were
	# correct.  Second instrument error of the day, so it is fixed rather
	# than remembered: look in the build tree when the source tree has no
	# such file, and only call it missing when neither does.
	src=$SRC/$f
	[ -f "$src" ] || src=$UROS/build-x86_64/src/mach_kernel/$f

	err=$(cc $DEFINES $FLAGS $INC -c "$src" -o /dev/null 2>&1)
	rc=$?
	if [ $rc -eq 0 ]; then
		ok=$((ok + 1))
		w=$(printf '%s\n' "$err" | grep -c 'warning:' || true)
		if [ "$w" -gt 0 ]; then
			printf 'OK  %-4s %s\n' "${w}w" "$f"
		else
			printf 'OK       %s\n' "$f"
		fi
	else
		bad=$((bad + 1))
		first=$(printf '%s\n' "$err" | grep -m1 -E 'error:' | sed 's/.*error: //')
		[ -z "$first" ] && first=$(printf '%s\n' "$err" | head -1)
		n=$(printf '%s\n' "$err" | grep -c 'error:')
		printf 'FAIL %-4s %-34s %s\n' "$n" "$f" "$first"
	fi
done < $(dirname "$0")/mi-files.txt
echo "### pass $PASS: $ok compile, $bad fail"
