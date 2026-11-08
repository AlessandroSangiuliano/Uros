#!/usr/bin/env python3
"""Boot to ush on x86-64 and type at it.

#414's box "ush reaches its prompt" and #501's "ush$ with a command that runs":
a shell that prints a prompt has shown its start-up, and only a command typed at
it shows the rest -- the line discipline, fork, execve through exec_server, the
new image's stdout on the controlling terminal, and waitpid bringing the prompt
back.  So this waits for the prompt, types, and reads what comes back.

    cmake -S uros -B uros/build-x86_64-ush -C uros/cmake/presets/x86_64.cmake \\
          -DOSFMK_BUILD_CHAR_SERVER=ON -DUROS_BUNDLE_USH=ON
    ninja -C uros/build-x86_64-ush
    scripts/ush-x86_64.py --build uros/build-x86_64-ush --kvm

It drives QEMU directly for the reason scripts/char-roundtrip.py does, and takes
the same lock: nothing in run-x86_64.sh can write to the serial line.

Exit status: 0 every command did what it should · 1 the machine answered wrongly
· 2 this script refused to start, or the boot never reached the prompt.
"""

import argparse
import fcntl
import os
import subprocess
import sys
import time

sys.dont_write_bytecode = True		# nothing left in scripts/
import run_lock				# noqa: E402

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))

PROMPT = "ush$ "

# ush starts from the bundle, and the root disk is mounted a moment later: a
# command typed at the first prompt is answered ENOENT, correctly.
ROOT_MOUNTED = "ext2: ready, entering message loop"

# What is typed, and the line each command must print before the next prompt.
# ⚠️ A whole line: `hello' alone is found in the shell's own
# "execve(/hello_world) failed", and a test passed on that.  The line ends as
# this line discipline ends it, with a bare newline.
COMMANDS = (
    ("/hello_world", "\nhello\n"),
    ("/hello_dyn_world", "\nhello\n"),
)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.path.join(REPO, "uros/build-x86_64-ush"),
                    help="a build configured with UROS_BUNDLE_USH")
    ap.add_argument("--entry", type=int, default=14,
                    help="GRUB entry to boot (default 14)")
    ap.add_argument("--kvm", action="store_true", help="boot under KVM")
    ap.add_argument("--smp", type=int, default=2)
    ap.add_argument("--budget", type=float, default=300.0,
                    help="seconds to wait for the prompt")
    ap.add_argument("--log", help="where to keep what the line carried")
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"ush-x86_64: no kernel at {kernel}", file=sys.stderr)
        return 2

    run_lock.stop_like_ctrl_c()
    mine = run_lock.take("ush-x86_64")
    if mine is None:
        return 2
    try:
        return session(a, build)
    finally:
        run_lock.give_back(mine, "ush-x86_64")


def session(a, build):
    env = dict(os.environ, UROS_BUILD_DIR=build,
               UROS_X86_64_BOOT_ENTRY=str(a.entry))
    subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                   env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    fifo = os.path.join(build, f"ush-serial-{os.getpid()}.fifo")
    for end in (".in", ".out"):
        try:
            os.unlink(fifo + end)
        except FileNotFoundError:
            pass
        os.mkfifo(fifo + end)

    log = a.log or os.path.join(build, "ush-x86_64.log")

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
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1",
            "-vga", "std", "-display", "none",
            "-serial", f"pipe:{fifo}", "-no-reboot"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")

    fd_out = os.open(fifo + ".out", os.O_RDONLY | os.O_NONBLOCK)
    fcntl.fcntl(fd_out, 1031, 1 << 20)          # F_SETPIPE_SZ: a whole boot
    fd_in = os.open(fifo + ".in", os.O_RDWR | os.O_NONBLOCK)

    q = run_lock.start_qemu(argv, stdout=subprocess.DEVNULL,
                            stderr=subprocess.PIPE, text=True)
    raw = bytearray()
    seen = 0
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
        """Wait for `text' after what has been read already, and move past it."""
        nonlocal seen
        end = time.time() + budget
        while time.time() < end:
            at = pump().find(text, seen)
            if at >= 0:
                seen = at + len(text)
                return True
            if q.poll() is not None:
                return False
            time.sleep(0.1)
        return False

    try:
        if not wait_for(PROMPT, a.budget):
            err = (q.stderr.read() if q.poll() is not None else "").strip()
            print(f"ush-x86_64: the boot never reached {PROMPT!r} in "
                  f"{a.budget:.0f}s" + (f": {err}" if err else ""),
                  file=sys.stderr)
            return 2
        print("ush-x86_64: the prompt is there")

        if not wait_for(ROOT_MOUNTED, a.budget):
            print("ush-x86_64: the root disk was never mounted", file=sys.stderr)
            return 2

        rc = 0
        for command, want in COMMANDS:
            os.write(fd_in, (command + "\r").encode("latin-1"))
            # The echo of the command first: an answer read before it would
            # be the name of the command, and /hello_world has `hello' in it.
            if not wait_for(command, 30.0):
                print(f"ush-x86_64: typed {command!r} and it was not echoed",
                      file=sys.stderr)
                rc = 1
                break
            if not wait_for(want, 60.0):
                print(f"ush-x86_64: {command!r} never said {want!r}",
                      file=sys.stderr)
                rc = 1
                break
            if not wait_for(PROMPT, 60.0):
                print(f"ush-x86_64: {command!r} ran and the prompt did not "
                      f"come back", file=sys.stderr)
                rc = 1
                break
            print(f"ush-x86_64: {command} printed its line, and the prompt "
                  f"came back")
        print(f"ush-x86_64: log: {log}")
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

    return rc


if __name__ == "__main__":
    sys.exit(main())
