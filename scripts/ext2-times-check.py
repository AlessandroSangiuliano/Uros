#!/usr/bin/env python3
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ext2-times-check.py -- what a boot left in an ext2 superblock's times (#290).
#
# ext2fs.c sets the mount time and the write time from the battery clock: both
# when it marks the volume in use at the mount, the write time again when it
# marks it clean at the unmount.  This reads them back from the image, with no
# code of the writer's: the superblock is 1024 bytes into the first MBR
# partition, s_mtime at 44, s_wtime at 48 and s_state at 58, little-endian,
# and e2fsck reads the copy too.
#
#   scripts/ext2-times-check.py IMAGE START END --dirty|--clean
#
# START and END are the host's clock, in seconds since the epoch, just before
# QEMU started and after it ended -- after the images were made, so that
# mke2fs's own write time falls before the boot.  The guest's clock is QEMU's
# RTC, which starts from the host's in UTC and follows it, so a time the boot
# wrote lies between them; two seconds on each side are the grain of the two
# clocks, much less than the minute the issue allows.  --dirty is a boot that
# ended without unmounting, which #284 leaves in use; --clean is one that
# unmounted.
#
# Asks:
#   [1] the mount time is within the boot;
#   [2] the write time is within the boot, and not before the mount time;
#   [3] the state is what the end of the boot leaves (#284);
#   [4] e2fsck -fn finds no time in the future.
#
# ⚠️ The partition is COPIED out before anything reads it, as xfile-verify.py
# does: the image is the evidence.
#
# Prints one line per question and a summary; exits 0 every one passed, 1 one
# failed, 2 could not check.

import os
import struct
import subprocess
import sys
import tempfile
import time

SECTOR = 512
MARGIN = 2
EXT2_SUPER_MAGIC = 0xEF53
EXT2_VALID_FS = 0x0001


def first_partition(image):
    with open(image, "rb") as f:
        mbr = f.read(SECTOR)
    if len(mbr) != SECTOR or mbr[510:512] != b"\x55\xaa":
        return None
    start, count = struct.unpack_from("<II", mbr, 446 + 8)
    if count == 0:
        return None
    return start, count


def utc(t):
    return time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(t))


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    want = [a for a in argv[1:] if a in ("--dirty", "--clean")]
    if len(args) != 3 or len(want) != 1:
        print("usage: ext2-times-check.py IMAGE START END --dirty|--clean",
              file=sys.stderr)
        return 2
    image = args[0]
    try:
        start, end = int(args[1]), int(args[2])
    except ValueError:
        print("ext2-times: START and END are seconds since the epoch",
              file=sys.stderr)
        return 2
    clean = want[0] == "--clean"

    part = first_partition(image)
    if part is None:
        print(f"ext2-times: {image} has no MBR partition to check")
        return 2
    pstart, pcount = part

    failed = 0
    with tempfile.TemporaryDirectory(prefix="ext2-times-") as tmp:
        fs = os.path.join(tmp, "part.img")
        with open(image, "rb") as src, open(fs, "wb") as dst:
            src.seek(pstart * SECTOR)
            left = pcount * SECTOR
            while left > 0:
                chunk = src.read(min(left, 1 << 20))
                if not chunk:
                    break
                dst.write(chunk)
                left -= len(chunk)
        with open(fs, "rb") as f:
            f.seek(1024)
            sb = f.read(1024)
        if len(sb) != 1024:
            print("ext2-times: the partition is too short for a superblock")
            return 2
        mtime, wtime = struct.unpack_from("<II", sb, 44)
        magic, state = struct.unpack_from("<HH", sb, 56)
        if magic != EXT2_SUPER_MAGIC:
            print(f"ext2-times: no ext2 superblock (magic 0x{magic:04x})")
            return 2

        lo, hi = start - MARGIN, end + MARGIN
        window = f"the boot ran from {utc(start)} to {utc(end)}"

        if lo <= mtime <= hi:
            print(f"ext2-times: [1] the mount time is {utc(mtime)}, within "
                  f"the boot -- set by the mount (#290)")
        else:
            failed += 1
            said = "never set" if mtime == 0 else utc(mtime)
            print(f"ext2-times: [1] WRONG — the mount time is {said}, and "
                  f"{window} (#290)")

        if lo <= wtime <= hi and wtime >= mtime:
            print(f"ext2-times: [2] the write time is {utc(wtime)}, within "
                  f"the boot and not before the mount (#290)")
        else:
            failed += 1
            print(f"ext2-times: [2] WRONG — the write time is {utc(wtime)} "
                  f"and the mount time {utc(mtime)}, and {window} (#290)")

        is_clean = (state & EXT2_VALID_FS) != 0
        if is_clean == clean:
            print(f"ext2-times: [3] the volume is "
                  f"{'clean' if clean else 'in use'} (state 0x{state:04x}), "
                  f"as a boot that {'unmounted' if clean else 'did not unmount'} "
                  f"leaves it (#284)")
        else:
            failed += 1
            print(f"ext2-times: [3] WRONG — the volume is "
                  f"{'clean' if is_clean else 'in use'} (state 0x{state:04x}), "
                  f"after a boot that "
                  f"{'unmounted' if clean else 'did not unmount'} (#284)")

        # In English whatever the host speaks: the words are what is looked for.
        fsck = subprocess.run(["e2fsck", "-fn", fs], capture_output=True,
                              text=True, env=dict(os.environ, LC_ALL="C"))
        said = fsck.stdout + fsck.stderr
        future = [l for l in said.splitlines() if "in the future" in l]
        if not future:
            print("ext2-times: [4] e2fsck -fn finds no time in the future")
        else:
            failed += 1
            print(f"ext2-times: [4] WRONG — e2fsck -fn: {future[0].strip()}")

    print(f"ext2-times: {4 - failed} of 4 checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
