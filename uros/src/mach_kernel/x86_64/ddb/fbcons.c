/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The other output (#568).  See fbcons.h for what this is and is not.
 */

#include <stdint.h>

#include <device/fbcons_font.h>
#include <ddb/cons.h>
#include <ddb/fbcons.h>
#include <pmap/pmap.h>
#include <time/tsc.h>

#define FONT_W		8
#define FONT_H		16

#define FB_FG		0x00ffffffu	/* white */
#define FB_BG		0x00000000u	/* black */

/*
 * The cell grid's bound, and why it is a bound rather than an allocation.
 *
 * 🔑 THE FRAMEBUFFER IS NEVER READ BACK, and this array is the whole reason.
 * Device memory is mapped uncached, and reads from it are punishingly slow --
 * a scroll done the obvious way, by moving pixels within the framebuffer,
 * reads the entire screen for every line and dominates the console's cost.
 * i386 measured exactly that in #371.  Linux reached the same place from the
 * other direction (`fbcon' prefers redrawing to moving pixels) and so did
 * FreeBSD (`vt' keeps a character buffer and redraws from it): three systems,
 * one conclusion.
 *
 * So what is kept is the CHARACTERS on screen, not their pixels: 64 KB that
 * is independent of the panel's resolution, where a pixel shadow would have to
 * be sized for the largest panel and could not be.  Static, because this has
 * to work before there is an allocator.
 *
 * 3840x2160 at 8x16 is 480 by 135, which is a 4K panel with nothing to spare
 * and everything smaller with room left.  A panel larger than this loses the
 * rows and columns past the bound rather than writing outside the array.
 */
#define FB_MAX_COLS	480
#define FB_MAX_ROWS	135

/*
 * ── The screen JUMPS, it does not slide (#568) ─────────────────────────
 *
 * 🔴 SCROLLING IS THE WHOLE COST OF THIS CONSOLE, and it is not close.  An
 * entry-14 boot drew 1.78 MILLION glyphs, of which eight thousand were the
 * initial clear: everything else was five hundred and forty-five scrolls, each
 * redrawing most of the screen so that it could move up by one row.
 *
 * A scroll by ONE row changes every row, so the work per printed line is a
 * whole screen.  A scroll by HALF a screen changes every row exactly as much,
 * for twenty-five printed lines instead of one.  The work per line falls by
 * the step, and the step costs nothing but the way it looks: the picture jumps
 * half a page instead of sliding, which is what a terminal called jump scroll
 * and what every early console did for the same reason.
 *
 * ⚠️ Nothing is lost by jumping.  After a jump the cursor sits in the middle of
 * the screen and the lines printed since are all still there; a machine that
 * stops shows everything from the last jump, plus the half above it.
 *
 * Two is the fraction and not eight, because the other half of a boot log is
 * usually what explains the line you are reading.
 */
#define FB_SCROLL_FRACTION	2

static struct mb2_framebuffer	fbcons_fb;	/* what the loader said */

static uint8_t		*fb_base;	/* mapped, or zero */
static unsigned		 fb_bytespp;
static unsigned		 fb_cols, fb_rows;
static unsigned		 fb_col, fb_row;
static unsigned		 fb_step;	/* rows the screen jumps by */
static int		 fb_ready;

/*
 * What is ON SCREEN right now, one byte a cell.
 *
 * Not "what has been printed": the difference is what makes the scroll cheap
 * below.  BSS, which the loader zeroes on this target, so every cell starts as
 * the code zero -- and fbcons_init() paints the screen black and fills this
 * with spaces before anything is drawn, so the two agree from the first glyph.
 */
static uint8_t		 fb_cell[FB_MAX_ROWS * FB_MAX_COLS];

/*
 * ⚠️ Advisory and unlocked, like the console's own counters and for the same
 * reason: this file takes no lock anywhere, deliberately, and a lost update
 * costs a number where a lock would cost the path that has to work when locks
 * do not.
 */
static uint64_t		 fb_cycles;
static uint64_t		 fb_glyphs;
static uint64_t		 fb_scrolls;

void fbcons_remember(const struct mb2_framebuffer *fb)
{
	fbcons_fb = *fb;
}

int fbcons_present(void)
{
	return fb_ready;
}

unsigned fbcons_cols(void)
{
	return fb_cols;
}

unsigned fbcons_rows(void)
{
	return fb_rows;
}

uint64_t fbcons_cycles(void)
{
	return fb_cycles;
}

uint64_t fbcons_glyphs(void)
{
	return fb_glyphs;
}

uint64_t fbcons_scrolls(void)
{
	return fb_scrolls;
}

/*
 * One glyph, straight into the framebuffer.
 *
 * The 32-bit path writes each row of the glyph as eight consecutive 32-bit
 * stores, which is the case every UEFI machine and this emulator produce.  It
 * is separate from the general one because consecutive stores are what a
 * write-combining buffer can coalesce, and a per-pixel call with a width
 * switch inside it cannot be coalesced by anything.
 *
 * ⚠️ Colours are white on black, so the channel order -- RGB against BGR --
 * does not matter: both are the same under any permutation.  That is why this
 * never reads the tag's colour information.
 */
static void render_glyph(uint8_t c, unsigned col, unsigned row)
{
	const unsigned char	*g = fbcons_font8x16[c];
	unsigned		 y0 = row * FONT_H;
	unsigned		 x0 = col * FONT_W;
	unsigned		 gy, gx;

	if (fb_bytespp == 4) {
		for (gy = 0; gy < FONT_H; gy++) {
			unsigned char	 bits = g[gy];
			uint32_t	*p;

			p = (uint32_t *)(fb_base + (uint64_t)(y0 + gy) *
					 fbcons_fb.pitch + (uint64_t)x0 * 4);

			for (gx = 0; gx < FONT_W; gx++)
				*p++ = (bits & (0x80 >> gx)) ? FB_FG : FB_BG;
		}
		return;
	}

	for (gy = 0; gy < FONT_H; gy++) {
		unsigned char	 bits = g[gy];
		uint8_t		*p;

		p = fb_base + (uint64_t)(y0 + gy) * fbcons_fb.pitch +
		    (uint64_t)x0 * fb_bytespp;

		for (gx = 0; gx < FONT_W; gx++) {
			uint32_t colour = (bits & (0x80 >> gx)) ? FB_FG : FB_BG;
			unsigned b;

			for (b = 0; b < fb_bytespp; b++)
				*p++ = (uint8_t)(colour >> (8 * b));
		}
	}
}

/* Draw it and record that it is what is on screen. */
static void draw_cell(uint8_t c, unsigned col, unsigned row)
{
	uint64_t t0 = rdtsc_ordered();

	render_glyph(c, col, row);
	fb_cell[row * FB_MAX_COLS + col] = c;
	fb_cycles += rdtsc_ordered() - t0;
	fb_glyphs++;
}

/*
 * Slide the screen up one row.
 *
 * 🔑 IT REDRAWS ONLY WHAT CHANGES, and on a console that is a small fraction
 * of it.  After a shift, row r must show what row r+1 showed; where those two
 * already agree -- which on a log screen is most of the width, because most of
 * a line is trailing spaces -- there is nothing to write.  The cell grid holds
 * what is on screen precisely so this comparison can be made without asking
 * the framebuffer, which cannot be asked cheaply.
 *
 * ⚠️ Top down, and that order is load-bearing: row r is written from row r+1,
 * which must still hold its old contents when r is done.  Going the other way
 * would smear the top line down the screen.
 */
static void fbcons_scroll(void)
{
	unsigned r, c;

	for (r = 0; r + fb_step < fb_rows; r++)
		for (c = 0; c < fb_cols; c++) {
			uint8_t want = fb_cell[(r + fb_step) * FB_MAX_COLS + c];

			if (want != fb_cell[r * FB_MAX_COLS + c])
				draw_cell(want, c, r);
		}

	for (r = fb_rows - fb_step; r < fb_rows; r++)
		for (c = 0; c < fb_cols; c++)
			if (fb_cell[r * FB_MAX_COLS + c] != ' ')
				draw_cell(' ', c, r);

	fb_row = fb_rows - fb_step;
	fb_scrolls++;
}

static void fbcons_newline(void)
{
	fb_col = 0;
	if (fb_row + 1 < fb_rows)
		fb_row++;
	else
		fbcons_scroll();	/* and it decides where the cursor lands */
}

void fbcons_putc(char ch)
{
	uint8_t c = (uint8_t)ch;

	if (!fb_ready)
		return;

	switch (c) {
	case '\r':
		fb_col = 0;
		return;
	case '\n':
		fbcons_newline();
		return;
	case '\b':
		if (fb_col > 0)
			fb_col--;
		return;
	case '\t':
		do {
			fbcons_putc(' ');
		} while ((fb_col & 7) != 0 && fb_col != 0);
		return;
	default:
		break;
	}

	/*
	 * Anything else below a space is a control code with no glyph.  Drawn
	 * as nothing rather than as the font's picture of it: the console's
	 * job here is to be readable, and a screen full of smiling faces from
	 * codepage 437 is how a bell becomes a bug report.
	 */
	if (c < 0x20)
		return;

	if (fb_col >= fb_cols)
		fbcons_newline();

	draw_cell(c, fb_col, fb_row);
	fb_col++;
}

void fbcons_init(void)
{
	uint64_t	va, size;
	unsigned	r, c;

	if (!fbcons_fb.present)
		return;

	/*
	 * 🔴 The depths this can draw, and a refusal that says so.  A mode
	 * with a depth nothing here handles is not a failure of the machine
	 * and must not be silent either: a console that quietly drew nothing
	 * would be indistinguishable from one that is broken, which is the
	 * whole failure mode this file exists to remove.
	 */
	fb_bytespp = fbcons_fb.bpp / 8;
	if (fb_bytespp < 2 || fb_bytespp > 4) {
		cons_printf("UrMach x86-64: fbcons: the loader's framebuffer "
			    "is %u bits a pixel, which this cannot draw into "
			    "— screen output is off, COM1 is unaffected "
			    "(#568)\n", (unsigned) fbcons_fb.bpp);
		return;
	}

	fb_cols = fbcons_fb.width / FONT_W;
	fb_rows = fbcons_fb.height / FONT_H;
	if (fb_cols > FB_MAX_COLS)
		fb_cols = FB_MAX_COLS;
	if (fb_rows > FB_MAX_ROWS)
		fb_rows = FB_MAX_ROWS;
	if (fb_cols == 0 || fb_rows == 0) {
		cons_printf("UrMach x86-64: fbcons: %ux%u is smaller than one "
			    "character — screen output is off (#568)\n",
			    fbcons_fb.width, fbcons_fb.height);
		return;
	}

	/*
	 * Map only the scanlines the console will use.  The panel may be
	 * taller than FB_MAX_ROWS characters, and mapping rows nothing will
	 * ever write is address space spent on nothing -- the device region is
	 * a bump allocator that never gives anything back.
	 */
	size = (uint64_t)fbcons_fb.pitch * (uint64_t)(fb_rows * FONT_H);
	va = pmap_map_device(fbcons_fb.addr, size);
	if (va == 0) {
		cons_printf("UrMach x86-64: fbcons: could not map the "
			    "framebuffer at %llx — screen output is off "
			    "(#568)\n",
			    (unsigned long long) fbcons_fb.addr);
		return;
	}
	fb_base = (uint8_t *)(uintptr_t)va;

	/*
	 * A black screen and a grid of spaces that agrees with it.
	 *
	 * ⚠️ Painted rather than assumed cleared.  Whatever the firmware or
	 * the loader left on the panel is still there, and a console that
	 * started writing over a GRUB menu would be legible only in the places
	 * it had reached.  Written through draw_cell()'s neighbour so the cost
	 * of the clear is counted like every other glyph: it is the largest
	 * single thing this console ever does, and a per-boot figure that
	 * silently left it out would be describing a different console.
	 */
	for (r = 0; r < fb_rows; r++)
		for (c = 0; c < fb_cols; c++)
			draw_cell(' ', c, r);

	fb_step = fb_rows / FB_SCROLL_FRACTION;
	if (fb_step == 0)
		fb_step = 1;

	fb_col = 0;
	fb_row = 0;
	fb_ready = 1;
}
