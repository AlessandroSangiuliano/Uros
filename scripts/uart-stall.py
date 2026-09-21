#!/usr/bin/env python3
#
# uart-stall.py — make THRE never arrive, and see whether the kernel comes
# back (#551).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# cons_putc() waits for the UART's transmitter to have room.  Until #551 it
# waited for ever, under printf_lock with interrupts masked, and an
# unprivileged trap could ask for it.  Now it gives up.  A bound nothing has
# ever hit is untested code, so this is how it is hit:
#
#   1. qemu's COM1 goes to a named pipe whose capacity this script has set to
#      one page, and this script is its reader.  While it reads, the
#      transmitter drains and the kernel prints.
#   2. After --after bytes it STOPS reading.  The pipe fills, qemu's write
#      returns EAGAIN, qemu's 16550 leaves THRE clear -- and the kernel's next
#      byte finds no room.  This is the stuck transmitter of the issue, made
#      to order.
#
#      ⚠️ A pipe and not a pty, and the capacity set on purpose.  The first
#      version of this used a pseudo-terminal, whose buffers hold some 68 KB
#      before the master's write fails; an entry-6 boot prints 21 KB, so the
#      transmitter never stuck, every byte arrived, and BOTH kernels "kept
#      running" -- the unbounded one included.  A stall that cannot fill its
#      sink is not a stall, and the script that reported it had tested
#      nothing.  The pipe's capacity is F_SETPIPE_SZ'd to one page so that the
#      sink is full four kilobytes after the reader stops.
#   3. During the stall the processor is sampled through the monitor: RIP,
#      mapped to a symbol with nm.  A kernel that waits for ever is IN
#      cons_putc in every sample; a kernel that gives up is somewhere else,
#      doing whatever the boot does next.
#   4. Reading resumes, and what arrives is counted.  The bounded kernel lost
#      bytes -- that is the deal -- and went on; the unbounded one lost none
#      and went nowhere.
#
# 🔴 Run it against BOTH kernels.  The bounded one passing means nothing until
# the unbounded one (UROS_ABLATE_551_UNBOUNDED=ON, in a build tree of its own)
# has been seen failing here, in the word this script uses: STOPPED.  A test
# whose failure has never been observed has not been shown to be a test.
#
# Usage:
#   scripts/uart-stall.py [--build DIR] [--entry N] [--kvm] [--smp N]
#                         [--after BYTES] [--stall S] [--samples N]
#
# Exit:  0  the kernel kept running while the transmitter was stuck
#        1  the kernel was inside the UART wait for the whole stall: STOPPED
#        2  the experiment could not be run (no pty, no monitor, no boot)
#
# ⚠️ One run at a time, with the harness's own lock: two qemus on one machine
# manufacture each other's failures (run-x86_64.sh says why).

import argparse, bisect, fcntl, os, re, select, socket, subprocess, sys, time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"

