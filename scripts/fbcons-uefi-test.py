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
# Usage:  scripts/fbcons-uefi-test.py            # diskless: early boot only
#         scripts/fbcons-uefi-test.py --disk     # attach disk.img, run the
#                                                # bundle's benchmark to the end
# Needs:  qemu-system-i386 (+KVM), grub-mkrescue, mtools, python3 (Pillow opt).
#         --disk expects uros/build/disk.img (build it with a run-qemu --bench).

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
DISK = os.path.join(BUILD, "disk.img")

def grub_cfg(want_fb):
    # want_fb: request a linear framebuffer (gfxpayload) so the kernel gets an
    # mb2 framebuffer tag and fbcons activates.  --no-fb keeps GRUB in text mode
    # (no framebuffer tag -> fbcons stays inactive) to A/B the fbcons cost.
    video = ("insmod all_video\ninsmod vbe\nset gfxpayload=1024x768x32\n"
             if want_fb else "")
    # '-f' turns on the in-kernel framebuffer console (off by default); only
    # meaningful when we also requested a framebuffer.
    kargs = "-r -f" if want_fb else "-r"
    return ("serial --unit=0 --speed=115200\n"
            "terminal_input  serial console\n"
            "terminal_output serial console\n"
            + video +
            "set timeout=1\n"
            'menuentry "Uros (multiboot2)" {\n'
            "    multiboot2 /boot/mach_kernel %s\n" % kargs +
            "    module2    /boot/bootstrap        bootstrap\n"
            "    module2    /boot/bootstrap.bundle bundle\n"
            "    module2    /boot/ksyms.bin        ksyms\n"
            "    boot\n"
            "}\n")

# Diskless: stop once the early servers are up (boot halts at "no partitions").
# With --disk: attach disk.img (AHCI) so stage-2 mounts the root fs and the
# bundle's benchmark suite runs to completion.
MARKERS_DISKLESS = ("init complete", "no partitions", "blk:")
MARKER_DISK = "Benchmark complete"


def build_iso(want_fb):
    import shutil
    boot = os.path.join(ISO_TREE, "boot")
    os.makedirs(os.path.join(boot, "grub"), exist_ok=True)
    shutil.copy(KERNEL, os.path.join(boot, "mach_kernel"))
    shutil.copy(BOOTSTRAP, os.path.join(boot, "bootstrap"))
    shutil.copy(BUNDLE, os.path.join(boot, "bootstrap.bundle"))
    if os.path.exists(KSYMS):
        shutil.copy(KSYMS, os.path.join(boot, "ksyms.bin"))
    with open(os.path.join(boot, "grub", "grub.cfg"), "w") as fh:
        fh.write(grub_cfg(want_fb))
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

    want_fb = "--no-fb" not in sys.argv
    print("building GRUB multiboot2 ISO (%s) ..."
          % ("framebuffer" if want_fb else "text mode, fbcons off"))
    build_iso(want_fb)

    want_disk = "--disk" in sys.argv or "--bench" in sys.argv
    cmd = [
        "qemu-system-i386", "-m", "512M", "-enable-kvm", "-cpu", "host",
        "-cdrom", ISO, "-boot", "d",
        "-vga", "std", "-display", "none",
        "-serial", "file:%s" % SER,
        "-qmp", "unix:%s,server,nowait" % QMP,
    ]
    if want_disk:
        cmd += [
            "-device", "ich9-ahci,id=ahci0",
            "-drive", "id=ahcidisk0,file=%s,format=raw,if=none" % DISK,
            "-device", "ide-hd,drive=ahcidisk0,bus=ahci0.0",
        ]
        markers, wait_s = (MARKER_DISK,), 300
    else:
        markers, wait_s = MARKERS_DISKLESS, 45

    print("booting QEMU (SeaBIOS + std-vga, headless%s) ..."
          % (", AHCI disk" if want_disk else ""))
    qemu = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)

    rc = 1
    try:
        s = qmp_connect(QMP)
        s.recv(65536)                  # greeting
        qmp(s, "qmp_capabilities")

        t0 = time.time()
        while time.time() - t0 < wait_s:
            try:
                seen = open(SER, "rb").read().decode("latin1")
            except FileNotFoundError:
                seen = ""
            if any(m in seen for m in markers):
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
