#!/bin/sh
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# make-disk-x86_64.sh — the whole system on one disk, GRUB included.
#
# ── What this replaced, and why the premise had to change ────────────
#
# This script used to build a data disk that sat next to a GRUB rescue ISO:
# the ISO carried the kernel and the bundle, the disk carried one test binary,
# and a run needed both.  That arrangement was described as the same one i386
# uses.  It is not, and the difference is the reason this file was rewritten.
#
# 🔴 i386's disk.img HAS NO BOOTLOADER AT ALL.  Its first 440 bytes are zero,
# the 62 sectors before the first partition are zero, no partition is marked
# active, and there is no /boot on any of its three partitions.  What loads the
# i386 kernel is QEMU's own multiboot loader, reached through -kernel and a
# comma-separated -initrd; the disk is attached purely as data, so that stage 2
# has somewhere to read servers from.  GRUB appears in the i386 world only in
# make-omen-boot.sh, which builds an ISO for real hardware and is DISKLESS --
# everything it boots is in RAM.
#
# So the arrangement the user described -- GRUB loads the kernel and the bundle
# from the disk like an initrd, the bundle starts the main servers including
# the block device server, and that server then loads the rest of the system
# from the same disk -- existed on NEITHER target.  Three harnesses, and each
# of them had something else doing half the job: QEMU's loader on i386, a CD on
# x86-64, RAM on the bare-metal ISO.
#
# This is that arrangement, built for the first time.  One disk, one partition,
# no CD, nothing handed to QEMU but the drive.
#
# ── One partition, two readers ───────────────────────────────────────
#
# /boot/...        is read by GRUB, in real mode, through the BIOS
# /mach_servers/.. is read by the block device server, in a task, through
#                  virtio_blk.so
#
# The same ext2, read twice by two pieces of software that share no code and
# do not know about each other.  That is deliberate and it is the point: a
# filesystem this kernel's ext_server writes is one an independent reader can
# still make sense of, and the boot proves it before the kernel is even loaded.
#
# ── Why the GRUB modules live INSIDE core.img ────────────────────────
#
# A normal grub-install puts a 160 KB core image in the gap and 8 MB of .mod
# files under /boot/grub/i386-pc, and the core loads what it needs from there.
# This disk has no /boot/grub/i386-pc.  Every module the boot can ever use is
# embedded in the core image, which is why the module list below is generous
# rather than minimal: what is not in the core does not exist.
#
# ⚠️ That cuts both ways -- there is no recovery path either.  At a GRUB rescue
# prompt on this disk you cannot insmod your way out, so ls, cat, echo and test
# are in the list for the sake of the person standing at that prompt, not
# because grub.cfg uses them.
#
# The gap between the MBR and LBA 2048 is 2047 sectors, just under a megabyte;
# the core image is about 314 of them.  There is no pressure to economise.
#
# ── Why grub-bios-setup is not used ──────────────────────────────────
#
# 🔴 grub-bios-setup RESOLVES AN IMAGE FILE TO THE BLOCK DEVICE THAT CONTAINS
# IT.  Pointed at this disk image it announced "the disk does not exist, using
# the partition device /dev/nvme0n1p2" -- the developer's own root filesystem --
# and only failed because it could not then open it as a GRUB device.  A tool
# whose failure mode is writing a boot sector onto the machine you are working
# on does not belong in a build script.
#
# 🔑 And it turns out nothing it does is needed here.  Its job is to make the
# boot sector and the core image agree about where the core image is, and
# grub-mkimage has already written that agreement for the default placement:
# boot.img carries kernel_sector = 1 at offset 0x5c, and the first sector of
# core.img carries a blocklist at 0x1f4 saying "the rest starts at LBA 2, for
# n-1 sectors, at segment 0x820".  Those were read out of the two files, not
# assumed.  Put the core at LBA 1 and both statements are already true.
#
# So the install is two splices and no patching:
#
#   boot.img[0..439] -> LBA 0      the first 440 bytes ONLY, because everything
#                                  from 0x1b8 on is the partition table sfdisk
#                                  just wrote, and boot.img's own bytes there
#                                  are the floppy-fallback path, which this
#                                  disk will never take
#   core.img         -> LBA 1      contiguous, which is what the blocklist says
#
# ── Root ─────────────────────────────────────────────────────────────
#
# None of this needs it.  No mount, no losetup, no sudo: every file goes in
# through debugfs and every raw byte through dd with conv=notrunc.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}"
SBIN="$BUILD/export/uros/x86_64/user/sbin"
BOOTDIR="$BUILD/export/uros/boot"
DISK="${UROS_X86_64_DISK:-$BUILD/disk-x86_64.img}"
GRUB_CFG="$REPO/uros/src/mach_kernel/x86_64/boot/grub.cfg"
GRUB_LIB="${UROS_GRUB_I386_PC:-/usr/lib/grub/i386-pc}"

