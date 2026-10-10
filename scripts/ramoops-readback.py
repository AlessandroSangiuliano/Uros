#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# ramoops-readback.py — does the kernel restart the machine when the boot is
# over, and does the console it kept in RAM say what the serial line said?
# (#373)
#
# On the bare metal of #595 there is no serial port.  The kernel writes its
# console into a zone of RAM that Linux reserves (memmap=1M$ADDR) and reads
# back after a reset with ramoops (fs/pstore), and with -b it resets the
# machine itself after the quiet census.  Under QEMU both halves can be read
# exactly: the guest's reset is turned into a pause (-action reboot=shutdown
# -action shutdown=pause), so the machine stops at the instant the kernel
# resets it, its memory as the kernel left it.  The zone is read out of guest
# memory with the monitor's pmemsave, parsed by ramoops' own rules, and
# compared with the serial log of the same boot.
#
# ramoops' rules, from fs/pstore/ram_core.c (Linux master, read 10/10/2026):
# struct persistent_ram_buffer { u32 sig; atomic_t start; atomic_t size;
# u8 data[]; }, the console zone's sig 0x43474244 ("DBGC"); what it holds is
# data[start..size) followed by data[0..start); taken only if size is at most
# the zone minus 12 and start at most size -- anything else is "found existing
# invalid buffer" and the zone is wiped, and a size of zero is no record at
# all.  Linux writes nothing in the zone with its console off (Manjaro's
# kernels have CONFIG_PSTORE_CONSOLE off), so what ramoops reads is exactly
# what this kernel wrote.
#
# What it tells apart:
#   - the guest never reset                    -> -b did not fire, or the reset did nothing
#   - no zone line on the serial line           -> the kernel did not start the zone
#   - a zone ramoops would not take             -> sig, start or size wrong
#   - a zone that is not the serial log's tail  -> bytes lost, added or out of order
#   - a zone that IS the tail, from the kernel's first line in C to the
#     reset's last line                         -> Linux will read what was said
#
# The serial log begins with what the zone never sees, and must not: the
# firmware and GRUB, and the three lines boot.S writes to COM1 itself before
# there is a console.  The zone begins at the first byte the console was
# handed, which #666 kept: "UrMach x86-64: reached C in long mode".
#
# Usage:
#   scripts/ramoops-readback.py [--build DIR] [--entry N] [--kvm] [--smp N]
#                               [--machine pc|q35] [--budget S] [--out DIR]
#                               [--no-zone]
#
# Exit:  0  the guest reset, and the zone is the serial log's tail
#        1  the kernel's side is wrong: no reset, no zone, or a zone that
#           disagrees with the log
#        2  the experiment could not be run (no qemu, no monitor, no boot)
#
# ⚠️ One run at a time, with the harness's own lock: two qemus on one machine
# manufacture each other's failures (run-x86_64.sh says why).

import argparse, os, re, socket, struct, subprocess, sys, time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"

RAMOOPS_SIG = 0x43474244
RAMOOPS_CONSOLE = 0x80000		# ramoops.console_size
HEADER = 12
FIRST_C_LINE = b"UrMach x86-64: reached C in long mode"
# The three ways -b sends the machine down, as the kernel says them.
B_REASONS = (rb"quiet_census: -b: the boot is over; restarting the machine",
             rb"reset: -b: \d+ seconds since the scheduler started and the "
             rb"boot has not ended; restarting the machine",
             rb"panic: -b: restarting the machine in \d+ seconds")


