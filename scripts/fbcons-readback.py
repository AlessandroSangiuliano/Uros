#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# fbcons-readback.py — did the kernel actually DRAW what it said? (#568)
#
# x86-64 grew a second output: the linear framebuffer the loader sets up,
# drawn into by x86_64/ddb/fbcons.c.  A screenshot proves nothing by itself --
# a picture of a blank screen and a picture of a broken blitter are both
# pictures -- so this does not look at the screen, it READS it.
#
# 🔑 THE FONT IS THE KEY, AND IT IS THE KERNEL'S OWN.  Every glyph is eight
# pixels by sixteen and comes from device/fbcons_font.h, which this script
# parses rather than reimplements.  So each 8x16 cell of the screendump can be
# turned back into the exact byte that produced it: not an OCR that might be
# wrong, an inversion of a table both sides share.  What comes back is compared
# against the serial log of the same boot.
#
# That makes the failures distinguishable, which is the whole point:
#
#   - a blank screen                 -> nothing was drawn
#   - cells that match no glyph      -> the blitter draws, but wrongly
#   - text that is not in the log    -> it draws something else
#   - text that IS in the log        -> the byte the kernel emitted reached
#                                       the panel, pixel for pixel
#
# ⚠️ -vga std, explicitly.  QEMU's -nographic hides the display without
# removing the device, so the ordinary dev loop already has a framebuffer; but
# a test whose subject is the framebuffer must say which one it asked for.
#
# Usage:
#   scripts/fbcons-readback.py [--build DIR] [--entry N] [--kvm] [--smp N]
#                              [--wait-for TEXT] [--budget S] [--out DIR]
#
# Exit:  0  the screen's text was found in the serial log
#        1  the screen disagreed with the log, or is blank
#        2  the experiment could not be run (no qemu, no monitor, no boot)
#
# ⚠️ One run at a time, with the harness's own lock: two qemus on one machine
# manufacture each other's failures (run-x86_64.sh says why).

import argparse, fcntl, os, re, socket, subprocess, sys, time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"

FONT_H = "uros/src/mach_kernel/device/fbcons_font.h"
GLYPH_W, GLYPH_H = 8, 16