# 64 MiB.  What is on it today is about 8, and the rest is the room the servers
# still in the bundle will need as they move out of it.  The file is sparse --
# it costs what it holds, not what it says.
IMG_MB=64
SECT=512
PART_START=2048
PART_LABEL=mach_servers

# Which GRUB menu entry boots, read from the environment the way the ISO rule
# in mach_kernel/CMakeLists.txt reads it, so --entry N keeps working unchanged.
BOOT_ENTRY="${UROS_X86_64_BOOT_ENTRY:-0}"

# What GRUB is given.  grub-mkimage resolves each name's dependencies from
# moddep.lst, so this is the list of things the boot NAMES, not the closure.
#
#   biosdisk part_msdos ext2   reach this disk and read this filesystem
#   normal configfile          the menu, and grub.cfg's `source'
#   multiboot2                 both `multiboot2' and `module2'
#   boot serial terminal       the rest of what grub.cfg actually says
#   echo test ls cat halt      for the person at a rescue prompt
#   search*                    NOT used by the boot -- see the prefix comment
#                              below -- but kept for the rescue prompt, where
#                              finding a partition by label is the first thing
#                              anyone needs and no module can be loaded
GRUB_MODULES="biosdisk part_msdos ext2 normal configfile multiboot2 boot
	serial terminal search search_label search_fs_uuid search_fs_file
	echo test ls cat halt reboot"

# What lives under /boot, read by GRUB.  The kernel and the bundle: the bundle
# is this system's initrd, and it holds the servers needed to reach a disk.
BOOT_FILES="$BOOTDIR/mach_kernel
	$SBIN/bootstrap
	$SBIN/name_server
	$SBIN/boot_probe
	$BUILD/bootstrap.bundle"

# What lives under /mach_servers, read by the block device server once the
# bundle has started it.  This is the half of the disk the kernel reads.
DISK_SERVERS="cow_test"

for cmd in dd truncate sfdisk mke2fs debugfs grub-mkimage; do
	command -v "$cmd" >/dev/null 2>&1 || {
		echo "make-disk-x86_64: $cmd not found — install fdisk, e2fsprogs and grub" >&2
		exit 1
	}
done

[ -f "$GRUB_LIB/boot.img" ] || {
	echo "make-disk-x86_64: $GRUB_LIB/boot.img is missing — set UROS_GRUB_I386_PC" >&2
	exit 1
}

for f in $BOOT_FILES; do
	[ -f "$f" ] || {
		echo "make-disk-x86_64: $f is missing — build first" >&2
		exit 1
	}
done

for s in $DISK_SERVERS; do
	[ -f "$SBIN/$s" ] || {
		echo "make-disk-x86_64: $SBIN/$s is missing — build first" >&2
		exit 1
	}
done

[ -f "$GRUB_CFG" ] || {
	echo "make-disk-x86_64: $GRUB_CFG is missing" >&2
	exit 1
}

PART_SECTS=$(( IMG_MB * 1024 * 1024 / SECT - PART_START ))

