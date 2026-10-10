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
#     and the bundle into RAM.  The bundle is the build's bootstrap-nodisk
#     (target bootstrap_bundle_nodisk): the servers and tests that drive no
#     device, and no block server, no AHCI module, nothing that writes a block
#     (#596) or claims a device where the disks are somebody's.  With
#     --bench-only it is name_server and ipc_bench alone, the way #332
#     measures.
#
# The script writes no device and needs no root.  Putting the ISO on a stick
# is a step the user takes, with the dd line it prints.
#
# Usage:  scripts/make-baremetal-x86_64.sh [--bench-only] [-o OUT.iso]
#             [--kernel NAME=PATH]... [--entry [NAME:]FLAGS]... [-- KERNEL-FLAGS...]
#   --bench-only  name_server and ipc_bench in the bundle (#332)
#   -o OUT.iso    where the ISO goes (default: the build directory)
#   --kernel      another kernel in the image, as /boot/NAME -- a build with an
#                 ablation, say; NAME is letters, digits, '.', '_' or '-'
#   --entry       one menu entry: the build's kernel, or NAME's, with -f and
#                 FLAGS ("" for none).  The first is GRUB's default; with more
#                 than one, GRUB waits for a choice, so one stick and one dd
#                 serve every case a session on the machine asks for
#   --before      a GRUB command the next --entry runs before the kernel, such
#                 as "wrmsr 0x1b 0xfee00d00" for a boot processor already in
#                 x2APIC mode, as some firmware hands it over (#659)
#   KERNEL-FLAGS  without --entry, the one entry's flags after -f
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
KNAMES=()
KPATHS=()
ENTRIES=()
BEFORES=()
BEFORE=""

while [ $# -gt 0 ]; do
	case "$1" in
	--bench-only)	BENCH_ONLY=1; shift ;;
	-o)		OUT=${2:?-o needs a path}; shift 2 ;;
	--kernel)	kv=${2:?--kernel needs NAME=PATH}
			KNAMES+=("${kv%%=*}"); KPATHS+=("${kv#*=}"); shift 2 ;;
	--entry)	[ $# -ge 2 ] || { echo "make-baremetal: --entry needs [NAME:]FLAGS" >&2; exit 2; }
			ENTRIES+=("$2"); BEFORES+=("$BEFORE"); BEFORE=""; shift 2 ;;
	--before)	BEFORE=${2:?--before needs a GRUB command}; shift 2 ;;
	--)		shift; FLAGS="$FLAGS $*"; break ;;
	*)		echo "usage: $0 [--bench-only] [-o OUT.iso] [--kernel NAME=PATH]... [--entry [NAME:]FLAGS]... [-- KERNEL-FLAGS...]" >&2; exit 2 ;;
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

# GRUB 2.14 makes every module's code read-only under UEFI firmware that offers
# the memory attribute protocol, and its x86 relocator then writes the
# registers it hands over into a stub in relocator.mod's code: there the image
# faults before the kernel starts (OVMF since edk2-stable202502; omen's
# firmware has no such protocol and boots it).  GRUB 2.16 moved the stubs to
# .data (62289192).  The module is asked, not the version, so a 2.14 that
# carries the fix is not warned about.
sec=$(objdump -t /usr/lib/grub/x86_64-efi/relocator.mod 2>/dev/null | awk '$NF == "grub_relocator32_eax" { print $(NF-2) }')
[ "$sec" != .text ] || echo "make-baremetal: warning: this GRUB's relocator writes into its own code, which GRUB makes read-only under UEFI firmware with the memory attribute protocol -- there the image faults before the kernel starts; BIOS and older UEFI firmware boot it (fixed in GRUB 2.16, 62289192)" >&2

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

# The bundle: nothing in it reaches a disk.
if [ "$BENCH_ONLY" = 1 ]; then
	conf=$TREE/bootstrap.conf
	printf 'name_server name_server\nipc_bench ipc_bench\n' > "$conf"
	"$MKBUNDLE" -o "$TREE/boot/bootstrap.bundle" "bootstrap.conf:$conf" \
		"name_server:$SBIN/name_server" "ipc_bench:$SBIN/ipc_bench" >/dev/null
	CONTENTS="name_server ipc_bench"
else
	ninja -C "$BUILD" bootstrap_bundle_nodisk >/dev/null || {
		echo "make-baremetal: the build could not make bootstrap-nodisk.bundle" >&2; exit 1; }
	cp "$BUILD/bootstrap-nodisk.bundle" "$TREE/boot/bootstrap.bundle"
	CONTENTS="bootstrap-nodisk ($(sed -n 's/^\([a-z_]*\) .*/\1/p' "$BUILD/src/servers/bootstrap/bootstrap-nodisk.conf" | tr '\n' ' ' | sed 's/ $//'))"
