#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
"""chora's task in the debugger's task listing, under its device's name (#963).

#963's second box: the task carries its device's name, read back with the
routine -- chora_test's arm [3] -- and seen in the debugger's task listing,
which 1913ae7f taught to print a task's own name.  The listing is only there
to be seen at the debugger's prompt, so this boots the entry with chora.conf
and the debugger armed, waits until the first chora drives the network card
and chora_test has given its count, opens the prompt with Ctrl-\\ and asks `k',
the debugger's listing of tasks.  A row must carry exactly the name chora said
it has, and only one: the second chora was refused and has ended.

It drives QEMU directly, with the boot's disks, as ush-x86_64.py does and for
its reason -- nothing in run-x86_64.sh writes to the serial line -- and takes
the same lock.  The debugger harnesses' ISO will not do: it has no disk, and
chora is a program on the root disk.  The board is q35, whose card has an
MSI-X table: on the default board the card shares its INTx line, which chora
cannot take (#972).

    scripts/ddb-chora.py [--build uros/build-x86_64] [--kvm] [--smp N]

Exit status: 0 the listing names chora's task by its device, once · 1 it does
not, or the debugger never answered · 2 this script refused to start, or the
boot never got as far as chora driving the card.
"""
import argparse
import fcntl
import os
import re
import subprocess
import sys
import time

sys.dont_write_bytecode = True		# nothing left in scripts/
import run_lock				# noqa: E402

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
GRUB = os.path.join(REPO, "uros/src/mach_kernel/x86_64/boot/grub.cfg")
TITLE = "bootstrap -f chora.conf, -r: chora in the debugger's task listing"

# What chora says once its module drives the card, and its name in it.
DRIVES = re.compile(r"(\S+@[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]): \S+ drives the "
                    r"device")
COUNT = re.compile(r"chora_test: [0-9]+ of [0-9]+ arms passed")
PROMPT = "ddb> "
CTRL_BACKSLASH = b"\x1c"		# the console's door when -r arms it


def entry_number(title):
    """The GRUB entry titled "UrMach x86-64 (TITLE)": by its title, never its
    number, for the reason scripts/ddb-common.sh gives (#750)."""
    want = f'menuentry "UrMach x86-64 ({title})" {{'
    n = 0
    with open(GRUB, encoding="utf-8") as f:
        for line in f:
            if line.startswith("menuentry "):
                if line.rstrip("\n") == want:
                    return n
                n += 1
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    # UROS_BUILD_DIR when the caller set it, as run-x86_64.sh reads it (#485):
    # ablate.sh runs a Script: arm with its own build there.
    ap.add_argument("--build", default=os.environ.get("UROS_BUILD_DIR")
                    or os.path.join(REPO, "uros/build-x86_64"))
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--smp", type=int, default=4)
    ap.add_argument("--budget", type=float, default=600.0,
                    help="seconds for the boot to reach chora driving the "
                         "card (default 600)")
    ap.add_argument("--log", help="where to keep what the line carried "
                                  "(default UROS_X86_64_LOG, else "
                                  "ddb-chora.log in the build)")
    a = ap.parse_args()

    entry = entry_number(TITLE)
    if entry is None:
        print(f"ddb-chora: grub.cfg has no entry titled "
              f"\"UrMach x86-64 ({TITLE})\"", file=sys.stderr)
        return 2

    build = os.path.realpath(a.build)
    run_lock.stop_like_ctrl_c()
    mine = run_lock.take("ddb-chora")
    if mine is None:
        return 2
    try:
        return session(a, build, entry)
    finally:
        run_lock.give_back(mine, "ddb-chora")


def session(a, build, entry):
    # The targets run-x86_64.sh builds, and the disk it makes, for this entry.
    r = subprocess.run(["ninja", "-C", build, "mach_kernel", "boot_probe",
                        "name_server_bin", "bootstrap_server",
                        "bootstrap_bundle"],
                       stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
                       text=True)
    if r.returncode != 0:
        print(f"ddb-chora: the build failed: {r.stderr.strip()[-400:]}",
              file=sys.stderr)
        return 2
    env = dict(os.environ, UROS_BUILD_DIR=build,
               UROS_X86_64_BOOT_ENTRY=str(entry))
    subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                   env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fifo = os.path.join(build, f"ddb-chora-serial-{os.getpid()}.fifo")
    for end in (".in", ".out"):
        try:
            os.unlink(fifo + end)
        except FileNotFoundError:
            pass
        os.mkfifo(fifo + end)
        run_lock.remove_at_end(fifo + end)  # #1016: gone with the run, however it ends

    log = (a.log or os.environ.get("UROS_X86_64_LOG")
           or os.path.join(build, "ddb-chora.log"))

    argv = ["qemu-system-x86_64", "-machine", "q35", "-cpu", "max",
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
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1",
            "-vga", "std", "-display", "none",
            "-serial", f"pipe:{fifo}", "-no-reboot"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")
    print(f"ddb-chora: GRUB entry {entry}, \"UrMach x86-64 ({TITLE})\", "
          f"{'KVM' if a.kvm else 'TCG'}, -smp {a.smp}, q35")

    fd_out = os.open(fifo + ".out", os.O_RDONLY | os.O_NONBLOCK)
    fcntl.fcntl(fd_out, 1031, 1 << 20)          # F_SETPIPE_SZ: a whole boot
    fd_in = os.open(fifo + ".in", os.O_RDWR | os.O_NONBLOCK)

    q = run_lock.start_qemu(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    raw = bytearray()

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

    def wait_until(found, budget):
        """Wait until found(text) answers something other than None."""
        end = time.time() + budget
        while time.time() < end:
            got = found(pump())
            if got is not None:
                return got
            if q.poll() is not None:
                return None
            time.sleep(0.1)
        return None

    try:
        drives = wait_until(lambda t: DRIVES.search(t) if COUNT.search(t)
                            else None, a.budget)
        if drives is None:
            print(f"ddb-chora: in {a.budget:.0f}s no chora drove the card "
                  f"and chora_test gave its count", file=sys.stderr)
            return 2
        name = drives.group(1)
        print(f"ddb-chora: chora drives the card as \"{name}\"")

        prompts = pump().count(PROMPT)
        os.write(fd_in, CTRL_BACKSLASH)
        if wait_until(lambda t: True if t.count(PROMPT) > prompts else None,
                      60) is None:
            print("ddb-chora: WRONG -- Ctrl-\\ opened no prompt",
                  file=sys.stderr)
            return 1
        asked_at = len(pump())
        prompts = pump().count(PROMPT)
        os.write(fd_in, b"k\n")
        if wait_until(lambda t: True if t.count(PROMPT) > prompts else None,
                      60) is None:
            print("ddb-chora: WRONG -- the debugger did not answer `k'",
                  file=sys.stderr)
            return 1
        listing = pump()[asked_at:].replace("\r", "")
        rows = [l.strip() for l in listing.splitlines() if 'task "' in l]
        mine = [l for l in rows if f'task "{name}"' in l]
        os.write(fd_in, b"c\n")
        for l in rows:
            print(f"ddb-chora: | {l}")
        if len(mine) == 1:
            print(f"ddb-chora: PASS -- the debugger's task listing names "
                  f"chora's task \"{name}\", once: {mine[0]}")
            return 0
        print(f"ddb-chora: WRONG -- {len(mine)} rows of the debugger's task "
              f"listing name \"{name}\", where one should", file=sys.stderr)
        return 1
    finally:
        try:
            open(log, "w").write(pump())
        except OSError:
            pass
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
        print(f"ddb-chora: log: {log}")


if __name__ == "__main__":
    sys.exit(main())
