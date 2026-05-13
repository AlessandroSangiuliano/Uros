#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# gen-ksyms.py — extract kernel symbol table from a linked ELF and emit
# a self-describing binary blob (ksyms.bin) loaded as a multiboot module
# at boot.  See docs / issue #211.
#
# Layout (little-endian):
#   char     magic[4]      = "KSYM"
#   uint32_t version       = 1
#   uint32_t n_syms
#   uint32_t strtab_off    (from start of file)
#   uint32_t strtab_size
#   entries[n_syms] { uint32_t addr; uint32_t name_off; }   sorted by addr
#   strings (null-terminated, name_off references this region)

import argparse
import struct
import subprocess
import sys

HEADER_FMT = "<4sIIII"
ENTRY_FMT  = "<II"

# Symbol types we care about (text/data, global or local).
KEEP_TYPES = set("TtWwBbDdRrAa")


def collect(nm, kernel):
    out = subprocess.check_output([nm, "-n", kernel], text=True)
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3:
            continue
        addr_s, t, name = parts
        if t not in KEEP_TYPES:
            continue
        # Skip compiler-emitted clutter (.LC0, $LPB123, etc.).
        if name.startswith(".") or name.startswith("$"):
            continue
        try:
            addr = int(addr_s, 16)
        except ValueError:
            continue
        syms.append((addr, name))
    # Already sorted by nm -n, but nm may emit duplicates at the same addr —
    # keep only the first to avoid lookup ambiguity.
    seen = set()
    uniq = []
    for addr, name in syms:
        if addr in seen:
            continue
        seen.add(addr)
        uniq.append((addr, name))
    return uniq


def emit(syms, out_path):
    strtab = bytearray()
    offsets = []
    interned = {}
    for _, name in syms:
        if name in interned:
            offsets.append(interned[name])
            continue
        off = len(strtab)
        interned[name] = off
        offsets.append(off)
        strtab.extend(name.encode("ascii"))
        strtab.append(0)

    n = len(syms)
    header_size = struct.calcsize(HEADER_FMT)
    entries_size = n * struct.calcsize(ENTRY_FMT)
    strtab_off = header_size + entries_size

    with open(out_path, "wb") as fp:
        fp.write(struct.pack(HEADER_FMT, b"KSYM", 1, n, strtab_off, len(strtab)))
        for (addr, _), name_off in zip(syms, offsets):
            fp.write(struct.pack(ENTRY_FMT, addr, name_off))
        fp.write(strtab)

    return n, strtab_off + len(strtab)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("kernel", help="linked kernel ELF")
    ap.add_argument("output", help="output ksyms.bin")
    ap.add_argument("--nm", default="nm", help="nm binary to use")
    args = ap.parse_args()

    syms = collect(args.nm, args.kernel)
    n, total = emit(syms, args.output)
    print(f"gen-ksyms: {n} symbols, {total} bytes -> {args.output}",
          file=sys.stderr)


if __name__ == "__main__":
    main()
