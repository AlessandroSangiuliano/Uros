#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# make-baremetal-x86_64.sh -- an x86-64 boot image for a real machine (#595).
#
# Everything the harness boots is a QEMU disk: GRUB built for i386-pc only, a
# grub.cfg that starts on the serial line, no video mode asked for, and a
# bundle whose servers look for QEMU's virtio and AHCI disks.  None of that
# fits omen, which has no COM port -- a serial terminal stalls GRUB there --
# and whose disks are real.  This is make-omen-boot.sh's ISO, for this target:
#
#   - BIOS and UEFI.  grub-mkrescue puts GRUB for i386-pc and for x86_64-efi
#     on one hybrid ISO, which boots from a USB stick either way.
#   - No serial.  GRUB's terminal is the console, and gfxpayload makes it hand
#     the kernel a linear framebuffer through multiboot2's framebuffer tag,
#     which is where the kernel's own console draws (#568).
#   - Diskless, and unable to reach a disk.  GRUB loads the kernel, bootstrap
#     and the bundle into RAM, and the bundle holds name_server alone -- or,
#     with --bench-only, name_server and ipc_bench, the way #332 measures.  No
#     block server, no AHCI module: a test that writes a block (#596) cannot
#     run where the disks are somebody's.
#
# The script writes no device and needs no root.  Putting the ISO on a stick
# is a step the user takes, with the dd line it prints.
#
# Usage:  scripts/make-baremetal-x86_64.sh [--bench-only] [-o OUT.iso] [-- KERNEL-FLAGS...]
#   --bench-only  name_server and ipc_bench in the bundle (#332)
#   -o OUT.iso    where the ISO goes (default: the build directory)
#   KERNEL-FLAGS  given to the kernel on the multiboot2 line, after -f
#
# The build is UROS_BUILD_DIR when set, else uros/build-x86_64, and must be an
# x86-64 build (its uros-arch says so).
#
# Exit status: 0 the ISO was made · 1 a file or a tool is missing · 2 the
# command line is wrong.

set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
OUT=""
BENCH_ONLY=0
# -f: the kernel puts on the screen, at a readable pace, what the boot said
# before the screen was ready -- the calibration and the early self-tests,
# which on a machine with no serial port reach no one otherwise (#595).
FLAGS=" -f"

while [ $# -gt 0 ]; do
	case "$1" in
	--bench-only)	BENCH_ONLY=1; shift ;;
	-o)		OUT=${2:?-o needs a path}; shift 2 ;;
	--)		shift; FLAGS="$FLAGS $*"; break ;;
	*)		echo "usage: $0 [--bench-only] [-o OUT.iso] [-- KERNEL-FLAGS...]" >&2; exit 2 ;;
	esac
done

arch=$(cat "$BUILD/uros-arch" 2>/dev/null || echo "?")
[ "$arch" = x86_64 ] || { echo "make-baremetal: $BUILD is not an x86-64 build (uros-arch: $arch)" >&2; exit 1; }
for t in grub-mkrescue xorriso mformat; do
	command -v $t >/dev/null || { echo "make-baremetal: $t is missing (grub, xorriso, mtools)" >&2; exit 1; }
done
for p in i386-pc x86_64-efi; do
	[ -d /usr/lib/grub/$p ] || { echo "make-baremetal: GRUB for $p is missing: the ISO would not boot under both firmwares" >&2; exit 1; }
done

KERNEL=$BUILD/export/uros/boot/mach_kernel
SBIN=$BUILD/export/uros/x86_64/user/sbin
MKBUNDLE=$BUILD/tools/mkbundle
for f in "$KERNEL" "$SBIN/bootstrap" "$SBIN/name_server" "$MKBUNDLE"; do
	[ -f "$f" ] || { echo "make-baremetal: missing $f -- build the x86-64 tree first" >&2; exit 1; }
done
[ "$BENCH_ONLY" = 0 ] || [ -f "$SBIN/ipc_bench" ] || { echo "make-baremetal: missing $SBIN/ipc_bench" >&2; exit 1; }

[ -n "$OUT" ] || OUT=$BUILD/uros-baremetal-x86_64.iso
TREE=$(mktemp -d "${TMPDIR:-/tmp}/uros-baremetal.XXXXXX")
trap 'rm -rf "$TREE"' EXIT
mkdir -p "$TREE/boot/grub"

# The bundle: bootstrap.conf and the binaries it names, nothing that reads a disk.
conf=$TREE/bootstrap.conf
printf 'name_server name_server\n' > "$conf"
set -- "bootstrap.conf:$conf" "name_server:$SBIN/name_server"
if [ "$BENCH_ONLY" = 1 ]; then
	printf 'ipc_bench ipc_bench\n' >> "$conf"
	set -- "$@" "ipc_bench:$SBIN/ipc_bench"
fi
"$MKBUNDLE" -o "$TREE/boot/bootstrap.bundle" "$@" >/dev/null

cp "$KERNEL" "$TREE/boot/mach_kernel"
cp "$SBIN/bootstrap" "$TREE/boot/bootstrap"

# No serial: on omen a serial terminal stalls GRUB.  all_video pulls in efi_gop
# under UEFI and vbe under BIOS, and gfxpayload asks for the panel's own mode,
# so the kernel is handed a framebuffer in either case.
cat > "$TREE/boot/grub/grub.cfg" <<EOF
set timeout=5
set default=0
terminal_input console
terminal_output console
insmod all_video
set gfxpayload=auto

menuentry "UrMach x86-64, bare metal$([ "$BENCH_ONLY" = 1 ] && echo ", ipc_bench")" {
	multiboot2 /boot/mach_kernel$FLAGS
	module2 /boot/bootstrap bootstrap
	module2 /boot/bootstrap.bundle bootstrap.bundle
	boot
}
EOF

grub-mkrescue -o "$OUT" "$TREE" >/dev/null 2>"$TREE/mkrescue.log" || {
	echo "make-baremetal: grub-mkrescue failed:" >&2; cat "$TREE/mkrescue.log" >&2; exit 1; }

echo "make-baremetal: $OUT ($(du -h "$OUT" | cut -f1)), BIOS and UEFI, bundle: name_server$([ "$BENCH_ONLY" = 1 ] && echo " ipc_bench"), kernel flags:${FLAGS:- none}"
echo "make-baremetal: to put it on a USB stick (check the device first, this erases it):"
echo "    sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
