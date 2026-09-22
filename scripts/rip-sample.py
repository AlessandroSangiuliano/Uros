#!/usr/bin/env python3
#
# rip-sample.py — where is the processor, while the boot is doing something
# expensive (#566).
#
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# A poor sampling profiler and nothing more: boot the disk image, ask qemu's
# monitor for every processor's RIP a few times a second, map each address to
# the nearest preceding symbol with nm, and print what came up most.
#
# 🔑 IT ANSWERS "WHERE", NOT "WHY", and it answers it about the KERNEL only:
# an address in ring 3 lands below the kernel's lowest symbol and is counted as
# `(user)'.  That distinction is the first thing worth knowing about a cost --
# #566 needed to know whether a trap sweep's missing milliseconds were spent in
# the kernel at all.
#
# ⚠️ Samples are NOT evenly spaced: each one is a monitor round trip, so the
# rate depends on the host.  The counts rank; they do not integrate.
#
# Usage:
#   scripts/rip-sample.py [--build DIR] [--entry N] [--kvm] [--smp N]
#                         [--seconds S] [--every MS] [--grep TEXT]
#
#   --grep TEXT   start sampling when the console prints TEXT.
#   --until TEXT  stop when it prints TEXT.  The pair is how a window of one
#                 line is isolated: a sweep row prints when it FINISHES, so
#                 arming on the row before and stopping on the row itself is
#                 exactly the interval that row spent (#566).
set = None
import argparse, bisect, os, re, select, socket, subprocess, sys, time, collections

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"


def symbols(kernel):
    out = subprocess.run(["nm", "-n", "--defined-only", kernel],
                         capture_output=True, text=True, check=True).stdout
    addrs, names = [], []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "tTwW":
            addrs.append(int(p[0], 16)); names.append(p[2])
    return addrs, names


def where(addrs, names, rip):
    if not addrs or rip < addrs[0]:
        return "(user)"
    return names[bisect.bisect_right(addrs, rip) - 1]


def monitor_rips(path):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(2)
    try:
        s.connect(path)
        time.sleep(0.05)
        try: s.recv(65536)
        except socket.timeout: pass
        s.sendall(b"info registers -a\n")
        time.sleep(0.12)
        buf = b""
        try:
            while True:
                c = s.recv(65536)
                if not c: break
                buf += c
                if len(c) < 65536: break
        except socket.timeout:
            pass
        return [int(m, 16) for m in re.findall(rb"RIP=([0-9a-f]+)", buf)]
    except OSError:
        return []
    finally:
        s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.environ.get(
        "UROS_BUILD_DIR", os.path.join(REPO, "uros/build-x86_64")))
    ap.add_argument("--entry", type=int, default=14)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--seconds", type=float, default=90.0)
    ap.add_argument("--every", type=float, default=150.0, help="milliseconds between samples")
    ap.add_argument("--grep", help="sample only after this text appears on the console")
    ap.add_argument("--until", dest="until", help="stop sampling when this text appears")
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print("rip-sample: no kernel at %s" % kernel, file=sys.stderr); return 2

    env = dict(os.environ, UROS_BUILD_DIR=build, UROS_X86_64_BOOT_ENTRY=str(a.entry))
    subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")], env=env,
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        os.mkdir(LOCK)
    except FileExistsError:
        print("rip-sample: another run is in flight (%s)" % LOCK, file=sys.stderr); return 2

    mon = "/tmp/uros-rip-%d.mon" % os.getpid()
    argv = ["qemu-system-x86_64", "-cpu", "max", "-m", "512M", "-smp", str(a.smp),
            "-drive", "file=%s/disk-x86_64.img,if=none,id=urosdisk,format=raw" % build,
            "-device", "virtio-blk-pci,drive=urosdisk,bootindex=0",
            "-device", "ich9-ahci,id=ahci0",
            "-drive", "file=%s/disk-x86_64-ahci.img,if=none,id=ahcidisk0,format=raw" % build,
            "-device", "ide-hd,drive=ahcidisk0,bus=ahci0.0,bootindex=1",
            "-drive", "file=%s/disk-x86_64-ahci2.img,if=none,id=ahcidisk1,format=raw" % build,
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1,bootindex=2",
            "-display", "none", "-serial", "stdio",
            "-monitor", "unix:%s,server=on,wait=off" % mon, "-no-reboot"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")

    q = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    addrs, names = symbols(kernel)
    hist = collections.Counter()
    armed = a.grep is None
    console = []
    end = time.time() + a.seconds
    next_sample = time.time()

    while time.time() < end and q.poll() is None:
        r, _, _ = select.select([q.stdout], [], [], 0.02)
        if r:
            line = q.stdout.readline()
            if line:
                text = line.decode("utf-8", "replace")
                console.append(text)
                if a.grep and a.grep in text:
                    armed = True
                if a.until and armed and a.until in text:
                    break
        if armed and time.time() >= next_sample:
            for rip in monitor_rips(mon):
                hist[where(addrs, names, rip)] += 1
            next_sample = time.time() + a.every / 1000.0

    q.kill(); q.wait()
    os.rmdir(LOCK)
    try: os.unlink(mon)
    except OSError: pass

    total = sum(hist.values())
    print("\n=== rip-sample: %d samples, %s, -smp %d, entry %d%s%s ===" % (
        total, "KVM" if a.kvm else "TCG", a.smp, a.entry,
        (", from %r" % a.grep) if a.grep else "",
        (" to %r" % a.until) if a.until else ""))
    if not total:
        print("no samples: the monitor never answered"); return 2
    for name, n in hist.most_common(18):
        print("  %6.2f%%  %5d  %s" % (100.0 * n / total, n, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
