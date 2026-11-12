#!/usr/bin/env bash
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# boot-log-disk-x86_64.sh -- the bundle for a real machine that keeps its boot
# log on a disk (#915), booted in QEMU on three AHCI disks laid out as
# lapsus's: one with two NTFS partitions, which the block server does not
# publish; one with an ext4 and a swap, as the Linux that runs there; and one
# with an ext2 labelled UROS_LOG_LABEL, made as lapsus's was (filetype alone).
# The image is make-baremetal-x86_64.sh --log's with -b, so the machine
# restarts after the boot's final census and qemu leaves (-no-reboot).
#
# The verdict is the boot's and the disks':
#   - blk_write_label_test's count, all three arms;
#   - the NTFS and the Linux disk byte for byte what they were before the boot,
#     an oracle no part of the system under test computes;
#   - the log partition read back as Linux would read it: e2fsck -fn finds no
#     error, and /log/boot-1.txt -- the partition's /boot-1.txt, as it is
#     mounted at /log -- is there with bootlog's head and the boot's own lines
#     in it (blk_write_label_test's count).
#
# Usage:  scripts/boot-log-disk-x86_64.sh [-o DIR] [--kvm] [-smp N]
#             [--ports A,B,C] [--boots N] [BUDGET]
#   -o DIR   where the image, the disks and the log go (a NEW directory;
#            default: the build's log-disk/<date-time>)
#   --ports  the AHCI ports of the three disks, in order (default 0,1,2);
#            lapsus has them on 0, 3 and 5, with the ports between empty
#   --boots  boots one after another on the same disks (default 1): the Nth
#            must leave /log/boot-N.txt and every earlier file as it was
#   --kvm    under KVM; the default is TCG
#   -smp N   processors (default 4)
#   BUDGET   seconds the boot may take before -b restarts it (default 600)
#
# The build is UROS_BUILD_DIR when set, else uros/build-x86_64.
#
# Exit status: 0 passed · 1 a WRONG, a disk written that must not be, or no
# verdict · 2 the run could not be made: the command line, a tool or a file
# (ablate.sh reads 2 so).

set -u

REPO=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${UROS_BUILD_DIR:-$REPO/uros/build-x86_64}
OUT=""
KVM=0
SMP=4
BUDGET=600
PORTS=0,1,2
BOOTS=1

while [ $# -gt 0 ]; do
	case "$1" in
	-o)	OUT=${2:?-o needs a directory}; shift 2 ;;
	--kvm)	KVM=1; shift ;;
	-smp)	SMP=${2:?-smp needs a number}; shift 2 ;;
	--ports) PORTS=${2:?--ports needs three port numbers}; shift 2 ;;
	--boots) BOOTS=${2:?--boots needs a number}; shift 2 ;;
	[0-9]*)	BUDGET=$1; shift ;;
	*)	echo "usage: $0 [-o DIR] [--kvm] [-smp N] [--ports A,B,C] [--boots N] [BUDGET]" >&2; exit 2 ;;
	esac
done
IFS=, read -r P0 P1 P2 <<< "$PORTS"
for p in "$P0" "$P1" "$P2"; do
	case $p in
	[0-5]) ;;
	*)	echo "boot-log-disk: --ports wants three of ich9-ahci's ports 0..5, got $PORTS" >&2; exit 2 ;;
	esac
done
[ "$P0" != "$P1" ] && [ "$P0" != "$P2" ] && [ "$P1" != "$P2" ] ||
	{ echo "boot-log-disk: --ports names one port twice: $PORTS" >&2; exit 2; }

for cmd in qemu-system-x86_64 sfdisk mke2fs e2fsck debugfs truncate dd md5sum xorriso; do
	command -v "$cmd" >/dev/null || { echo "boot-log-disk: $cmd is missing" >&2; exit 2; }
done
[ -n "$OUT" ] || OUT=$BUILD/log-disk/$(date +%Y%m%d-%H%M%S)
[ ! -e "$OUT" ] || { echo "boot-log-disk: $OUT already exists: give a new directory" >&2; exit 2; }
mkdir -p "$OUT" || exit 2

