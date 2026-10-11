#!/usr/bin/env python3
# Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
# SPDX-License-Identifier: MIT
#
# gen-fbcons-font.py — regenerate the 8x16 console font used by the in-kernel
# emergency framebuffer console (i386/AT386/fbcons.c, #342).
#
# Reads a PC Screen Font (PSF1 or PSF2, 8x16) and emits the C glyph table
# device/fbcons_font.h (it was i386/AT386's until #568 gave x86-64 a framebuffer
# console too, and one table is one table).  The default source is the
# public-domain CP850 VGA
# font shipped by the kbd package; its codepage-437/850 layout matches what the
# kernel prints for ASCII and for the box-drawing/banner glyphs.
#
# #661: and the font's own Unicode table beside its glyphs, which is what makes
# a psfu a psfu: for every glyph, the code points it draws.  The console decodes
# the UTF-8 it is handed (device/fbcons_utf8.h) and asks this table which glyph
# a code point is; scripts/fbcons-readback.py parses the same table to read the
# screen back as characters.  Both come out of the one file the glyphs do, so
# the glyphs and what they are said to be cannot drift apart.
#
# 🔴 The whole header is this script's output, its comments included: what is
# in the tree is what this prints, and a hand edit there is lost the next time
# it runs.
#
# Usage:
#   scripts/gen-fbcons-font.py [PSF.gz] > uros/src/mach_kernel/device/fbcons_font.h

import gzip, struct, sys, os, unicodedata

DEFAULT = "/usr/share/kbd/consolefonts/cp850-8x16.psfu.gz"

# The glyph drawn for what the font has no glyph for, and for bytes that are
# not UTF-8.  ■ and not the font's own U+FFFD, which in this font is glyph 0x00
# and blank: a replacement nobody can see is a character dropped in silence.
REPLACEMENT = 0xFE

# Code points the font's table does not list, drawn with a glyph that reads as
# them.  The dashes are i386's choice since #372, and the reason this exists:
# the kernel and the tests print some twelve hundred em dashes and the font has
# none.  0xC4 is the box-drawing horizontal, eight pixels wide on row 7, where
# '-' is seven: it reads as a long dash, and stays distinguishable from a
# hyphen, so the screen keeps the difference the text makes.
ALIASES = {
    0x2013: 0xC4,       # EN DASH
    0x2014: 0xC4,       # EM DASH
    0x2015: 0xC4,       # HORIZONTAL BAR
    0xFFFD: REPLACEMENT,
}


def load_font(path):
    """The glyphs, their size, and the code points each glyph draws."""
    data = gzip.open(path, "rb").read() if path.endswith(".gz") else open(path, "rb").read()
    if data[:2] == b"\x36\x04":            # PSF1
        mode, charsize = data[2], data[3]
        nglyphs = 512 if (mode & 0x01) else 256
        glyphs = data[4:4 + nglyphs * charsize]
        if not mode & 0x02:
            sys.exit("%s has no Unicode table (PSF1 mode %#x)" % (path, mode))
        return glyphs, 8, charsize, psf1_table(data, 4 + nglyphs * charsize, nglyphs)
    if data[:4] == b"\x72\xb5\x4a\x86":    # PSF2
        (_v, hsize, flags, length, charsize, height, width) = struct.unpack("<IIIIIII", data[4:32])
        if not flags & 0x01:
            sys.exit("%s has no Unicode table (PSF2 flags %#x)" % (path, flags))
        glyphs = data[hsize:hsize + length * charsize]
        return glyphs, width, height, psf2_table(data, hsize + length * charsize, length)
    sys.exit("unknown PSF magic %r in %s" % (data[:4], path))


def psf1_table(data, off, n):
    """Per glyph, 16-bit code points up to 0xFFFF; 0xFFFE starts sequences,
    which are combinations of several code points and are left out."""
    table = []
    for _ in range(n):
        cps, seq = [], False
        while True:
            u = struct.unpack_from("<H", data, off)[0]
            off += 2
            if u == 0xFFFF:
                break
            if u == 0xFFFE:
                seq = True
            elif not seq:
                cps.append(u)
        table.append(cps)
    return table


def psf2_table(data, off, n):
    """Per glyph, UTF-8 code points up to 0xFF; 0xFE starts sequences, left out
    as in PSF1."""
    table = []
    for _ in range(n):
        cps, seq = [], False
        while True:
            b = data[off]
            if b == 0xFF:
                off += 1
                break
            if b == 0xFE:
                seq = True
                off += 1
                continue
            size = 1 if b < 0x80 else 2 if b < 0xE0 else 3 if b < 0xF0 else 4
            cp = ord(data[off:off + size].decode("utf-8"))
            off += size
            if not seq:
                cps.append(cp)
        table.append(cps)
    return table


