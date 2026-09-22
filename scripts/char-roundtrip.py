#!/usr/bin/env python3
"""Type one character at the machine and see whether char_server hands it back.

#497's second clause -- "one character goes in and one comes out through it,
demonstrated rather than assumed" -- has an output half a boot can show by
itself and an input half it cannot.  char_test writes a line through
char_server and then waits for somebody to type; with nobody there it declines
the question and says NOT ASKED (#563).  This is the somebody.

    scripts/char-roundtrip.py --build uros/build-x86_64-497 --kvm

🔑 IT DRIVES QEMU DIRECTLY, and that is not a second harness.  run-x86_64.sh
boots and reads; nothing in it can WRITE to the serial line, because until now
no test on this target wanted to be typed at.  The qemu invocation below is
fbcons-readback.py's, for the same reason that one has it: a pipe instead of a
file is what makes the line two-way.

⚠️ It takes the SAME lock run-x86_64.sh takes.  Two qemus on one disk image is
a mistake this project has already made and misread as a failing kernel, so
the refusal is deliberate and its exit status is 2 -- a statement about the
caller, never about the kernel.

Exit status: 0 the round trip happened · 1 the machine answered wrongly ·
2 this script refused to start, or the boot never reached the question.
"""

import argparse
import fcntl
import os
import subprocess
import sys
import time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"

# What char_test prints when it has found the tty and is about to write.  The
# byte is typed AFTER this, so the input arm cannot be answered by something
# that was already in the FIFO before char_server owned the port.
READY = "char_test: the tty is device"

# What it prints when the input arm has decided, whichever way.
VERDICT = "char_test: [2]"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "uros/build-x86_64"),
                    help="build tree to boot (honours the same layout as "
                         "run-x86_64.sh)")
    ap.add_argument("--entry", type=int, default=14,
                    help="GRUB entry to boot (default 14)")
    ap.add_argument("--kvm", action="store_true", help="boot under KVM")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--send", default="u",
                    help="what to type; must match char_test's RX_EXPECT")
    ap.add_argument("--budget", type=float, default=180.0,
                    help="seconds to wait for the machine to be ready")
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"char-roundtrip: no kernel at {kernel}", file=sys.stderr)
        return 2

    env = dict(os.environ, UROS_BUILD_DIR=build,
               UROS_X86_64_BOOT_ENTRY=str(a.entry))
    subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                   env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        os.mkdir(LOCK)
    except FileExistsError:
        print(f"char-roundtrip: another x86-64 run is in flight ({LOCK}) — "
              f"refusing to interleave", file=sys.stderr)
        return 2

    fifo = f"/tmp/uros-charrt-{os.getpid()}.fifo"
    for end in (".in", ".out"):
        try:
            os.unlink(fifo + end)
        except FileNotFoundError:
            pass
        os.mkfifo(fifo + end)

    log = os.path.join(build, "char-roundtrip.log")

    argv = ["qemu-system-x86_64", "-cpu", "max", "-m", "512M",
            "-smp", str(a.smp),
            "-drive", f"file={build}/disk-x86_64.img,if=none,id=urosdisk,"
                      f"format=raw",
            "-device", "virtio-blk-pci,drive=urosdisk,bootindex=0",
            "-device", "ich9-ahci,id=ahci0",
            "-drive", f"file={build}/disk-x86_64-ahci.img,if=none,"
                      f"id=ahcidisk0,format=raw",
            "-device", "ide-hd,drive=ahcidisk0,bus=ahci0.0,bootindex=1",
            "-drive", f"file={build}/disk-x86_64-ahci2.img,if=none,"
                      f"id=ahcidisk1,format=raw",
            "-vga", "std", "-display", "none",
            "-serial", f"pipe:{fifo}", "-no-reboot"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")

    fd_out = os.open(fifo + ".out", os.O_RDONLY | os.O_NONBLOCK)
    fcntl.fcntl(fd_out, 1031, 1 << 20)          # F_SETPIPE_SZ: a whole boot
    fd_in = os.open(fifo + ".in", os.O_RDWR | os.O_NONBLOCK)

    q = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True)
    raw = bytearray()
    rc = 2

    def pump():
        try:
            while True:
                chunk = os.read(fd_out, 65536)
                if not chunk:
                    break
                raw.extend(chunk)
        except (BlockingIOError, OSError):
            pass
        return raw.decode(errors="replace")

    def wait_for(text, budget):
        end = time.time() + budget
        while time.time() < end:
            if text in pump():
                return True
            if q.poll() is not None:
                return text in pump()
            time.sleep(0.25)
        return False

    try:
        if not wait_for(READY, a.budget):
            err = (q.stderr.read() if q.poll() is not None else "").strip()
            print(f"char-roundtrip: the boot never reached {READY!r} in "
                  f"{a.budget:.0f}s — that says nothing about the round trip, "
                  f"the machine did not get there"
                  + (f": {err}" if err else ""), file=sys.stderr)
            return 2

        # char_test polls for two seconds; the line above is printed just
        # before its write, so a moment here lands the byte inside that
        # window rather than racing the attach.
        time.sleep(0.5)
        os.write(fd_in, a.send.encode("latin-1"))

        if not wait_for(VERDICT, 30.0):
            print(f"char-roundtrip: typed {a.send!r} and char_test never "
                  f"reached its input verdict", file=sys.stderr)
            return 2

        time.sleep(1.0)
        text = pump()
        open(log, "w").write(text)

        for line in text.splitlines():
            if VERDICT not in line:
                continue
            print(line.strip())
            if "WRONG" in line:
                rc = 1
            elif "NOT ASKED" in line:
                print("char-roundtrip: the byte was typed and char_test still "
                      "saw nothing — the input path did not carry it",
                      file=sys.stderr)
                rc = 1
            else:
                rc = 0
            break

        # Which path carried it is uart.so's to say, not this script's.
        for line in text.splitlines():
            if line.startswith("uart: IRQ 4 delivered") \
               or line.startswith("uart: a byte arrived"):
                print(line.strip())

        print(f"char-roundtrip: log: {log}")
    finally:
        if q.poll() is None:
            q.terminate()
            try:
                q.wait(timeout=10)
            except subprocess.TimeoutExpired:
                q.kill()
        os.close(fd_in)
        os.close(fd_out)
        for end in (".in", ".out"):
            try:
                os.unlink(fifo + end)
            except FileNotFoundError:
                pass
        os.rmdir(LOCK)

    return rc


if __name__ == "__main__":
    sys.exit(main())
