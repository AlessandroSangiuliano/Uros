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
# turned back into the exact glyph that produced it: not an OCR that might be
# wrong, an inversion of a table both sides share.  What comes back is compared
# against the serial log of the same boot.
#
# 🔑 AND SO IS WHAT EACH GLYPH IS (#661).  The console decodes the UTF-8 it is
# handed and draws the glyph the font's Unicode table names, from the same
# header; this parses that table too, reads every cell as the character its
# glyph is, and passes the serial log through the same table before comparing.
# An em dash is glyph 0xC4 on the panel, which the table says U+2500 is too:
# both sides read it as U+2500, and a line agrees when the screen shows what
# the wire said.  A character the font has no glyph for is drawn as ■ and
# stays itself on the wire's side, so that line disagrees: it does not read
# on the screen as on the wire.
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
#                              [--no-uart [--poll S]]
#                              [--cdrom ISO] [--all-lines [--poll S]]
#
# Exit:  0  the screen's text was found in the serial log
#        1  the screen disagreed with the log, or is blank
#        2  the experiment could not be run (no qemu, no monitor, no boot)
#
# 🔑 --no-uart IS THE MACHINE WITH NO SERIAL PORT (#670).  -serial none, and
# QEMU then puts no 16550 at COM1 (hw/char/serial-isa.c creates one only for
# a -serial it was given) and an empty port reads all ones (system/ioport.c,
# unassigned_io_read()): what pavillion, omen and OMEGA are.  There is no wire
# to wait on or to compare with, so --wait-for is looked for ON THE SCREEN:
# the machine runs --poll seconds, is paused, and its screen dumped and read;
# where the text is, it stays paused, which is --freeze.  A line the screen
# draws during -f's replay is there for a few tenths of a second, so a
# machine that went on running while each dump was read would scroll it away
# between two looks.  Exit 0 then means the --expect texts are on the screen.
#
# 🔑 --all-lines IS EVERY LINE OF A BOOT, not the last screenful (#661).  One
# screendump shows what is on the panel at the end, and the rows that scrolled
# away before it were never compared with anything.  So the screen is looked
# at every --poll seconds from the start, the machine paused while each look
# is read, as --no-uart does, until --wait-for is on the wire; every row ever
# seen is kept, each must be in the log, and each line of the log must have
# been seen, whole, on some look.  A line no look caught is named: drawn and
# scrolled away between two looks, or never drawn, which a shorter --poll
# tells apart.
#
# --cdrom boots an image instead of the harness's disk: the one made for bare
# metal (#595), whose GRUB has no serial terminal and whose kernel has -f, so
# that what the screen shows is what a machine with no serial port shows.  The
# serial line is still captured, for the comparison: the kernel writes COM1
# whether or not anything listens.
#
# ⚠️ One run at a time, with the harness's own lock: two qemus on one machine
# manufacture each other's failures (run-x86_64.sh says why).

import argparse, fcntl, os, re, socket, subprocess, sys, time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"

FONT_H = "uros/src/mach_kernel/device/fbcons_font.h"
# The kernel's first line, which boot.S says before anything else (#661).
KERNEL_FIRST = "UrMach x86-64: reached 32-bit multiboot entry"
GLYPH_W, GLYPH_H = 8, 16