def symbols(kernel):
    out = subprocess.run(["nm", "-n", "--defined-only", kernel],
                         capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in "tTwW":
            addrs.append(int(parts[0], 16)); names.append(parts[2])
    return addrs, names

def where(addrs, names, rip):
    i = bisect.bisect_right(addrs, rip) - 1
    return names[i] if i >= 0 else "?"

def monitor_rip(path):
    """RIP of every processor, from `info registers' on the monitor socket."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(3)
    try:
        s.connect(path)
        # the banner, then the command
        time.sleep(0.2)
        try: s.recv(65536)
        except socket.timeout: pass
        s.sendall(b"info registers -a\n")
        time.sleep(0.3)
        buf = b""
        try:
            while True:
                chunk = s.recv(65536)
                if not chunk: break
                buf += chunk
                if len(chunk) < 65536: break
        except socket.timeout:
            pass
        return [int(m, 16) for m in re.findall(rb"RIP=([0-9a-f]+)", buf)]
    finally:
        s.close()

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.environ.get("UROS_BUILD_DIR", os.path.join(REPO, "uros/build-x86_64")))
    ap.add_argument("--entry", type=int, default=6)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--after", type=int, default=8192, help="bytes to read before the stall")
    ap.add_argument("--stall", type=float, default=15.0, help="seconds the transmitter stays stuck")
    ap.add_argument("--samples", type=int, default=5)
    ap.add_argument("--budget", type=float, default=90.0, help="seconds to keep reading after the stall")
    ap.add_argument("--dump", help="write everything the console delivered to this file")
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"uart-stall: no kernel at {kernel}", file=sys.stderr); return 2

    # the disk, with the entry written into it, exactly as the harness does
    env = dict(os.environ, UROS_BUILD_DIR=build, UROS_X86_64_BOOT_ENTRY=str(a.entry))
    subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")], env=env,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        os.mkdir(LOCK)
    except FileExistsError:
        print(f"uart-stall: another run-x86_64.sh is in flight ({LOCK}) — refusing to interleave", file=sys.stderr)
        return 2

    mon = f"/tmp/uros-uart-stall-{os.getpid()}.mon"
    fifo = f"/tmp/uros-uart-stall-{os.getpid()}.fifo"
    # qemu's pipe chardev reads <path>.in and writes <path>.out; both must
    # exist or it falls back to one file for both directions.
    for end in (".in", ".out"):
        try: os.unlink(fifo + end)
        except FileNotFoundError: pass
        os.mkfifo(fifo + end)
    # The reader opens first, non-blocking, so that neither open() waits for
    # the other side; qemu opens its ends O_RDWR and never waits either.
    fd = os.open(fifo + ".out", os.O_RDONLY | os.O_NONBLOCK)
    F_SETPIPE_SZ = 1031
    fcntl.fcntl(fd, F_SETPIPE_SZ, 4096)
    fd_in = os.open(fifo + ".in", os.O_RDWR | os.O_NONBLOCK)	# never written; keeps qemu's reader quiet
    argv = ["qemu-system-x86_64", "-cpu", "max", "-m", "512M", "-smp", str(a.smp),
            "-drive", f"file={build}/disk-x86_64.img,if=none,id=urosdisk,format=raw",
            "-device", "virtio-blk-pci,drive=urosdisk,bootindex=0",
            "-device", "ich9-ahci,id=ahci0",
            "-drive", f"file={build}/disk-x86_64-ahci.img,if=none,id=ahcidisk0,format=raw",
            "-device", "ide-hd,drive=ahcidisk0,bus=ahci0.0,bootindex=1",
            "-drive", f"file={build}/disk-x86_64-ahci2.img,if=none,id=ahcidisk1,format=raw",
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1,bootindex=2",
            "-display", "none", "-serial", f"pipe:{fifo}",
            "-monitor", f"unix:{mon},server=on,wait=off", "-no-reboot"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")

    q = subprocess.Popen(argv, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
    time.sleep(1.0)
    if q.poll() is not None:
        err = q.stderr.read()
        os.rmdir(LOCK)
        print(f"uart-stall: qemu did not start: {err.strip()}", file=sys.stderr); return 2

    addrs, names = symbols(kernel)
    got_before = got_after = 0
    tail = b""
    dump = open(a.dump, "wb") if a.dump else None

    def drain(seconds, upto=None):
        """Read for `seconds', or until `upto' bytes have arrived."""
        nonlocal tail
        n = 0
        end = time.time() + seconds
        while time.time() < end and (upto is None or n < upto):
            r, _, _ = select.select([fd], [], [], 0.2)
            if r:
                try:
                    chunk = os.read(fd, 65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    time.sleep(0.05); continue	# qemu holds the write end open; empty means idle
                n += len(chunk); tail = (tail + chunk)[-4096:]
                if dump: dump.write(chunk)
            if q.poll() is not None:
                break
        return n

    print(f"uart-stall: {'KVM' if a.kvm else 'TCG'}, -smp {a.smp}, entry {a.entry}, {os.path.relpath(build, REPO)}")
    t_start = time.time()
    got_before = drain(60.0, upto=a.after)
    if got_before < a.after:
        q.kill(); os.rmdir(LOCK)
        print(f"uart-stall: only {got_before} bytes in 60s, the boot did not get going", file=sys.stderr); return 2
    print(f"uart-stall: {got_before} bytes read in {time.time() - t_start:.1f}s, now the transmitter is stuck for {a.stall:.0f}s")

    inside = 0
    seen = []
    for i in range(a.samples):
        time.sleep(a.stall / a.samples)
        try:
            rips = monitor_rip(mon)
        except OSError as e:
            print(f"uart-stall: monitor: {e}", file=sys.stderr); rips = []
        syms = [where(addrs, names, r) for r in rips]
        seen.append(syms)
        if any(s in ("cons_putc", "kputc") for s in syms):
            inside += 1
        print(f"  sample {i + 1}: " + (", ".join(f"{s} ({r:#x})" for r, s in zip(rips, syms)) or "(no answer)"))

    got_after = drain(a.budget)
    if dump: dump.close()
    q.kill(); q.wait()
    os.close(fd); os.close(fd_in); os.rmdir(LOCK)
    for path in (mon, fifo + ".in", fifo + ".out"):
        try: os.unlink(path)
        except OSError: pass
    if any(not syms for syms in seen):
        print("uart-stall: the monitor did not answer during the stall — qemu itself was stuck, "
              "which says nothing about the kernel; not a result")
        return 2

    print(f"uart-stall: {got_after} bytes read after the transmitter drained again")
    if inside == a.samples:
        print(f"uart-stall: STOPPED — in the UART wait in {inside} of {a.samples} samples while the transmitter was stuck")
        return 1
    if inside == 0:
        print(f"uart-stall: kept running — in the UART wait in 0 of {a.samples} samples while the transmitter was stuck")
        return 0
    print(f"uart-stall: in the UART wait in {inside} of {a.samples} samples — neither answer; look at the samples")
    return 1

if __name__ == "__main__":
    sys.exit(main())