LABEL=$(sed -n 's/^UROS_LOG_LABEL:STRING=//p' "$BUILD/CMakeCache.txt" 2>/dev/null)
[ -n "$LABEL" ] || { echo "boot-log-disk: no UROS_LOG_LABEL in $BUILD/CMakeCache.txt" >&2; exit 2; }

# The image: the log bundle, -b with the budget as its deadline.
UROS_BUILD_DIR=$BUILD "$REPO/scripts/make-baremetal-x86_64.sh" --log \
	-o "$OUT/log.iso" --entry "-b reset_after=$BUDGET" > "$OUT/iso.txt" 2>&1 || {
	echo "boot-log-disk: the image could not be made, see $OUT/iso.txt" >&2; exit 2; }

SECT=512
START=2048
# part IMAGE SECTORS MKFS-ARGS... -- a partition's filesystem in a file of its own.
part() {
	local img=$1 sects=$2
	shift 2
	truncate -s $(( sects * SECT )) "$img" && mke2fs -q -F "$@" "$img"
}

# Disk 0, Windows's: two NTFS entries over zeros, which the block server does
# not publish (it publishes 0x83 and 0x82 alone).
truncate -s 64M "$OUT/disk0-ntfs.img"
sfdisk --quiet "$OUT/disk0-ntfs.img" <<EOF || exit 2
label: dos
start=$START, size=40960, type=7
type=7
EOF

# Disk 1, the Linux that runs there: an ext4 root and a swap.
truncate -s 128M "$OUT/disk1-linux.img"
sfdisk --quiet "$OUT/disk1-linux.img" <<EOF || exit 2
label: dos
start=$START, size=196608, type=83
type=82
EOF
part "$OUT/root.part" 196608 -t ext4 -L linuxroot || exit 2
dd if="$OUT/root.part" of="$OUT/disk1-linux.img" bs=$SECT seek=$START conv=notrunc status=none

# Disk 2, the one given to Uros: the log partition, as lapsus's was made.
truncate -s 64M "$OUT/disk2-log.img"
sfdisk --quiet "$OUT/disk2-log.img" <<EOF || exit 2
label: dos
start=$START, type=83
EOF
part "$OUT/log.part" $(( 64 * 2048 - START )) -t ext2 -b 4096 -I 256 -r 1 \
	-O none,filetype -L "$LABEL" || exit 2
dd if="$OUT/log.part" of="$OUT/disk2-log.img" bs=$SECT seek=$START conv=notrunc status=none
rm -f "$OUT/root.part" "$OUT/log.part"

md5sum "$OUT"/disk*.img > "$OUT/md5-before.txt"

ACCEL="-accel tcg"
[ "$KVM" = 1 ] && ACCEL="-enable-kvm"
rc=0
say() { echo "boot-log-disk: $*" | tee -a "$OUT/verdict.txt"; }