def code_points(table):
    """Code point -> glyph, for every code point above ASCII.

    🔴 ASCII IS NOT IN IT, and is refused unless the font draws it in place:
    the console draws a byte below 0x80 as the glyph of the same number, and a
    font that put 'A' anywhere else would have it draw the wrong letter with no
    table in sight.  Where the table names a code point twice -- this one gives
    pilcrow and section sign both to 0x14/0x15, CP437's place, and to 0xF4/0xF5,
    CP850's -- the higher glyph is taken, CP850's own place."""
    owner = {}
    for g, cps in enumerate(table[:256]):
        for cp in cps:
            owner.setdefault(cp, []).append(g)
    for cp in range(0x20, 0x7F):
        if owner.get(cp) != [cp]:
            sys.exit("the font draws U+%04X with glyph(s) %s, not with glyph "
                     "0x%02x: the console draws ASCII in place and would show "
                     "the wrong character" % (cp, owner.get(cp), cp))
    out = {cp: max(gs) for cp, gs in owner.items() if cp >= 0x80}
    for cp, g in ALIASES.items():
        if cp in out and cp != 0xFFFD:
            sys.exit("U+%04X is in the font's own table (glyph 0x%02x): its "
                     "alias is not needed" % (cp, out[cp]))
        out[cp] = g
    return out


HEAD = """\
/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * ⚠️ MOVED OUT OF i386/AT386 (#568).  It lives here because x86-64 draws with
 * it too, and a second copy of a 25 KB table is a second copy that drifts.
 * It is machine-independent by nature -- 256 glyphs of bitmap -- so the only
 * thing that was ever i386 about it was the directory.
 *
 * 🔴 And it is reached by a path that NAMES this directory.  The kernel's
 * include roots put src/mach_kernel/i386 and i386/AT386 on the compiler's
 * search path for BOTH targets, which is the shadowing header-shadow-check.py
 * exists to track: a bare "fbcons_font.h" from the x86-64 tree would have
 * found the i386 copy and worked, and gone on working after somebody edited
 * one of the two.  <device/fbcons_font.h> cannot resolve anywhere else.
 *
 * Generated by scripts/gen-fbcons-font.py; edit that, not this.
 */

#ifndef _DEVICE_FBCONS_FONT_H_
#define _DEVICE_FBCONS_FONT_H_

/* 8x16 console font, codepage-437/850 layout (public-domain PC VGA font,
 * extracted from kbd %s).  256 glyphs, 16 bytes each, MSB=left. */
static const unsigned char fbcons_font8x16[256][16] = {
"""

MAP = """\
};

/*
 * What each glyph is, for every code point above ASCII the font draws (#661).
 *
 * The font's own Unicode table, which kbd ships beside the glyphs, plus the
 * aliases scripts/gen-fbcons-font.py lists for what the table leaves out --
 * the em dash first.  ASCII is not here: a byte below 0x80 is drawn as the
 * glyph of the same number, which the script refuses to emit a font without.
 * Sorted by code point, for device/fbcons_utf8.h to search.
 *
 * 🔑 One table, two readers: the console, which draws by it, and
 * scripts/fbcons-readback.py, which parses it to turn the screen back into
 * characters.  A glyph this says draws a code point is the glyph the reader
 * expects for it, because neither has a copy of its own.
 */
#define FBCONS_FONT_REPLACEMENT\t0x%02x\t/* %s, for what has no glyph */

static const struct fbcons_font_map {
\tunsigned int\tcp;
\tunsigned char\tglyph;
} fbcons_font_unicode[] = {
"""


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else DEFAULT
    glyphs, width, height, table = load_font(path)
    if (width, height) != (8, 16):
        sys.exit("expected an 8x16 font, got %dx%d" % (width, height))
    if len(table) < 256:
        sys.exit("expected 256 glyphs, got %d" % len(table))
    cs = 16
    out = [HEAD % os.path.basename(path).replace(".gz", "")]
    for g in range(256):
        rows = glyphs[g * cs:(g + 1) * cs].ljust(cs, b"\x00")
        out.append("  { %s }, /* 0x%02x */\n" % (",".join("0x%02x" % b for b in rows), g))
    out.append(MAP % (REPLACEMENT, unicodedata.name(chr(table[REPLACEMENT][0]))))
    for cp, g in sorted(code_points(table).items()):
        out.append("  { 0x%04x, 0x%02x }, /* %s */\n" %
                   (cp, g, unicodedata.name(chr(cp), "U+%04X" % cp)))
    out.append("};\n\n#endif\t/* _DEVICE_FBCONS_FONT_H_ */\n")
    sys.stdout.write("".join(out))


if __name__ == "__main__":
    main()
