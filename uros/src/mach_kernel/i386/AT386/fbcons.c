/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * fbcons.c — emergency in-kernel framebuffer console (#342).
 *
 * Pure-UEFI machines have no serial port and no legacy VGA text buffer
 * (0xB8000), so the normal console paths — cnputc -> com_putc, and the
 * userspace gpu_server that drives 0xB8000 — draw nothing on real hardware.
 * The one thing the firmware leaves behind is a linear "GOP" framebuffer; GRUB
 * reports its address through the multiboot2 framebuffer tag, which mb2_parse()
 * stashes in mb2_fb.  This file blits an 8x16 bitmap font straight into that
 * framebuffer so boot and panic output is visible on metal.
 *
 * It is deliberately dependency-free: no locks, no allocation after init, no
 * interrupts, no scheduler — so it keeps working inside the debugger and during
 * a panic, when userspace (and much of the kernel) may be dead.  It is NOT a
 * display driver: the normal graphics stack still belongs to the userspace
 * gpu_server.  cnputc() simply mirrors every byte here in addition to COM1.
 *
 * Colours are monochrome white-on-black, so the pixel channel order (RGB vs
 * BGR) does not matter — white and black are identical under any permutation.
 */

#include <mach/vm_param.h>
#include <string.h>			/* bcopy, bzero */
#include <i386/io_map_entries.h>		/* io_map */
#include "fbcons.h"
#include "fbcons_font.h"

#define FONT_W		8
#define FONT_H		16

#define FB_FG		0x00ffffffu	/* white */
#define FB_BG		0x00000000u	/* black */

static unsigned char	*fb_base;	/* io_map'd framebuffer (kernel virt) */
static unsigned int	fb_pitch;	/* bytes per scanline                 */
static unsigned int	fb_bytespp;	/* bytes per pixel                    */
static unsigned int	fb_cols, fb_rows;	/* size in characters         */
static unsigned int	cur_col, cur_row;
static int		fb_active;

static void
put_pixel(unsigned int x, unsigned int y, unsigned int color)
{
	unsigned char *p = fb_base + y * fb_pitch + x * fb_bytespp;

	switch (fb_bytespp) {
	case 4:
		*(unsigned int *)p = color;
		break;
	case 3:
		p[0] = (unsigned char)(color);
		p[1] = (unsigned char)(color >> 8);
		p[2] = (unsigned char)(color >> 16);
		break;
	case 2:
		*(unsigned short *)p = (unsigned short)color;
		break;
	default:
		break;
	}
}

static void
draw_glyph(unsigned char c, unsigned int col, unsigned int row)
{
	const unsigned char	*g = fbcons_font8x16[c];
	unsigned int		x0 = col * FONT_W;
	unsigned int		y0 = row * FONT_H;
	unsigned int		gy, gx;

	for (gy = 0; gy < FONT_H; gy++) {
		unsigned char bits = g[gy];
		for (gx = 0; gx < FONT_W; gx++)
			put_pixel(x0 + gx, y0 + gy,
				  (bits & (0x80 >> gx)) ? FB_FG : FB_BG);
	}
}

/*
 * Shift the whole text area up by one character row and clear the freed last
 * row.  bcopy() is a forward (cld; rep movs) copy and the destination is below
 * the source, so the overlap is safe.
 */
static void
fb_scroll(void)
{
	unsigned int rowbytes  = FONT_H * fb_pitch;
	unsigned int movebytes = (fb_rows - 1) * rowbytes;

	bcopy((const char *)(fb_base + rowbytes), (char *)fb_base, movebytes);
	bzero((char *)(fb_base + movebytes), rowbytes);
}

static void
fb_newline(void)
{
	cur_col = 0;
	if (++cur_row >= fb_rows) {
		fb_scroll();
		cur_row = fb_rows - 1;
	}
}

void
fbcons_putc(char ch)
{
	unsigned char c = (unsigned char)ch;

	if (!fb_active)
		return;

	switch (c) {
	case '\n':
		fb_newline();
		return;
	case '\r':
		cur_col = 0;
		return;
	case '\b':
		if (cur_col)
			cur_col--;
		return;
	case '\t':
		do {
			draw_glyph(' ', cur_col, cur_row);
			if (++cur_col >= fb_cols) {
				fb_newline();
				break;
			}
		} while (cur_col & 7);
		return;
	default:
		break;
	}

	draw_glyph(c, cur_col, cur_row);
	if (++cur_col >= fb_cols)
		fb_newline();
}

void
fbcons_init(void)
{
	vm_size_t	size;

	if (!mb2_fb.present || mb2_fb.fb_type != 1 || mb2_fb.addr == 0)
		return;				/* no LFB, or EGA-text type */
	if (mb2_fb.bpp != 32 && mb2_fb.bpp != 24 && mb2_fb.bpp != 16)
		return;				/* unsupported pixel format */
	if (mb2_fb.width < FONT_W || mb2_fb.height < FONT_H)
		return;

	size       = (vm_size_t)mb2_fb.pitch * mb2_fb.height;
	fb_base    = (unsigned char *)io_map((vm_offset_t)mb2_fb.addr, size);
	fb_pitch   = mb2_fb.pitch;
	fb_bytespp = mb2_fb.bpp / 8;
	fb_cols    = mb2_fb.width  / FONT_W;
	fb_rows    = mb2_fb.height / FONT_H;
	cur_col    = 0;
	cur_row    = 0;

	bzero((char *)fb_base, size);		/* clear to black */
	fb_active = 1;
}
