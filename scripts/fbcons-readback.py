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
#        1  the screen disagreed with the log -- a row the log does not have,
#           or, with --all-lines, a line of the log no look saw, or a kernel
#           whose -f is not the boot's -- or is blank, or never opened
#        2  the experiment could not be run, or compared nothing: no qemu,
#           no monitor, no boot, a monitor or a screendump that broke, an
#           entry grub.cfg does not have, a --wait-for found before the
#           screen existed; --all-lines is not given with --kvm either
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
# at every --poll seconds from the kernel's first line on the wire, the
# machine paused while each look is read, as --no-uart does, until --wait-for
# is on the wire; every row two looks in a row agreed on must be in the log,
# and each line of the log must have been seen, whole, on some look -- from
# the kernel's first line with -f, which draws what was printed before the
# screen existed, and without it from the line saying that went to COM1 only.
# A line no look caught is named: drawn and scrolled away between two looks,
# or never drawn, which a shorter --poll tells apart.  A line is matched by
# its text and not by where it was: one printed twice is credited by either
# copy and a wrapped tail by any equal row, so what is counted is distinct
# lines (#661's review).
#
# ⚠️ Under TCG.  The bare-metal image under KVM, paused every 10 ms from the
# kernel's first line, had drawn nothing on its screen after ten minutes;
# unpaused it reached the quiet census in seconds.  QEMU's and Linux's sources
# say why, though no run here has measured it: under KVM the 8254 lives in the
# host's kernel, and at every cont QEMU writes it back (KVM_SET_PIT2), which
# loads each channel's count again and starts it from nothing.  A countdown
# longer than one run slice never ends, and the kernel's self-tests wait on
# 8254 countdowns of 20 and 50 ms, with no bound, before the screen exists
# (#679).  TCG's 8254 counts on QEMU's virtual clock, which stops with the
# machine, so there a pause is invisible.
#
# --cdrom boots an image instead of the harness's disk: the one made for bare
# metal (#595), whose GRUB has no serial terminal and whose kernel has -f, so
# that what the screen shows is what a machine with no serial port shows.  The
# serial line is still captured, for the comparison: the kernel writes COM1
# whether or not anything listens.
#
# ⚠️ One run at a time, with the harness's own lock: two qemus on one machine
# manufacture each other's failures (run-x86_64.sh says why).

import argparse, fcntl, os, re, signal, socket, subprocess, sys, time

REPO = os.path.realpath(os.path.join(os.path.dirname(__file__), ".."))
LOCK = "/tmp/uros-x86_64-run.lock"

FONT_H = "uros/src/mach_kernel/device/fbcons_font.h"
# The kernel's first line, which boot.S says before anything else (#661).
KERNEL_FIRST = "UrMach x86-64: reached 32-bit multiboot entry"
GRUB_CFG = "uros/src/mach_kernel/x86_64/boot/grub.cfg"
# What the kernel says once the screen exists, with -f and without it (#666).
F_DRAWN = "fbcons: -f: the "
COM1_ONLY = "went to COM1 only; -f draws them here"


def as_drawn(line, cols=0):
    """A line of the wire as the console draws it: its rows on a screen
    `cols' wide (0, one row however long), by fbcons_put() and
    fbcons_draw()'s rules -- a glyph past the last column goes to the next
    row first, \\r back to the row's first column, \\b back one within the
    row, \\t spaces to the next stop of eight and at least one (across a
    wrap, to the stop on the new row), any other control code nothing
    (#661's review, four rounds)."""
    rows, col = [[]], 0

    def put(ch):
        nonlocal col
        if cols and col >= cols:
            rows.append([])
            col = 0
        row = rows[-1]
        row.extend(" " * (col + 1 - len(row)))
        row[col] = ch
        col += 1
    for ch in line:
        if ch == "\r":
            col = 0
        elif ch == "\b":
            if col > 0:
                col -= 1
        elif ch == "\t":
            put(" ")
            while col % 8 and col != 0:
                put(" ")
        elif ord(ch) >= 0x20:
            put(ch)
    return ["".join(r).rstrip() for r in rows]