def load_font(path):
    """The kernel's own table, parsed rather than reimplemented."""
    text = open(path).read()
    text = text[:text.index("fbcons_font_unicode[]")]
    rows = re.findall(r"\{((?:\s*0x[0-9a-fA-F]{2}\s*,?){16})\}", text)
    if len(rows) != 256:
        raise SystemExit(f"fbcons-readback: {path} gave {len(rows)} glyphs, "
                         f"expected 256 — the font's shape changed and this "
                         f"parser has to change with it")
    font = []
    for r in rows:
        font.append(tuple(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", r)))
    return font


def load_map(path):
    """What each glyph is (#661): the font's Unicode table, code point ->
    glyph, for every code point above ASCII.  Parsed, like the glyphs."""
    text = open(path).read()
    text = text[text.index("fbcons_font_unicode[]"):]
    table = {int(cp, 16): int(g, 16) for cp, g in
             re.findall(r"\{ 0x([0-9a-f]{4,6}), 0x([0-9a-f]{2}) \}", text)}
    if 0x2014 not in table:
        raise SystemExit(f"fbcons-readback: {path} gave {len(table)} code "
                         f"points and no em dash — the table's shape changed "
                         f"and this parser has to change with it")
    return table


def reading(font, table):
    """What each bitmap reads as, and how a character of the log is read.

    A bitmap is read as ASCII where ASCII draws it, else as the highest code
    point the table gives it -- U+2500 for 0xC4, which the em dash also is,
    and U+FFFD for 0xFE, the replacement -- and a blank one as a space.  A
    character of the log is read as the bitmap it would be drawn with, so
    both sides of the comparison go through the one table.  One with no glyph
    stays as it is, and matches nothing on the screen."""
    by_bitmap = {tuple([0] * GLYPH_H): " "}
    for g in range(0x20, 0x7F):
        by_bitmap.setdefault(tuple(font[g]), chr(g))
    for cp, g in sorted(table.items(), reverse=True):
        by_bitmap.setdefault(tuple(font[g]), chr(cp))
    for g in range(256):
        by_bitmap.setdefault(tuple(font[g]), chr(g))

    def fold(text):
        out = []
        for ch in text:
            o = ord(ch)
            g = o if 0x20 <= o <= 0x7F else table.get(o)
            out.append(ch if g is None else by_bitmap[tuple(font[g])])
        return "".join(out)
    return by_bitmap, fold


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
    # A dump cut short is refused here: lit_pixels() would pad it with dark
    # pixels, and its missing rows would read as a blank part of the screen.
    if len(data) - i < w * h * 3:
        raise SystemExit(f"fbcons-readback: the screendump holds "
                         f"{len(data) - i} bytes of pixels, not the "
                         f"{w * h * 3} of a {w}x{h} screen")
    return w, h, data[i : i + w * h * 3]


# White on black: a pixel is foreground when any of its channels is above
# 127.  The test is made by bytes.translate() on each channel and an OR of the
# three, in C, and not by a loop over every pixel: --no-uart reads the screen
# again and again while the machine is paused (#670).  On a 160x50 screen the
# loop took 0.29 s a dump and this takes 0.07 s, giving the same rows.
_LIT = bytes(0 if v <= 127 else 1 for v in range(256))
# Eight such flags, leftmost pixel first, back to the byte the font stores.
_PACK = {bytes((v >> (7 - i)) & 1 for i in range(8)): v for v in range(256)}


def lit_pixels(w, h, pix):
    """One byte per pixel, 1 where any channel is lit."""
    chans = [int.from_bytes(pix[k::3].translate(_LIT), "big") for k in range(3)]
    return (chans[0] | chans[1] | chans[2]).to_bytes(w * h, "big")


def cell_to_char(lit, w, col, row, by_bitmap):
    """Turn one 8x16 cell back into what its glyph is, or None."""
    bits = []
    for gy in range(GLYPH_H):
        o = (row * GLYPH_H + gy) * w + col * GLYPH_W
        bits.append(_PACK[lit[o : o + GLYPH_W]])
    return by_bitmap.get(tuple(bits))


def screen_lines(ppm, by_bitmap, mark="?"):
    w, h, pix = read_ppm(ppm)
    lit = lit_pixels(w, h, pix)
    # 🔴 AND THE BLANK GLYPH IS A SPACE, said explicitly, by reading().
    #
    # Several codes draw nothing at all, and the lowest of them is 0x00 -- so
    # taking the first match turned every untouched cell into a NUL, which
    # rstrip() does not strip and no log line contains.  Every row then failed
    # the comparison below for a reason that was entirely this script's: the
    # first shape of it reported fifty disagreements on a screen that was
    # correct.
    #
    # 🔑 CHARACTERS, not bytes (#661).  Until the console decoded UTF-8 an em
    # dash was three glyphs, one a byte, and a row was read back as bytes and
    # decoded; that made a screen showing "ÔÇö" agree with a wire saying "—",
    # and is why this script never saw it.  A cell is a glyph now, and a glyph
    # is what the table says it is.
    cols, rows = w // GLYPH_W, h // GLYPH_H
    out, unknown = [], 0
    for r in range(rows):
        line = []
        for c in range(cols):
            ch = cell_to_char(lit, w, c, r, by_bitmap)
            if ch is None:
                unknown += 1
                ch = mark
            line.append(ch)
        out.append("".join(line).rstrip())
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
    ap.add_argument("--freeze", action="store_true",
                    help="stop the machine as soon as --wait-for is seen, so "
                         "the screen is dumped as it was then: a screen that "
                         "goes on scrolling loses what it drew a moment ago "
                         "(#666)")
    ap.add_argument("--expect", action="append", default=[],
                    help="text that must be ON THE SCREEN; may be given more "
                         "than once.  Matching the serial log says the panel "
                         "and the wire agree, not that the panel shows a "
                         "given line (#666)")
    ap.add_argument("--no-uart", action="store_true",
                    help="boot with no UART at COM1 (-serial none), as on a "
                         "machine without a serial port: --wait-for is looked "
                         "for on the screen, with the machine paused while "
                         "each dump is read (#670)")
    ap.add_argument("--poll", type=float, default=0.05,
                    help="with --no-uart or --all-lines, the seconds the "
                         "machine runs between two looks at its screen")
    ap.add_argument("--all-lines", action="store_true",
                    help="look at the screen through the whole boot, until "
                         "--wait-for is on the wire, and require every row "
                         "seen to be in the log and every line of the log to "
                         "have been seen (#661)")
    ap.add_argument("--cdrom", default=None,
                    help="boot this image instead of the harness's disk -- "
                         "the bare-metal one (#595); --entry is then its "
                         "GRUB's business")
    a = ap.parse_args()
    if a.no_uart and a.send is not None:
        ap.error("--send types at the serial line, and --no-uart has none")
    if a.all_lines and (a.no_uart or a.send is not None or a.freeze):
        ap.error("--all-lines compares the whole boot with the serial line, "
                 "and is not given with --no-uart, --send or --freeze")

    build = os.path.realpath(a.build)
    out = a.out or build
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"fbcons-readback: no kernel at {kernel}", file=sys.stderr)
        return 2

    font = load_font(os.path.join(REPO, FONT_H))
    by_bitmap, fold = reading(font, load_map(os.path.join(REPO, FONT_H)))

    if a.cdrom is None:
        env = dict(os.environ, UROS_BUILD_DIR=build,
                   UROS_X86_64_BOOT_ENTRY=str(a.entry))
        subprocess.run([os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                       env=env, check=True,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    elif not os.path.exists(a.cdrom):
        print(f"fbcons-readback: no image at {a.cdrom}", file=sys.stderr)
        return 2

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

    if a.cdrom is not None:
        disks = ["-cdrom", os.path.realpath(a.cdrom), "-boot", "d"]
    else:
        disks = [
            "-drive", f"file={build}/disk-x86_64.img,if=none,id=urosdisk,"
                      f"format=raw",
            "-device", "virtio-blk-pci,drive=urosdisk,bootindex=0",
            "-device", "ich9-ahci,id=ahci0",
            "-drive", f"file={build}/disk-x86_64-ahci.img,if=none,"
                      f"id=ahcidisk0,format=raw",
            "-device", "ide-hd,drive=ahcidisk0,bus=ahci0.0,bootindex=1",
            "-drive", f"file={build}/disk-x86_64-ahci2.img,if=none,"
                      f"id=ahcidisk1,format=raw",
            "-device", "ide-hd,drive=ahcidisk1,bus=ahci0.1,bootindex=2"]
    argv = ["qemu-system-x86_64", "-cpu", "max", "-m", "512M",
            "-smp", str(a.smp)] + disks + [
            "-vga", "std", "-display", "none",
            "-serial", "none" if a.no_uart else f"pipe:{fifo}",
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

    def wait_for(text, budget, step=0.25):
        end = time.time() + budget
        while time.time() < end:
            if q.poll() is not None:
                pump()
                return text in pump()
            if text in pump():
                return True
            time.sleep(step)
        return False

    def hmp(m, cmd=None):
        """A monitor command, and its answer up to the next prompt: a
        screendump is on disk when the prompt comes back."""
        if cmd is not None:
            m.sendall(cmd.encode() + b"\n")
        buf = b""
        while not buf.rstrip().endswith(b"(qemu)"):
            chunk = m.recv(65536)
            if not chunk:
                raise OSError("the monitor closed")
            buf += chunk
        return buf

    def screen_wait_for(text, budget):
        """--no-uart: run, pause, read the screen; left paused where the
        text is (#670)."""
        end = time.time() + budget
        m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        m.settimeout(30.0)
        while True:
            try:
                m.connect(mon)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > end or q.poll() is not None:
                    return False
                time.sleep(0.05)
        try:
            hmp(m)				# the banner, up to the first prompt
            last = None
            while time.time() < end and q.poll() is None:
                time.sleep(a.poll)
                hmp(m, "stop")
                hmp(m, f"screendump {ppm}")
                raw = open(ppm, "rb").read()
                if raw != last:
                    last = raw
                    if any(fold(text) in l
                           for l in screen_lines(ppm, by_bitmap)[0]):
                        return True
                hmp(m, "cont")
            return False
        finally:
            m.close()

    def watch(text, budget):
        """--all-lines: run, pause, look, keeping every row seen, until
        text is on the wire (#661).  The wire is read while the machine is
        paused, before its screen is, so the look that finds the text has
        drawn it too: both are written at one place.

        🔑 Only once the wire has the kernel's first line.  Before it the
        panel is the firmware's and the loader's -- SeaBIOS's text mode,
        QEMU's own "Guest has not initialized the display" -- and the first
        shape of this read them as rows the wire never had.  The loader sets
        the kernel's graphics mode before it jumps, so from that line on the
        screen is black or the kernel's.  Not fbcons_open()'s line: it goes
        through the console's ring and can reach the wire after -f's replay
        has drawn and scrolled away the lines before it.

        🔑 And a row is judged only when two looks in a row show it.  A pause
        can fall inside a scroll, which rewrites the screen row by row, or
        inside a glyph: what that look shows is half of two lines, true for
        a moment and in no log.  So one look more is taken after the one
        that found the text, --poll later.

        🔴 The lines a look must have seen are the wire's up to that look,
        and no further: those after it were printed while nothing looked.
        The first shape of this let the machine run --settle seconds and
        looked once, and with the text in the middle of a boot it asked for
        a second and a half of lines that one look could not hold."""
        seen, stable, prev = {}, set(), set()
        looks, skipped, cols = 0, 0, None
        end = time.time() + budget
        m = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        m.settimeout(30.0)
        while True:
            try:
                m.connect(mon)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if time.time() > end or q.poll() is not None:
                    return False, seen, stable, looks, skipped, cols, 0
                time.sleep(0.05)

        def look():
            nonlocal prev, looks, cols
            hmp(m, f"screendump {ppm}")
            lines, _unk, cols, _rows = screen_lines(ppm, by_bitmap, "\0")
            looks += 1
            now = {l for l in lines if l.strip()}
            for l in now:
                seen.setdefault(l, looks)
            stable.update(now & prev)
            prev = now
            return now
        try:
            hmp(m)				# the banner, up to the first prompt
            while time.time() < end and q.poll() is None:
                time.sleep(a.poll)
                hmp(m, "stop")
                wire = pump()
                if KERNEL_FIRST not in wire:
                    skipped += 1
                    hmp(m, "cont")
                    continue
                look()
                if text in wire:
                    hmp(m, "cont")
                    time.sleep(a.poll)
                    hmp(m, "stop")
                    look()
                    return True, seen, stable, looks, skipped, cols, len(wire)
                hmp(m, "cont")
            return False, seen, stable, looks, skipped, cols, 0
        finally:
            m.close()

    def every_line():
        """--all-lines' verdict: every row two looks agreed on is in the
        log, and every line of the log was seen (#661)."""
        found, seen, stable, looks, skipped, cols, upto = watch(a.wait_for,
                                                                a.budget)
        raw = pump()
        open(log, "w").write(raw)
        if not found:
            print(f"fbcons-readback: never saw {a.wait_for!r} on the serial "
                  f"line in {a.budget:.0f}s, after {looks} looks at the "
                  f"screen — the boot did not get there", file=sys.stderr)
            return 2
        serial = fold(raw)
        unknown = [l for l in stable if "\0" in l]
        only = [l for l in stable if "\0" not in l and l.strip() not in serial]
        # Each line the wire finished, as the screen draws it: no escape
        # sequence (a terminal's, which this console does not have), no
        # control code but the tab, which is drawn as spaces to the next
        # stop of eight, and cut at the screen's width where it wraps.
        missed, nlines = [], 0
        looked = fold(raw[:upto])
        start = looked.find(KERNEL_FIRST)
        for w in looked[start:].split("\n")[:-1]:
            w = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", w)
            w = "".join(ch for ch in w if ch == "\t" or ord(ch) >= 0x20)
            w = w.expandtabs(8).rstrip()
            if not w:
                continue
            nlines += 1
            pieces = [w[i:i + cols].rstrip() for i in range(0, len(w), cols)]
            if not all(p == "" or p in seen for p in pieces):
                missed.append(w)
        print(f"fbcons-readback: --all-lines: {looks} looks at the screen, "
              f"the machine run {a.poll} s between two, after {skipped} "
              f"before the kernel's first line; {len(seen)} distinct rows "
              f"seen, {len(stable)} of them in two looks in a row, "
              f"{len(seen) - len(stable)} caught once (a scroll or a line "
              f"under way, or gone by the next look)")
        if unknown:
            print(f"fbcons-readback: {len(unknown)} rows two looks agreed on "
                  f"hold cells that are not a glyph of this font:")
            for l in unknown[:5]:
                print(f"  not a glyph | {l.replace(chr(0), '?')}")
        if only:
            print(f"fbcons-readback: {len(only)} of {len(stable)} rows two "
                  f"looks agreed on are NOT in the serial log — the screen "
                  f"and the wire disagree:")
            for l in only[:5]:
                print(f"  only on screen | {l}")
        if missed:
            print(f"fbcons-readback: {len(missed)} of {nlines} lines of the "
                  f"serial log were NEVER SEEN whole on the screen — drawn "
                  f"and scrolled away between two looks, or not drawn: a "
                  f"shorter --poll tells which:")
            for l in missed[:5]:
                print(f"  only on the wire | {l}")
        if only or missed or unknown:
            return 1
        print(f"fbcons-readback: PASS — every one of the {nlines} lines of the "
              f"serial log from the kernel's first was seen on the screen, "
              f"and every one of the {len(stable)} rows two looks agreed on "
              f"is in the log")
        return 0

    try:
        if a.all_lines:
            rc = every_line()
            return rc
        if a.no_uart:
            if not screen_wait_for(a.wait_for, a.budget):
                err = (q.stderr.read() if q.poll() is not None else "").strip()
                print(f"fbcons-readback: never saw {a.wait_for!r} on the "
                      f"screen in {a.budget:.0f}s — with no UART the screen is "
                      f"all there is, and it never drew that"
                      + (f": {err}" if err else ""), file=sys.stderr)
                return 2
            print(f"fbcons-readback: stopped the machine on {a.wait_for!r}, "
                  f"read off its screen: no UART (--no-uart)")
        elif not wait_for(a.wait_for, a.budget, 0.02 if a.freeze else 0.25):
            err = (q.stderr.read() if q.poll() is not None else "").strip()
            print(f"fbcons-readback: never saw {a.wait_for!r} on the serial "
                  f"line in {a.budget:.0f}s — that says nothing about the "
                  f"framebuffer, the boot did not get there"
                  + (f": {err}" if err else ""), file=sys.stderr)
            return 2

        if a.freeze and not a.no_uart:
            f = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            f.settimeout(10.0)
            f.connect(mon)
            f.recv(65536)
            f.sendall(b"stop\n")
            time.sleep(0.5)
            f.close()
            print(f"fbcons-readback: stopped the machine on {a.wait_for!r}")

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
        if not a.freeze and not a.no_uart:
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

        lines, unknown, cols, rows = screen_lines(ppm, by_bitmap)
        serial = fold(pump())
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

        absent = [t for t in a.expect if not any(fold(t) in l for l in lines)]
        if absent:
            print(f"fbcons-readback: {len(absent)} of {len(a.expect)} "
                  f"expected texts are NOT ON THE SCREEN:")
            for t in absent:
                print(f"  not on screen | {t}")
            return 1
        for t in a.expect:
            print(f"fbcons-readback: on screen as expected | {t}")

        if a.no_uart:
            print("fbcons-readback: PASS — read off the screen of a machine "
                  "with no UART; comparing it with a serial log is NOT "
                  "ASKED, there is none")
            return 0

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