def hmp(mon, cmd, timeout=10.0):
    """One human-monitor command; its output, without the prompts."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect(mon)
    buf = b""
    while not buf.endswith(b"(qemu) "):
        chunk = s.recv(65536)
        if not chunk:
            raise OSError("the monitor closed before its prompt")
        buf += chunk
    s.sendall(cmd.encode() + b"\n")
    buf = b""
    while not buf.endswith(b"(qemu) "):
        chunk = s.recv(65536)
        if not chunk:
            break
        buf += chunk
    s.close()
    text = buf.decode(errors="replace").replace("\r", "")
    return text.split("\n", 1)[1].rsplit("(qemu)", 1)[0].strip() \
        if "\n" in text else ""


def parse_zone(raw):
    """What ramoops would make of the zone: (verdict, text or None)."""
    sig, start, size = struct.unpack_from("<III", raw, 0)
    cap = RAMOOPS_CONSOLE - HEADER
    data = raw[HEADER:HEADER + cap]
    head = f"sig {sig:#010x}, start {start}, size {size}, capacity {cap}"
    if sig != RAMOOPS_SIG:
        return f"{head}: NOT the console zone's signature — ramoops would " \
               f"say 'no valid data' and wipe it", None
    if size > cap or start > size:
        return f"{head}: ramoops would say 'found existing invalid buffer' " \
               f"and wipe it", None
    if size == 0:
        return f"{head}: EMPTY — ramoops would make no record of it", None
    return head, data[start:size] + data[0:start]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.environ.get(
        "UROS_BUILD_DIR", os.path.join(REPO, "uros/build-x86_64")))
    ap.add_argument("--entry", type=int, default=42)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--machine", default="pc", choices=("pc", "q35"),
                    help="pc's FADT has no reset register (revision 1), "
                         "q35's has one: the two paths of the reset")
    ap.add_argument("--budget", type=float, default=900.0)
    ap.add_argument("--out", default=None, help="where to leave the logs")
    ap.add_argument("--no-zone", action="store_true",
                    help="judge the reset only: the entry asks for -b and "
                         "no zone")
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    out = a.out or build
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"ramoops-readback: no kernel at {kernel}", file=sys.stderr)
        return 2

    env = dict(os.environ, UROS_BUILD_DIR=build,
               UROS_X86_64_BOOT_ENTRY=str(a.entry))
    made = subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                          env=env, stdout=subprocess.DEVNULL,
                          stderr=subprocess.PIPE, text=True)
    if made.returncode != 0:
        print("ramoops-readback: make-disk-x86_64.sh could not make the disk "
              "-- a build without its targets (the harness builds them: "
              "mach_kernel boot_probe name_server_bin bootstrap_server "
              "bootstrap_bundle)" +
              (f": {made.stderr.strip().splitlines()[-1]}"
               if made.stderr.strip() else ""), file=sys.stderr)
        return 2

    try:
        os.mkdir(LOCK)
    except FileExistsError:
        print(f"ramoops-readback: another run is in flight ({LOCK}) — "
              f"refusing to interleave", file=sys.stderr)
        return 2

    mon = f"/tmp/uros-ramoops-{os.getpid()}.mon"
    log = os.path.join(out, "ramoops-readback.log")
    zone_file = os.path.join(out, "ramoops-readback.zone")
    for p in (log, zone_file, mon):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

    argv = ["qemu-system-x86_64", "-machine", a.machine, "-cpu", "max",
            "-m", "512M", "-smp", str(a.smp),
            "-drive", f"file={build}/disk-x86_64.img,if=none,id=urosdisk,"
                      f"format=raw",
            "-device", "virtio-blk-pci,drive=urosdisk,bootindex=0",
            "-device", "ich9-ahci,id=ahci0",
            "-drive", f"file={build}/disk-x86_64-ahci.img,if=none,"
                      f"id=ahcidisk0,format=raw",
            "-device", "ide-hd,drive=ahcidisk0,bus=ahci0.0,bootindex=1",
            "-drive", f"file={build}/disk-x86_64-ahci2.img,if=none,"
                      f"id=ahcidisk1,format=raw",
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1,bootindex=2",
            "-display", "none", "-serial", f"file:{log}",
            "-monitor", f"unix:{mon},server=on,wait=off",
            # 🔑 The reset becomes a pause, and the pause keeps the memory.
            # -no-reboot would end qemu at the reset, and the zone with it.
            "-action", "reboot=shutdown", "-action", "shutdown=pause"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")

    q = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True)
    try:
        status = ""
        end = time.time() + a.budget
        while time.time() < end:
            time.sleep(1.0)
            if q.poll() is not None:
                err = q.stderr.read().strip()
                print(f"ramoops-readback: qemu exited ({q.returncode}) "
                      f"before the guest reset" + (f": {err}" if err else ""),
                      file=sys.stderr)
                return 2
            if not os.path.exists(mon):
                continue
            try:
                status = hmp(mon, "info status")
            except OSError:
                continue
            if "shutdown" in status:
                break
        serial = open(log, "rb").read() if os.path.exists(log) else b""
        norm = serial.replace(b"\r", b"")

        if "shutdown" not in status:
            print(f"ramoops-readback: the guest did not reset in "
                  f"{a.budget:.0f}s (status: {status or 'none'}), after "
                  f"{len(serial)} bytes on the serial line")
            tail = norm.rstrip(b"\n").split(b"\n")[-3:]
            for l in tail:
                print(f"  serial | {l.decode(errors='replace')}")
            return 1

        why = next((m.group(0) for m in (re.search(r, norm)
                                          for r in B_REASONS) if m), None)
        resets = [l for l in norm.split(b"\n") if l.startswith(b"reset: ")]
        print(f"ramoops-readback: the guest reset ({status}), "
              f"{len(serial)} bytes on the serial line")
        for l in resets:
            print(f"  serial | {l.decode(errors='replace')}")
        if why is None:
            print("ramoops-readback: the guest reset WITHOUT the kernel "
                  "saying -b sent it — a triple fault, or something else "
                  "that resets the machine")
            return 1
        print(f"  why    | {why.decode(errors='replace')}")

        if a.no_zone:
            print("ramoops-readback: passed: -b reset the machine (no zone "
                  "asked for)")
            return 0

        m = re.search(rb"ramoops: the zone at (0x[0-9a-fA-F]+)", norm)
        if m is None:
            print("ramoops-readback: NO ZONE LINE on the serial line — the "
                  "kernel did not start the zone")
            for l in norm.split(b"\n"):
                if l.startswith(b"ramoops"):
                    print(f"  serial | {l.decode(errors='replace')}")
            return 1
        addr = int(m.group(1), 16)
        said = hmp(mon, f"pmemsave {addr:#x} {RAMOOPS_CONSOLE:#x} "
                        f"\"{zone_file}\"", timeout=30.0)
        if not os.path.exists(zone_file):
            print(f"ramoops-readback: the monitor saved no zone"
                  + (f": {said}" if said else ""), file=sys.stderr)
            return 2
        raw = open(zone_file, "rb").read()
        verdict, text = parse_zone(raw)
        print(f"ramoops-readback: the zone at {addr:#x}: {verdict}")
        if text is None:
            return 1

        ztext = text.replace(b"\r", b"")
        wrapped = len(text) == RAMOOPS_CONSOLE - HEADER
        first = ztext.split(b"\n", 1)[0]
        last = ztext.rstrip(b"\n").split(b"\n")[-1]
        print(f"  zone   | first: {first.decode(errors='replace')}")
        print(f"  zone   | last:  {last.decode(errors='replace')}")
        if not norm.endswith(ztext):
            # Where they part: the longest common tail says how much agrees.
            n = 0
            while n < min(len(norm), len(ztext)) and \
                    norm[-1 - n] == ztext[-1 - n]:
                n += 1
            print(f"ramoops-readback: the zone is NOT the serial log's tail "
                  f"— they agree on the last {n} of the zone's "
                  f"{len(ztext)} bytes")
            return 1
        if not wrapped and not ztext.startswith(FIRST_C_LINE):
            print(f"ramoops-readback: the zone does not begin at the "
                  f"kernel's first line in C ({FIRST_C_LINE.decode()!r}) — "
                  f"what #666 kept before the zone existed is not in it")
            return 1
        print(f"ramoops-readback: passed: the zone is the serial log's last "
              f"{len(ztext)} bytes" +
              (" (it wrapped: the boot's first bytes were overwritten, as a "
               "ring does)" if wrapped else
               ", from the kernel's first line in C to the reset's last"))
        return 0
    finally:
        if q.poll() is None:
            try:
                hmp(mon, "quit")
            except OSError:
                pass
            try:
                q.wait(timeout=10)
            except subprocess.TimeoutExpired:
                q.kill()
        try:
            os.unlink(mon)
        except FileNotFoundError:
            pass
        os.rmdir(LOCK)


if __name__ == "__main__":
    sys.exit(main())