# A kernel whose screen never opened says so, in one of these (#568, #567):
# the loader gave no framebuffer, fbcons_open() refused it, or a census --
# which runs only after fbcons_init() -- found none.  Not the halt's "no
# framebuffer was drawn on this boot": a kernel that dies before the screen
# exists says that too, and that is a boot that did not get there, not a
# screen that failed (#661's review, five rounds).
NEVER_OPENED = re.compile(r"[^\n]*(NO usable framebuffer from the loader|"
                          r"screen output is off|; no framebuffer \(#567\))"
                          r"[^\n]*")


def entry_has_f(entry):
    """Whether grub.cfg's entry `entry' (from 0) gives the kernel -f, read by
    bootarg.c's rule: a letter counts inside a word that begins with a dash.
    None if there is no such entry."""
    blocks = re.split(r"^menuentry ", open(os.path.join(REPO, GRUB_CFG)).read(),
                      flags=re.M)[1:]
    if entry < 0 or entry >= len(blocks):
        return None
    for line in blocks[entry].splitlines():
        words = line.split()
        if words[:1] == ["multiboot2"]:
            return any(w.startswith("-") and "f" in w[1:] for w in words[2:])
    return False
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
                         "two looks agreed on to be in the log and every line "
                         "of the log to have been seen (#661); under TCG, "
                         "the header says why")
    ap.add_argument("--cdrom", default=None,
                    help="boot this image instead of the harness's disk -- "
                         "the bare-metal one (#595); --entry is then its "
                         "GRUB's business")
    a = ap.parse_args()
    # 🔴 A TERM goes through the same way out as everything else, so the
    # lock, the fifos, the monitor's socket and qemu do not outlive the run;
    # killed without this, they did (#661's review, five rounds).
    signal.signal(signal.SIGTERM, lambda *_: sys.exit("stopped by SIGTERM"))
    if a.no_uart and a.send is not None:
        ap.error("--send types at the serial line, and --no-uart has none")
    if a.all_lines and (a.no_uart or a.send is not None or a.freeze):
        ap.error("--all-lines compares the whole boot with the serial line, "
                 "and is not given with --no-uart, --send or --freeze")
    if a.all_lines and a.kvm:
        ap.error("--all-lines stops the machine between looks, and under KVM "
                 "every resume starts the in-kernel 8254's countdowns again, "
                 "so a boot waiting on one never gets past it (the header's "
                 "warning, #679): run it under TCG")

    build = os.path.realpath(a.build)
    out = a.out or build
    kernel = os.path.join(build, "export/uros/boot/mach_kernel")
    if not os.path.exists(kernel):
        print(f"fbcons-readback: no kernel at {kernel}", file=sys.stderr)
        return 2

    # Nothing below has looked at a screen yet, so whatever breaks here is a
    # run that could not be made, 2 -- a parser's refusal, a grub.cfg that
    # cannot be read, a disk that could not be made -- and not 1, which says
    # the screen disagreed (#661's review, three rounds).
    try:
        font = load_font(os.path.join(REPO, FONT_H))
        by_bitmap, fold = reading(font, load_map(os.path.join(REPO, FONT_H)))
        # The bare-metal image's kernel has -f (#595); an entry says it in
        # grub.cfg, which make-disk-x86_64.sh writes as it is.
        want_f = True if a.cdrom is not None else entry_has_f(a.entry)
        if want_f is None:
            print(f"fbcons-readback: grub.cfg has no entry {a.entry}",
                  file=sys.stderr)
            return 2
        if a.cdrom is None:
            env = dict(os.environ, UROS_BUILD_DIR=build,
                       UROS_X86_64_BOOT_ENTRY=str(a.entry))
            subprocess.run(
                [os.path.join(REPO, "scripts/make-disk-x86_64.sh")],
                env=env, check=True,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except (OSError, SystemExit, ValueError,
            subprocess.CalledProcessError) as e:
        print(f"fbcons-readback: the experiment could not be set up: {e}",
              file=sys.stderr)
        return 2
    if a.cdrom is not None and not os.path.exists(a.cdrom):
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
    fd_out = fd_in = None

    def unstart(e):
        """🔴 A start that failed after the lock takes the lock and the
        fifos with it: left behind, the lock refused every later run of the
        harness until somebody removed it by hand (#661's review, three
        rounds)."""
        for fd in (fd_out, fd_in):
            if fd is not None:
                try:
                    os.close(fd)
                except OSError:
                    pass
        for path in (fifo + ".in", fifo + ".out"):
            try:
                os.unlink(path)
            except OSError:
                pass
        try:
            os.rmdir(LOCK)
        except OSError:
            pass
        try:
            print(f"fbcons-readback: the experiment could not start: {e}",
                  file=sys.stderr)
        except OSError:
            pass
        return 2

    try:
        for end in (".in", ".out"):
            try:
                os.unlink(fifo + end)
            except FileNotFoundError:
                pass
            os.mkfifo(fifo + end)
    except (OSError, SystemExit) as e:
        return unstart(e)
    log = os.path.join(out, "fbcons-readback.log")
    ppm = os.path.join(out, "fbcons-readback.ppm")
    for p in (log, ppm):
        try:
            os.unlink(p)
        except FileNotFoundError:
            pass
        except (OSError, SystemExit) as e:
            return unstart(e)

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
    try:
        fd_out = os.open(fifo + ".out", os.O_RDONLY | os.O_NONBLOCK)
        fcntl.fcntl(fd_out, 1031, 1 << 20)	# F_SETPIPE_SZ: a whole boot
        fd_in = os.open(fifo + ".in", os.O_RDWR | os.O_NONBLOCK)
        q = subprocess.Popen(argv, stdout=subprocess.DEVNULL,
                             stderr=subprocess.PIPE, text=True)
    except (OSError, ValueError, SystemExit,
            subprocess.SubprocessError) as e:
        return unstart(e)
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
        has drawn and scrolled away the lines before it.  Nor is the machine
        paused before it: there is nothing to look at, and under KVM a pause
        every 10 ms kept SeaBIOS and GRUB from ever handing over (20 minutes,
        49 MB of the guest's memory touched), where the same image booted
        unpaused in seconds: GRUB calibrates the TSC on a 55 ms countdown of
        the 8254, which every cont starts again, as the header's ⚠️ says of
        the kernel's (read in GRUB's sources, not measured).

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
                if KERNEL_FIRST not in pump():
                    skipped += 1
                    continue
                hmp(m, "stop")
                wire = pump()
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
        # The instrument breaking is not the screen disagreeing: a monitor
        # that closed or timed out, or a screendump cut short, is a run that
        # could not be made (#661's review).
        try:
            found, seen, stable, looks, skipped, cols, upto = watch(a.wait_for,
                                                                    a.budget)
        except (OSError, SystemExit, ValueError) as e:
            print(f"fbcons-readback: the looks could not go on: {e}",
                  file=sys.stderr)
            return 2
        raw = pump()
        open(log, "w").write(raw)
        if not found:
            # Waiting for a line only a screen prints, a screen that never
            # opened is still a blank screen (#661's review, four rounds).
            off = NEVER_OPENED.search(raw)
            if off:
                print(f"fbcons-readback: THE SCREEN NEVER OPENED — "
                      f"{off.group(0).strip()}")
                return 1
            print(f"fbcons-readback: never saw {a.wait_for!r} on the serial "
                  f"line in {a.budget:.0f}s, after {looks} looks at the "
                  f"screen — the boot did not get there", file=sys.stderr)
            return 2
        # As the screen draws it, on this side too (#661's review).
        serial = "\n".join(r for l in fold(raw).split("\n")
                           for r in as_drawn(l, cols))
        unknown = [l for l in stable if "\0" in l]
        only = [l for l in stable if "\0" not in l and l.strip() not in serial]
        # Each line the wire finished, as the screen draws it (as_drawn():
        # an escape is dropped and what follows it drawn, this console having
        # no terminal's sequences), and cut at the screen's width where it
        # wraps.
        missed, nlines, distinct = [], 0, set()
        looked = fold(raw[:upto])
        start = looked.find(KERNEL_FIRST)
        # 🔑 Whether the boot has -f is the caller's to say -- its grub.cfg
        # entry, or the bare-metal image, which has it -- and the kernel's
        # lines must agree: a kernel that lost -f says what went to COM1 only,
        # and taking that from the kernel passed it (#661's review, two
        # rounds).  With -f every line from the kernel's first is required;
        # without it, from the line that says what went to COM1 only.
        com1 = looked.find(COM1_ONLY, start)
        drawn_early = looked.find(F_DRAWN, start)
        if want_f and com1 >= 0:
            print(f"fbcons-readback: the boot gives the kernel -f, and the "
                  f"kernel says what it printed before the screen existed "
                  f"went to COM1 only — the screen never drew it")
            return 1
        if not want_f and drawn_early >= 0:
            print(f"fbcons-readback: the boot does not give the kernel -f, and "
                  f"the kernel says it drew what came before the screen")
            return 1
        if want_f and drawn_early < 0 or not want_f and com1 < 0:
            # A screen that never opened is a blank screen, not a look taken
            # too soon (#661's review, three rounds).
            off = NEVER_OPENED.search(raw)
            if off:
                print(f"fbcons-readback: THE SCREEN NEVER OPENED — "
                      f"{off.group(0).strip()}")
                return 1
            print(f"fbcons-readback: {a.wait_for!r} was on the wire before the "
                  f"screen existed, so nothing could be compared yet: wait for "
                  f"a line printed after it", file=sys.stderr)
            return 2
        if not want_f:
            start = looked.rfind("\n", 0, com1) + 1
        for w in looked[start:].split("\n")[:-1]:
            pieces = as_drawn(w, cols)
            if not any(pieces):
                continue
            nlines += 1
            distinct.add(tuple(pieces))
            if not all(p == "" or p in seen for p in pieces):
                missed.append(as_drawn(w)[0])
        if nlines == 0 and not only and not unknown:
            print(f"fbcons-readback: no line of the serial log was there to "
                  f"compare, up to {a.wait_for!r}: wait for a later line",
                  file=sys.stderr)
            return 2
        print(f"fbcons-readback: --all-lines: {looks} looks at the screen, "
              f"the machine run {a.poll} s between two, from the kernel's "
              f"first line ({skipped} polls before it); {len(seen)} distinct "
              f"rows seen, {len(stable)} of them in two looks in a row, "
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
        print(f"fbcons-readback: PASS — every one of the {len(distinct)} "
              f"distinct lines of the serial log ({nlines} lines) from "
              f"{'the line saying what went to COM1 only' if com1 >= 0 else 'the kernel' + chr(39) + 's first'} "
              f"was seen on the screen, a line printed more than once credited "
              f"by any of its copies, and every one of the {len(stable)} rows "
              f"two looks agreed on is in the log")
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
            off = NEVER_OPENED.search(fold(pump()))
            if off:
                print(f"fbcons-readback: THE SCREEN NEVER OPENED — "
                      f"{off.group(0).strip()}")
                return 1
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
        wire = fold(pump())
        serial = "\n".join(r for l in wire.split("\n")
                           for r in as_drawn(l, cols))
        drawn = [l for l in lines if l.strip()]

        print(f"fbcons-readback: {cols}x{rows} cells, {len(drawn)} non-blank "
              f"rows, {unknown} cells matched no glyph")
        for l in drawn[-6:]:
            print(f"  screen | {l}")

        if not drawn:
            print("fbcons-readback: THE SCREEN IS BLANK — the kernel printed "
                  f"{wire.count(chr(10))} lines to the serial line and drew "
                  f"none of them")
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
    # The instrument breaking, in any mode, is a run that could not be made,
    # not a screen that disagreed: a monitor closed or timed out, a
    # screendump cut short or not a PPM (#661's review, two rounds).
    except (OSError, SystemExit, ValueError) as e:
        print(f"fbcons-readback: the experiment broke: {e}", file=sys.stderr)
        rc = 2
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
