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
# bundle, then the block device server's boot partition, then -- on i386 -- the
# kernel's own IDE driver.  The bundle is meant to be the MINIMUM needed to
# reach a disk, and everything after that is meant to come off the disk.
#
# 🔴 THERE IS NO THIRD BACK END HERE, AND THERE SHOULD NEVER BE ONE.  This
# kernel has no in-kernel disk driver and the project's direction is that it
# will not get one.  So on x86-64 the sequence is genuinely two stages rather
# than three: the bundle for what must exist before any driver runs, and the
# block device server for everything after.  That is not a reduction of i386's
# arrangement -- it is that arrangement without the escape hatch that made the
# microkernel claim untrue.
#
# ── What is on it, and why that one ──────────────────────────────────
#
# cow_test, which was the last entry of the bundle and is now the last entry of
# the disk.  It is chosen because moving it changes NOTHING that a run reports:
# the same program runs at the same point and prints the same lines, so the
# self-test count is identical.
#
# 🔑 Which is exactly what makes it a test of the path rather than of itself.
# The verdict stays the same number while the route changes completely, so a
# stage-2 that has stopped working cannot hide -- cow_test simply does not run,
# and the count falls by everything it prints.

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
