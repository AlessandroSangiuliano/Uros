#!/bin/sh
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# make-disk-x86_64.sh — the boot disk this target loads its late servers from.
#
# ⚠️ Not a port of make-disk-image.sh, for the reason the bundle rule in
# servers/bootstrap/CMakeLists.txt gives about make-bundle.sh: that script
# describes an i386 system -- three partitions, twenty servers, swap, benchmark
# data -- and checks for every one of them as a REQUIRED_FILE.  On this target
# none of that exists yet.  What exists is one partition with the servers that
# have crossed, so this is what that is.
#
# ── Why there is a disk at all, when everything fits in the bundle ────
#
# bootstrap loads a server from the first back end that answers: the multiboot
# bundle, then the block device server's boot partition, then a device opened on
# the kernel's master port.  The bundle is meant to be the MINIMUM needed to
# reach a disk, and everything after that is meant to come off the disk.
#
# 🔑 AND THE ARRANGEMENT IS THE SAME ON BOTH TARGETS, which is worth saying
# because the obvious guess is that it is not.  i386 does not reach its disk
# through the kernel either: its dev_name_list[] holds console, time, rtclock,
# a floppy, a tape and two network cards, and no disk at all.  It gets there
# through the same block device server, which loads ahci.so -- a userspace
# driver, the same shape as the virtio_blk.so this target loads.
#
# So the third back end reaches no disk on EITHER target.  What differs between
# them is only which driver the block device server loads, and that is the
# difference this file exists for: an image the virtio path can read.
#
# ── What is on it, and why nothing loads from it yet ─────────────────
#
# cow_test, which is also still in the bundle -- so bootstrap finds it there
# first and this copy is not the one that runs.
#
# It was the other way round for one commit.  Moving it out of the bundle is
# the right test of the path, because it changes NOTHING a run reports: the
# same program runs at the same point and prints the same lines, so the verdict
# stays the same number while the route changes completely, and a stage 2 that
# has stopped working cannot hide.  It loaded, and ran every one of its arms.
#
# ⚠️ But not reliably -- see #521 -- so it went back in the bundle.  The disk is
# still built and attached because the block device server then probes a real
# disk, finds a partition and publishes it on every boot, which is more than
# the nothing it had.  When #521 is answered, cow_test comes out of the bundle
# and this copy becomes the one that runs.

set -e

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD="${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}"
SBIN="$BUILD/export/uros/x86_64/user/sbin"
DISK="${UROS_X86_64_DISK:-$BUILD/disk-x86_64.img}"

# 32 MiB is far more than the one binary needs.  It is the smallest size at
# which mke2fs's defaults leave room without being asked to economise, and the
# file is sparse -- it costs what it holds.
IMG_MB=32
SECT=512
PART_START=2048

# What lives on the disk.  One name per line: the file in sbin, and the name it
# takes under /mach_servers.
DISK_SERVERS="cow_test"

for cmd in dd sfdisk mke2fs debugfs; do
	command -v "$cmd" >/dev/null 2>&1 || {
		echo "make-disk-x86_64: $cmd not found — install fdisk and e2fsprogs" >&2
		exit 1
	}
done

for s in $DISK_SERVERS; do
	[ -f "$SBIN/$s" ] || {
		echo "make-disk-x86_64: $SBIN/$s is missing — build first" >&2
		exit 1
	}
done

PART_SECTS=$(( IMG_MB * 1024 * 1024 / SECT - PART_START ))
PART_IMG=$(mktemp /tmp/uros-x86_64-part.XXXXXX.img)
trap 'rm -f "$PART_IMG"' EXIT

# The partition's filesystem, built on its own and then placed.  Assembling it
# in place would mean teaching this script the offset arithmetic that sfdisk
# already knows.
dd if=/dev/zero of="$PART_IMG" bs="$SECT" count="$PART_SECTS" status=none

# ⚠️ -b 4096 and -I 256 match what the i386 image uses, and they are not
# cosmetic: ext2fs.c reads the block size and the inode size off the
# superblock, and a disk written with different ones is the cheapest way to
# find out whether it really does.
mke2fs -t ext2 -q -F -b 4096 -I 256 -r 1 -L "mach_servers" -O filetype \
	"$PART_IMG"

{
	echo "mkdir /mach_servers"
	for s in $DISK_SERVERS; do
		echo "cd /mach_servers"
		echo "write $SBIN/$s $s"
	done
} | debugfs -w -f /dev/stdin "$PART_IMG" >/dev/null 2>&1

dd if=/dev/zero of="$DISK" bs=1M count="$IMG_MB" status=none
sfdisk --quiet "$DISK" <<EOF
label: dos
start=$PART_START, size=$PART_SECTS, type=83
EOF

dd if="$PART_IMG" of="$DISK" bs="$SECT" seek="$PART_START" conv=notrunc status=none

echo "make-disk-x86_64: $DISK — one ext2 partition at LBA $PART_START holding:"
for s in $DISK_SERVERS; do
	echo "                  /mach_servers/$s"
done