fi

cp "$KERNEL" "$TREE/boot/mach_kernel"
cp "$SBIN/bootstrap" "$TREE/boot/bootstrap"

# The other kernels, each under its own name.  A name is checked here, where a
# typo can still be said, and not on the machine, where it is GRUB's "file not
# found" on a screen that is gone before it can be read.
for i in "${!KNAMES[@]}"; do
	n=${KNAMES[$i]} k=${KPATHS[$i]}
	[[ $n =~ ^[A-Za-z0-9._-]+$ ]] && [ "$n" != mach_kernel ] && [ "$n" != bootstrap ] || {
		echo "make-baremetal: --kernel $n: a name is letters, digits, '.', '_' or '-', and not mach_kernel or bootstrap" >&2; exit 2; }
	[ -f "$k" ] || { echo "make-baremetal: --kernel $n: no file at $k" >&2; exit 1; }
	[ ! -e "$TREE/boot/$n" ] || { echo "make-baremetal: --kernel $n given twice" >&2; exit 2; }
	cp "$k" "$TREE/boot/$n"
done

# The entries: one per --entry, or the one the command line's flags make.
# "NAME:FLAGS" boots NAME's kernel, and a NAME no --kernel gave is refused
# rather than taken for flags.
[ -z "$BEFORE" ] || { echo "make-baremetal: --before with no --entry after it" >&2; exit 2; }
if [ ${#ENTRIES[@]} -eq 0 ]; then
	ENTRIES=("${FLAGS# -f}")
	BEFORES=("")
	TIMEOUT=5
else
	[ "$FLAGS" = " -f" ] || { echo "make-baremetal: flags after -- and --entry together: give each entry its flags" >&2; exit 2; }
	TIMEOUT=-1	# several cases on one stick: wait for the choice
fi
LIST=()

# No serial: on omen a serial terminal stalls GRUB.  all_video pulls in efi_gop
# under UEFI and vbe under BIOS, and gfxpayload asks for the panel's own mode,
# so the kernel is handed a framebuffer in either case.
{
	printf 'set timeout=%s\nset default=0\nterminal_input console\nterminal_output console\ninsmod all_video\nset gfxpayload=auto\n' "$TIMEOUT"
	for i in "${!ENTRIES[@]}"; do
		e=${ENTRIES[$i]} b=${BEFORES[$i]}
		k=mach_kernel f=$e
		if [[ $e =~ ^([A-Za-z0-9._-]+):(.*)$ ]]; then
			k=${BASH_REMATCH[1]} f=${BASH_REMATCH[2]}
			[ -f "$TREE/boot/$k" ] && [ "$k" != bootstrap ] || {
				echo "make-baremetal: --entry $e: no --kernel named $k" >&2; exit 2; }
		fi
		f=$(echo $f)	# one space between flags, none at the ends
		title="UrMach x86-64, bare metal: -f${f:+ $f}"
		[ "$k" = mach_kernel ] || title="$title, kernel $k"
		[ "$BENCH_ONLY" = 1 ] && title="$title, ipc_bench"
		[ -z "$b" ] || title="$title, after GRUB's $b"
		printf '\nmenuentry "%s" {\n' "$title"
		# A command of a module: loaded by name first, so the line does not
		# depend on GRUB finding the command in its list of modules.
		[ -z "$b" ] || printf '\tinsmod %s\n\t%s\n' "${b%% *}" "$b"
		printf '\tmultiboot2 /boot/%s -f%s\n\tmodule2 /boot/bootstrap bootstrap\n\tmodule2 /boot/bootstrap.bundle bootstrap.bundle\n\tboot\n}\n' \
			"$k" "${f:+ $f}"
		LIST+=("$title")
	done
} > "$TREE/boot/grub/grub.cfg" || exit $?

grub-mkrescue -o "$OUT" "$TREE" >/dev/null 2>"$TREE/mkrescue.log" || {
	echo "make-baremetal: grub-mkrescue failed:" >&2; cat "$TREE/mkrescue.log" >&2; exit 1; }

echo "make-baremetal: $OUT ($(du -h "$OUT" | cut -f1)), BIOS and UEFI, bundle: $CONTENTS"
for i in "${!LIST[@]}"; do
	echo "make-baremetal:   menu entry $((i + 1)): ${LIST[$i]}"
done
echo "make-baremetal: to put it on a USB stick (check the device first, this erases it):"
echo "    sudo dd if=$OUT of=/dev/sdX bs=4M conv=fsync status=progress"