def load_font(path):
    """The kernel's own table, parsed rather than reimplemented."""
    text = open(path).read()
    rows = re.findall(r"\{((?:\s*0x[0-9a-fA-F]{2}\s*,?){16})\}", text)
    if len(rows) != 256:
        raise SystemExit(f"fbcons-readback: {path} gave {len(rows)} glyphs, "
                         f"expected 256 — the font's shape changed and this "
                         f"parser has to change with it")
    font = []
    for r in rows:
        font.append(tuple(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", r)))
    return font


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit("fbcons-readback: the screendump is not a P6 PPM")
    # header: P6 <w> <h> <maxval>, whitespace separated, comments allowed
    fields, i = [], 2
    while len(fields) < 3:
        while i < len(data) and data[i : i + 1].isspace():
            i += 1
        if data[i : i + 1] == b"#":
            while data[i : i + 1] not in (b"\n", b""):
                i += 1
            continue
        j = i
        while j < len(data) and not data[j : j + 1].isspace():
            j += 1
        fields.append(int(data[i:j]))
        i = j
    i += 1
    w, h, _maxval = fields
    return w, h, data[i : i + w * h * 3]


def cell_to_byte(pix, w, col, row, font_index):
    """Turn one 8x16 cell back into the byte that drew it, or None."""
    bits = []
    for gy in range(GLYPH_H):
        b = 0
        base = ((row * GLYPH_H + gy) * w + col * GLYPH_W) * 3
        for gx in range(GLYPH_W):
            o = base + gx * 3
            # white on black: any lit channel is foreground.
            if pix[o] > 127 or pix[o + 1] > 127 or pix[o + 2] > 127:
                b |= 0x80 >> gx
        bits.append(b)
    return font_index.get(tuple(bits))


def screen_lines(ppm, font):
    w, h, pix = read_ppm(ppm)
    index = {}
    for code, g in enumerate(font):
        index.setdefault(tuple(g), code)

    # 🔴 AND THE BLANK GLYPH IS A SPACE, said explicitly.
    #
    # Several codes draw nothing at all, and the lowest of them is 0x00 -- so
    # taking the first match turned every untouched cell into a NUL, which
    # rstrip() does not strip and no log line contains.  Every row then failed
    # the comparison below for a reason that was entirely this script's: the
    # first shape of it reported fifty disagreements on a screen that was
    # correct.
    index[tuple([0] * GLYPH_H)] = 0x20
    cols, rows = w // GLYPH_W, h // GLYPH_H
    out, unknown = [], 0
    for r in range(rows):
        # 🔑 BYTES, NOT CHARACTERS, and only then decoded as UTF-8.
        #
        # The kernel writes UTF-8 and fbcons draws one CP437 glyph per BYTE, so
        # an em-dash is three glyphs on the panel.  Recovering a character cell
        # by cell would turn every one of them into mojibake, and the
        # comparison below would report a disagreement that is entirely this
        # script's.  Read back as the byte string it really is, a row decodes
        # to exactly what the kernel emitted.
        line = bytearray()
        for c in range(cols):
            code = cell_to_byte(pix, w, c, r, index)
            if code is None:
                unknown += 1
                line.append(0x3F)
            else:
                line.append(code)
        out.append(line.decode("utf-8", errors="replace").rstrip())
    return out, unknown, cols, rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default=os.environ.get(
        "UROS_BUILD_DIR", os.path.join(REPO, "uros/build-x86_64")))
    ap.add_argument("--entry", type=int, default=6)
    ap.add_argument("--kvm", action="store_true")
    ap.add_argument("--smp", type=int, default=1)
    ap.add_argument("--wait-for", default="boot_probe:",
                    help="serial text to wait for before the screendump")
    ap.add_argument("--send", default=None,
                    help="bytes to type at the console once --wait-for is "
                         "seen, before the screendump; \\x escapes allowed "
                         "(the debugger's door is \\x1c)")
    ap.add_argument("--then-wait-for", default=None,
                    help="serial text to wait for after --send")
    ap.add_argument("--budget", type=float, default=120.0)
    ap.add_argument("--settle", type=float, default=1.5,
                    help="seconds to let the machine run after the marker "
                         "(and after --send) before the screen is dumped; "
                         "the screen is where a kernel whose serial line "
                         "has stopped still says what it did (#538)")
    ap.add_argument("--out", default=None, help="where to leave the dump")
    a = ap.parse_args()

    build = os.path.realpath(a.build)
    out = a.out or build
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"fbcons-readback: no kernel at {kernel}", file=sys.stderr)
        return 2

    font = load_font(os.path.join(REPO, FONT_H))

    env = dict(os.environ, UROS_BUILD_DIR=build,
               UROS_X86_64_BOOT_ENTRY=str(a.entry))
    subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                   env=env, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    try:
        os.mkdir(LOCK)
    except FileExistsError:
        print(f"fbcons-readback: another run is in flight ({LOCK}) — refusing "
              f"to interleave", file=sys.stderr)
        return 2

    mon = f"/tmp/uros-fbcons-{os.getpid()}.mon"
    # 🔑 A PIPE AND NOT A FILE, because a screen that only ever shows a boot
    # tests one of the three places this console has to work.  The debugger's
    # prompt and a panic's last line are the other two, and reaching the first
    # of them means being able to TYPE at the machine.
    fifo = f"/tmp/uros-fbcons-{os.getpid()}.fifo"
    for end in (".in", ".out"):
        try:
            os.unlink(fifo + end)
        except FileNotFoundError:
            pass
        os.mkfifo(fifo + end)
    log = os.path.join(out, "fbcons-readback.log")
    ppm = os.path.join(out, "fbcons-readback.ppm")
    for p in (log, ppm):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass

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
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1,bootindex=2",
            "-vga", "std", "-display", "none",
            "-serial", f"pipe:{fifo}",
            "-monitor", f"unix:{mon},server=on,wait=off", "-no-reboot"]
    if a.kvm:
        argv.insert(1, "-enable-kvm")

    # The reader opens first and non-blocking, so neither side waits for the
    # other; qemu opens its ends O_RDWR and never waits either.
    fd_out = os.open(fifo + ".out", os.O_RDONLY | os.O_NONBLOCK)
    fcntl.fcntl(fd_out, 1031, 1 << 20)		# F_SETPIPE_SZ: a whole boot
    fd_in = os.open(fifo + ".in", os.O_RDWR | os.O_NONBLOCK)

    q = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                         stderr=subprocess.PIPE, text=True)
    rc = 2
    serial_raw = bytearray()

    def pump():
        try:
            while True:
                chunk = os.read(fd_out, 65536)
                if not chunk:
                    break
                serial_raw.extend(chunk)
        except BlockingIOError:
            pass
        except OSError:
            pass
        return serial_raw.decode(errors="replace")

    def wait_for(text, budget):
        end = time.time() + budget
        while time.time() < end:
            if q.poll() is not None:
                pump()
                return text in pump()
            if text in pump():
                return True
            time.sleep(0.25)
        return False

    try:
        if not wait_for(a.wait_for, a.budget):
            err = (q.stderr.read() if q.poll() is not None else "").strip()
            print(f"fbcons-readback: never saw {a.wait_for!r} on the serial "
                  f"line in {a.budget:.0f}s — that says nothing about the "
                  f"framebuffer, the boot did not get there"
                  + (f": {err}" if err else ""), file=sys.stderr)
            return 2

        if a.send is not None:
            time.sleep(1.0)
            os.write(fd_in, a.send.encode().decode("unicode_escape")
                     .encode("latin-1"))
            if a.then_wait_for and not wait_for(a.then_wait_for, 30.0):
                print(f"fbcons-readback: typed {a.send!r} and never saw "
                      f"{a.then_wait_for!r} — the machine did not answer, "
                      f"which says nothing about the framebuffer",
                      file=sys.stderr)
                return 2

        # A moment for the last lines to be drawn as well as sent -- or, with
        # --settle, long enough for a machine whose wire went quiet to write
        # whatever it is going to write on the screen alone.
        time.sleep(a.settle)
        open(log, "w").write(pump())

        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10.0)
        s.connect(mon)
        s.recv(65536)
        s.sendall(f"screendump {ppm}\n".encode())
        time.sleep(1.5)
        s.close()

        if not os.path.exists(ppm):
            print("fbcons-readback: the monitor produced no screendump",
                  file=sys.stderr)
            return 2

        lines, unknown, cols, rows = screen_lines(ppm, font)
        serial = pump()
        drawn = [l for l in lines if l.strip()]

        print(f"fbcons-readback: {cols}x{rows} cells, {len(drawn)} non-blank "
              f"rows, {unknown} cells matched no glyph")
        for l in drawn[-6:]:
            print(f"  screen | {l}")

        if not drawn:
            print("fbcons-readback: THE SCREEN IS BLANK — the kernel printed "
                  f"{len(serial)} bytes to the serial line and drew none of "
                  f"them")
            return 1
        if unknown:
            print(f"fbcons-readback: {unknown} cells hold something that is "
                  f"not a glyph of this font — the blitter draws, but not "
                  f"what the font says")
            return 1

        # Every row long enough to be distinctive must be somewhere in the log.
        checked = [l.strip() for l in drawn if len(l.strip()) >= 20]
        missing = [l for l in checked if l not in serial]
        if not checked:
            print("fbcons-readback: nothing on screen is long enough to look "
                  "for in the log — NOT ASKED, rerun somewhere the kernel "
                  "prints more")
            return 2
        if missing:
            print(f"fbcons-readback: {len(missing)} of {len(checked)} rows are "
                  f"NOT in the serial log — the screen and the wire disagree:")
            for l in missing[:5]:
                print(f"  only on screen | {l}")
            return 1

        print(f"fbcons-readback: PASS — all {len(checked)} distinctive rows "
              f"read back off the panel appear verbatim in the serial log")
        rc = 0
    finally:
        q.kill()
        q.wait()
        os.close(fd_out)
        os.close(fd_in)
        for path in (mon, fifo + ".in", fifo + ".out"):
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
        os.rmdir(LOCK)
    return rc


if __name__ == "__main__":
    sys.exit(main())