# ⚠️ Not under /tmp.  grub-mkimage is fine there, but /tmp is tmpfs on this
# machine and the whole point of the working directory is that the raw splices
# below and the image agree about what a byte offset means.
WORK=$(mktemp -d "$BUILD/.disk-x86_64.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

PART_IMG="$WORK/part.img"

# The partition's filesystem, built on its own and then placed.  Assembling it
# in place would mean teaching this script the offset arithmetic that sfdisk
# already knows.
truncate -s $(( PART_SECTS * SECT )) "$PART_IMG"

# ⚠️ -b 4096 and -I 256 match what the i386 image uses, and they are not
# cosmetic: ext2fs.c reads the block size and the inode size off the
# superblock, and a disk written with different ones is the cheapest way to
# find out whether it really does.  GRUB reads the same superblock, so a
# mistake here is now caught before the kernel is even loaded.
mke2fs -t ext2 -q -F -b 4096 -I 256 -r 1 -L "$PART_LABEL" -O filetype \
	"$PART_IMG"

echo "set default=$BOOT_ENTRY" > "$WORK/entry.cfg"

{
	echo "mkdir /boot"
	echo "mkdir /boot/grub"
	echo "cd /boot"
	for f in $BOOT_FILES; do
		echo "write $f $(basename "$f")"
	done
	echo "cd /boot/grub"
	echo "write $GRUB_CFG grub.cfg"
	echo "write $WORK/entry.cfg entry.cfg"
	echo "mkdir /mach_servers"
	echo "cd /mach_servers"
	for s in $DISK_SERVERS; do
		echo "write $SBIN/$s $s"
	done
} | debugfs -w -f /dev/stdin "$PART_IMG" >/dev/null 2>&1

# ── Where GRUB looks for grub.cfg, and why there is nothing else here ──
#
# The prefix is compiled into the core image and that is the whole mechanism:
# `(hd0,msdos1)/boot/grub'.  hd0 is not a guess -- GRUB's biosdisk numbers the
# BIOS drives and hd0 is the one whose boot sector the firmware just ran, which
# is this disk by construction, since this disk is the only thing QEMU is given
# and on real hardware it is whatever the boot order picked.  One disk, one
# partition, and the partition number is fixed by the sfdisk call below.
#
# 🔴 THERE IS NO `search' AND NO FALLBACK, AND THAT IS A LIMIT OF THE PARSER,
# not a choice.  Three forms were written here before this one, each with a
# comment explaining why it worked, and all three were refuted by running them:
#
#	search --set=root --label L	a failed search CLEARS root, so the
#	set prefix=($root)/boot/grub	next line builds `()/boot/grub'
#
#	search --set=uros_root ...	a failed search raises a GRUB error
#	if [ -n "$uros_root" ] ...	and the config stops at that line
#
#	if search --set=uros_root ...	`Unknown command `if'.'
#
# 🔑 The last one names the reason for all three.  The embedded config runs
# BEFORE `normal' is loaded, and `normal' is what provides the script engine --
# `if', `fi', `test', command status.  What runs it is the rescue parser, which
# executes simple commands and nothing else.  A conditional fallback is not
# expressible in this file, so the honest thing is to not pretend to have one.
#
# ⚠️ Found by accident, which is the part worth keeping: not by testing the
# fallback, but by a disk damaged for an unrelated check that stopped
# satisfying the search as a side effect.  Three commits' worth of reasoning
# about a code path that had never once been executed.
cat > "$WORK/embedded.cfg" <<EOF
set root=(hd0,msdos1)
EOF

grub-mkimage -O i386-pc -d "$GRUB_LIB" \
	-p "(hd0,msdos1)/boot/grub" \
	-c "$WORK/embedded.cfg" \
	-o "$WORK/core.img" \
	$GRUB_MODULES

CORE_SECTS=$(( ( $(stat -c%s "$WORK/core.img") + SECT - 1 ) / SECT ))
if [ "$CORE_SECTS" -ge "$PART_START" ]; then
	echo "make-disk-x86_64: core.img is $CORE_SECTS sectors and the gap before" \
	     "LBA $PART_START holds $(( PART_START - 1 ))" >&2
	exit 1
fi

rm -f "$DISK"
truncate -s "${IMG_MB}M" "$DISK"
sfdisk --quiet "$DISK" <<EOF
label: dos
start=$PART_START, size=$PART_SECTS, type=83, bootable
EOF

# 440 and not 512: from 0x1b8 on, the sector belongs to sfdisk.
dd if="$GRUB_LIB/boot.img" of="$DISK" bs=1 count=440 conv=notrunc status=none
dd if="$WORK/core.img" of="$DISK" bs="$SECT" seek=1 conv=notrunc status=none
dd if="$PART_IMG" of="$DISK" bs="$SECT" seek="$PART_START" conv=notrunc status=none

echo "make-disk-x86_64: $DISK — bootable, grub core at LBA 1 ($CORE_SECTS sectors),"
echo "                  one ext2 partition at LBA $PART_START (label $PART_LABEL) holding:"
for f in $BOOT_FILES; do
	echo "                  /boot/$(basename "$f")"
done
echo "                  /boot/grub/grub.cfg, /boot/grub/entry.cfg (default=$BOOT_ENTRY)"
for s in $DISK_SERVERS; do
	echo "                  /mach_servers/$s"
done
