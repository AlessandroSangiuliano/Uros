#!/usr/bin/env python3
# Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# xfile-verify.py -- the THIRD reader of xfile_test's file (#498).
#
# xfile_test writes a file on one target and reads it on the other.  Both of
# those readers are Uros: the same ext2 code, compiled twice.  This one shares
# nothing with either -- e2fsck and debugfs read the disk, and the pattern is
# recomputed here from its definition -- so a file that passes here and fails
# on a target is that target's reader, and one that fails here is the writer.
#
#   scripts/xfile-verify.py IMAGE PATH [--writer i386|x86_64]
#
# IMAGE is a whole disk with an MBR; its first partition is checked.  PATH is
# the file inside that filesystem, e.g. /from_x86_64.dat for the file a boot
# wrote to /mnt/disk2/from_x86_64.dat.
#
# ⚠️ The partition is COPIED out before anything reads it.  e2fsck -n does not
# write, but a tool pointed at a disk image is one flag away from writing to
# it, and the image is the evidence.
#
# Exit status: 0 every check passed, 1 a check failed, 2 could not check.

import os
import struct
import subprocess
import sys
import tempfile

XF_SIZE = 62674
XF_HDR = 32
XF_NAME_OFF = 16
XF_NAME_LEN = 16
XF_MIX = 0x9E3779B97F4A7C15
XF_MAGIC = b"UROSXFT1"
SECTOR = 512


def xf_byte(off):
    # The same definition as xfile_test.c: every 8-byte word carries its own
    # index, multiplied by an odd constant modulo 2^64.
    v = ((off >> 3) * XF_MIX) & 0xFFFFFFFFFFFFFFFF
    return (v >> ((off & 7) * 8)) & 0xFF


def first_partition(image):
    with open(image, "rb") as f:
        mbr = f.read(SECTOR)
    if len(mbr) != SECTOR or mbr[510:512] != b"\x55\xaa":
        return None
    start, count = struct.unpack_from("<II", mbr, 446 + 8)
    if count == 0:
        return None
    return start, count


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    writer = None
    for i, a in enumerate(argv):
        if a == "--writer" and i + 1 < len(argv):
            writer = argv[i + 1]
            args = [x for x in args if x != writer]
    if len(args) != 2:
        print("usage: xfile-verify.py IMAGE PATH [--writer i386|x86_64]",
              file=sys.stderr)
        return 2
    image, path = args

    part = first_partition(image)
    if part is None:
        print(f"xfile-verify: {image} has no MBR partition to check")
        return 2
    start, count = part

    failed = 0
    with tempfile.TemporaryDirectory(prefix="xfile-verify-") as tmp:
        fs = os.path.join(tmp, "part.img")
        out = os.path.join(tmp, "file.dat")
        with open(image, "rb") as src, open(fs, "wb") as dst:
            src.seek(start * SECTOR)
            dst.write(src.read(count * SECTOR))

        # e2fsck: is the filesystem the writer left one an independent
        # implementation agrees with?  Exit 0 is clean; anything else is a
        # finding about the writer's METADATA, which the byte check below
        # cannot see.
        fsck = subprocess.run(["e2fsck", "-fn", fs], capture_output=True,
                              text=True)
        if fsck.returncode == 0:
            print("xfile-verify: [1] e2fsck -fn finds the filesystem clean")
        else:
            failed += 1
            print(f"xfile-verify: [1] WRONG — e2fsck -fn exits "
                  f"{fsck.returncode}:")
            for line in (fsck.stdout + fsck.stderr).splitlines():
                print(f"    {line}")

        dump = subprocess.run(["debugfs", "-R", f"dump -p {path} {out}", fs],
                              capture_output=True, text=True)
        if not os.path.exists(out) or dump.returncode != 0:
            print(f"xfile-verify: [2] WRONG — debugfs could not dump {path}: "
                  f"{dump.stderr.strip()}")
            return 1
        with open(out, "rb") as f:
            data = f.read()

    if len(data) != XF_SIZE:
        failed += 1
        print(f"xfile-verify: [2] WRONG — {path} is {len(data)} bytes on the "
              f"disk; the writer wrote {XF_SIZE}")
    else:
        print(f"xfile-verify: [2] {path} is {XF_SIZE} bytes on the disk")

    hdr = data[:XF_HDR]
    name = hdr[XF_NAME_OFF:XF_NAME_OFF + XF_NAME_LEN].split(b"\0")[0]
    name = name.decode("ascii", "replace")
    size_field = struct.unpack_from("<Q", hdr, 8)[0] if len(hdr) >= 16 else -1
    if hdr[:8] != XF_MAGIC or size_field != XF_SIZE or \
            name not in ("i386", "x86_64"):
        failed += 1
        print(f"xfile-verify: [3] WRONG — the header is not xfile_test's: "
              f"magic {hdr[:8]!r}, size {size_field}, writer {name!r}")
    elif writer is not None and name != writer:
        failed += 1
        print(f"xfile-verify: [3] WRONG — the header says {name} wrote it, "
              f"and {writer} was expected")
    else:
        print(f"xfile-verify: [3] the header says {XF_SIZE} bytes, written "
              f"by {name}")

    bad = [o for o in range(XF_HDR, min(len(data), XF_SIZE))
           if data[o] != xf_byte(o)]
    if bad:
        failed += 1
        o = bad[0]
        print(f"xfile-verify: [4] WRONG — {len(bad)} byte(s) differ; the "
              f"first at offset {o} (block {o // 4096} of 4 KiB) is "
              f"0x{data[o]:02x}, expected 0x{xf_byte(o):02x}")
        zeros = sum(1 for o in bad if data[o] == 0)
        if zeros == len(bad):
            print("    every one of them is zero: blocks the filesystem "
                  "allocated and nobody wrote")
    elif len(data) >= XF_SIZE:
        print(f"xfile-verify: [4] all {XF_SIZE - XF_HDR} bytes after the "
              f"header are the ones the pattern defines")

    print(f"xfile-verify: {4 - failed} of 4 checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