# The boots, one after another on the same disks: the Nth leaves the Nth file,
# and every file an earlier boot left is byte for byte what it was (#915).
b=1
while [ $b -le "$BOOTS" ]; do
	LOG=$OUT/serial-$b.log
	# shellcheck disable=SC2086
	qemu-system-x86_64 -cpu max $ACCEL -smp "$SMP" -m 1G \
		-cdrom "$OUT/log.iso" -boot d \
		-device ich9-ahci,id=ahci0 \
		-drive if=none,id=d0,file="$OUT/disk0-ntfs.img",format=raw \
		-device ide-hd,drive=d0,bus=ahci0.$P0 \
		-drive if=none,id=d1,file="$OUT/disk1-linux.img",format=raw \
		-device ide-hd,drive=d1,bus=ahci0.$P1 \
		-drive if=none,id=d2,file="$OUT/disk2-log.img",format=raw \
		-device ide-hd,drive=d2,bus=ahci0.$P2 \
		-display none -monitor none -serial file:"$LOG" -no-reboot \
		> "$OUT/qemu-$b.out" 2>&1 &
	QPID=$!
	# -b restarts the machine after the census, or at BUDGET; a minute more for
	# the restart, and then qemu is stopped as a run that did not end.
	t0=$(date +%s)
	while kill -0 "$QPID" 2>/dev/null; do
		if [ $(( $(date +%s) - t0 )) -gt $(( BUDGET + 60 )) ]; then
			kill "$QPID" 2>/dev/null
			say "boot $b: qemu still running after $(( BUDGET + 60 )) s: stopped"
			break
		fi
		sleep 1
	done
	wait "$QPID" 2>/dev/null
	say "boot $b took $(( $(date +%s) - t0 )) s ($([ "$KVM" = 1 ] && echo KVM || echo TCG), -smp $SMP, disks on AHCI ports $PORTS)"

	grep -a -E '^blk: (-W|  [a-z_0-9]+ \()|^blk_write_label_test: ' "$LOG" | tee -a "$OUT/verdict.txt"
	if grep -aq '^blk_write_label_test: 3 of 3 arms passed' "$LOG"; then
		say "boot $b: blk_write_label_test 3 of 3 arms passed"
	else
		say "WRONG -- boot $b: blk_write_label_test did not pass all three arms (or said nothing)"
		rc=1
	fi

	# The log partition as the boot left it, read as Linux would: e2fsck, and
	# the file bootlog made.  The partition is mounted AT /log, so bootlog's
	# /log/boot-N.txt is the partition's /boot-N.txt.
	dd if="$OUT/disk2-log.img" of="$OUT/log-after-$b" bs=$SECT skip=$START status=none
	e2fsck -fn "$OUT/log-after-$b" > "$OUT/e2fsck-$b.txt" 2>&1
	erc=$?
	if [ $erc -lt 4 ]; then
		say "boot $b: e2fsck -fn on the log partition: exit $erc, no error"
	else
		say "WRONG -- boot $b: e2fsck -fn on the log partition: exit $erc, see $OUT/e2fsck-$b.txt"
		rc=1
	fi
	debugfs -R 'ls -l /' "$OUT/log-after-$b" > "$OUT/log-ls-$b.txt" 2>/dev/null
	debugfs -R "cat /boot-$b.txt" "$OUT/log-after-$b" > "$OUT/boot-$b.txt" 2>/dev/null
	if grep -aq "^bootlog: boot $b, " "$OUT/boot-$b.txt" &&
	   grep -aq '^blk_write_label_test: 3 of 3 arms passed' "$OUT/boot-$b.txt"; then
		say "boot $b: /log/boot-$b.txt is on the disk, $(wc -c < "$OUT/boot-$b.txt") bytes, $(wc -l < "$OUT/boot-$b.txt") lines, with bootlog's head and blk_write_label_test's count"
	else
		say "WRONG -- boot $b: no /log/boot-$b.txt with bootlog's head and blk_write_label_test's count ($(wc -c < "$OUT/boot-$b.txt") bytes read)"
		rc=1
	fi
	k=1
	while [ $k -lt $b ]; do
		debugfs -R "cat /boot-$k.txt" "$OUT/log-after-$b" > "$OUT/boot-$k-after-$b.txt" 2>/dev/null
		if cmp -s "$OUT/boot-$k.txt" "$OUT/boot-$k-after-$b.txt"; then
			say "boot $b: /log/boot-$k.txt byte for byte as boot $k left it"
		else
			say "WRONG -- boot $b: /log/boot-$k.txt is not as boot $k left it"
			rc=1
		fi
		k=$(( k + 1 ))
	done
	b=$(( b + 1 ))
done

md5sum "$OUT"/disk*.img > "$OUT/md5-after.txt"
for d in disk0-ntfs disk1-linux; do
	if [ "$(grep "/$d.img" "$OUT/md5-before.txt" | cut -d' ' -f1)" = \
	     "$(grep "/$d.img" "$OUT/md5-after.txt" | cut -d' ' -f1)" ]; then
		say "$d.img byte for byte as before the first boot"
	else
		say "WRONG -- $d.img was written"
		rc=1
	fi
done
if [ "$(grep '/disk2-log.img' "$OUT/md5-before.txt" | cut -d' ' -f1)" = \
     "$(grep '/disk2-log.img' "$OUT/md5-after.txt" | cut -d' ' -f1)" ]; then
	say "disk2-log.img unchanged"
else
	say "disk2-log.img written"
fi
say "$([ $rc = 0 ] && echo PASSED || echo WRONG), in $OUT"
exit $rc
