#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# fbcons-uefi-test.py — validate the in-kernel emergency framebuffer console
# (i386/AT386/fbcons.c, #342) under QEMU, with no UEFI/OVMF and no real
# hardware needed.
#
# How it works: GRUB on a multiboot2 boot sets a linear (VBE) framebuffer and
# hands its address to the kernel via the mb2 framebuffer tag.  SeaBIOS +
# "-vga std" gives a real linear framebuffer in QEMU, so booting the GRUB ISO
# there exercises exactly the path a pure-UEFI machine (GOP) uses.  The kernel
# is booted with "-r" so the same bytes go to BOTH COM1 (captured as the serial
# log) AND fbcons (captured as a QMP screendump) — an A/B of what fbcons drew.
#
# Steps: refresh the GRUB ISO payload from the build tree, build the ISO, boot
# one QEMU headless with a QMP socket, wait for a boot marker on the serial
# log, screendump the framebuffer, quit.  Outputs land in uros/build/ (PNG if
# Pillow is installed, otherwise the raw PPM).
#
# Usage:  scripts/fbcons-uefi-test.py
# Needs:  qemu-system-i386 (+KVM), grub-mkrescue, mtools, python3 (Pillow opt).

import json, os, socket, subprocess, sys, time

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(REPO_ROOT, "uros", "build")
ISO_TREE = os.path.join(BUILD, "uefi-iso")
ISO = os.path.join(BUILD, "uros-uefi.iso")
QMP = "/tmp/fbcons-qmp.sock"
SER = os.path.join(BUILD, "fbcons-serial.log")
PPM = os.path.join(BUILD, "fbcons.ppm")
PNG = os.path.join(BUILD, "fbcons.png")

KERNEL = os.path.join(BUILD, "export", "uros", "boot", "mach_kernel")
BOOTSTRAP = os.path.join(BUILD, "export", "uros", os.uname().machine, "user", "sbin", "bootstrap")
BUNDLE = os.path.join(BUILD, "bootstrap.bundle")
KSYMS = os.path.join(BUILD, "export", "uros", "boot", "ksyms.bin")

GRUB_CFG = """\
serial --unit=0 --speed=115200
terminal_input  serial console
terminal_output serial console
insmod all_video
insmod vbe
set gfxpayload=1024x768x32
set timeout=1
menuentry "Uros (multiboot2 + framebuffer)" {
    multiboot2 /boot/mach_kernel -r
    module2    /boot/bootstrap        bootstrap
    module2    /boot/bootstrap.bundle bundle
    module2    /boot/ksyms.bin        ksyms
    boot
}
"""

MARKERS = ("init complete", "Benchmark complete", "no partitions", "blk:")


def build_iso():
    import shutil
    boot = os.path.join(ISO_TREE, "boot")
    os.makedirs(os.path.join(boot, "grub"), exist_ok=True)
    shutil.copy(KERNEL, os.path.join(boot, "mach_kernel"))
    shutil.copy(BOOTSTRAP, os.path.join(boot, "bootstrap"))
    shutil.copy(BUNDLE, os.path.join(boot, "bootstrap.bundle"))
    if os.path.exists(KSYMS):
        shutil.copy(KSYMS, os.path.join(boot, "ksyms.bin"))
    with open(os.path.join(boot, "grub", "grub.cfg"), "w") as fh:
        fh.write(GRUB_CFG)
    subprocess.run(["grub-mkrescue", "-o", ISO, ISO_TREE],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def qmp_connect(path, timeout=15):
    t0 = time.time()
    while time.time() - t0 < timeout:
        try:
            s = socket.socket(socket.AF_UNIX)
            s.connect(path)
            return s
        except (FileNotFoundError, ConnectionRefusedError):
            time.sleep(0.1)
    raise RuntimeError("qmp connect timeout")


def qmp(s, cmd, **args):
    o = {"execute": cmd}
    if args:
        o["arguments"] = args
    s.sendall((json.dumps(o) + "\n").encode())
    buf = b""
    while True:
        buf += s.recv(65536)
        for line in buf.split(b"\n"):
            if not line.strip():
                continue
            try:
                m = json.loads(line)
            except ValueError:
                continue
            if "return" in m or "error" in m:
                return m
        time.sleep(0.02)


def main():
    for f in (QMP, SER, PPM, PNG):
        try:
            os.unlink(f)
        except FileNotFoundError:
            pass

    print("building GRUB multiboot2 ISO ...")
    build_iso()

    print("booting QEMU (SeaBIOS + std-vga, headless) ...")
    qemu = subprocess.Popen([
        "qemu-system-i386", "-m", "512M", "-enable-kvm",
        "-cdrom", ISO, "-boot", "d",
        "-vga", "std", "-display", "none",
        "-serial", "file:%s" % SER,
        "-qmp", "unix:%s,server,nowait" % QMP,
    ], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    rc = 1
    try:
        s = qmp_connect(QMP)
        s.recv(65536)                  # greeting
        qmp(s, "qmp_capabilities")

        t0 = time.time()
        while time.time() - t0 < 45:
            try:
                seen = open(SER, "rb").read().decode("latin1")
            except FileNotFoundError:
                seen = ""
            if any(m in seen for m in MARKERS):
                break
            time.sleep(0.3)
        time.sleep(1.5)                # let the last lines flush to the fb

        r = qmp(s, "screendump", filename=PPM)
        ok = "return" in r
        qmp(s, "quit")
        rc = 0 if ok else 2
    except Exception as e:                            # noqa: BLE001
        print("ERROR:", e)
    finally:
        try:
            qemu.wait(timeout=8)
        except Exception:
            qemu.terminate()
            try:
                qemu.wait(timeout=4)
            except Exception:
                qemu.kill()

    if os.path.exists(PPM):
        try:
            from PIL import Image
            with open(PPM, "rb") as fh:
                assert fh.readline().strip() == b"P6"
                w, h = map(int, fh.readline().split())
                fh.readline()
                Image.frombytes("RGB", (w, h), fh.read()).save(PNG)
            print("screenshot: %s (%dx%d)" % (PNG, w, h))
        except Exception as e:                       # noqa: BLE001
            print("screenshot: %s (Pillow unavailable: %s)" % (PPM, e))
    else:
        print("no screendump produced")
        rc = 2

    print("serial log: %s" % SER)
    sys.exit(rc)


if __name__ == "__main__":
    main()
