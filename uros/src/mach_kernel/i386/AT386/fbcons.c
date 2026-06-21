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
 * Performance: the framebuffer is device memory mapped uncached, and *reads*
 * from it are punishingly slow.  The naive scroll (bcopy framebuffer->framebuffer)
 * reads back the whole screen every line and dominated boot time.  Instead we
 * keep a RAM shadow of the framebuffer pixels: glyphs are written to both the
 * framebuffer and the shadow, and scrolling is done by sliding the shadow in
 * RAM and then a single bulk bcopy(shadow -> framebuffer).  The framebuffer is
 * therefore only ever *written*, in bulk, never read.
 *
 * Colours are monochrome white-on-black, so the pixel channel order (RGB vs
 * BGR) does not matter — white and black are identical under any permutation.
 */

#include <mach/vm_param.h>
#include <string.h>			/* bcopy, bzero */
#include <i386/io_map_entries.h>		/* io_map */
#include <i386/pmap.h>			/* pmap_pte, INTEL_PTE_NCACHE, flush_tlb */
#include "fbcons.h"
#include "fbcons_font.h"

#define FONT_W		8
#define FONT_H		16

#define FB_FG		0x00ffffffu	/* white */
#define FB_BG		0x00000000u	/* black */

/* RAM shadow of the on-screen pixels — the scroll source, so we never read the
 * uncached framebuffer back.  Static (so fbcons works at cninit, before the
 * kernel allocator exists); sized for a 1024x768x32 console.  On a larger
 * display the console is capped to the rows that fit here (top of the screen). */
#define FB_SHADOW_BYTES	(1024u * 768u * 4u)
static unsigned char	fb_shadow[FB_SHADOW_BYTES];

static unsigned char	*fb_base;	/* io_map'd framebuffer (kernel virt) */
static unsigned int	fb_pitch;	/* bytes per scanline                 */
static unsigned int	fb_bytespp;	/* bytes per pixel                    */
static unsigned int	fb_cols, fb_rows;	/* size in characters         */
static unsigned int	cur_col, cur_row;
static int		fb_active;

/* Opt-in: set by the '-f' boot argument (parse_arguments).  fbcons stays off
 * by default so the userspace gpu_server owns the display in normal operation;
 * '-f' makes the kernel console own the framebuffer instead (boot/panic/DDB
 * debugging on a pure-UEFI box with no serial port).  The two must not drive
 * the same framebuffer at once, so this is a deliberate either/or.
 *
 * Lives in .data (not BSS): parse_arguments() sets it before i386_init() clears
 * the BSS, so a BSS-resident flag would be wiped before fbcons_init() reads it
 * (same hazard as cons_is_com1 / mem_size, #337). */
int			fbcons_enabled __attribute__((section(".data"))) = 0;

static void
put_pixel(unsigned int x, unsigned int y, unsigned int color)
{
	unsigned int	off = y * fb_pitch + x * fb_bytespp;
	unsigned char	*p = fb_base + off;
	unsigned char	*s = fb_shadow + off;	/* shadow mirrors the framebuffer */

	switch (fb_bytespp) {
	case 4:
		*(unsigned int *)p = color;
		*(unsigned int *)s = color;
		break;
	case 3:
		p[0] = s[0] = (unsigned char)(color);
		p[1] = s[1] = (unsigned char)(color >> 8);
		p[2] = s[2] = (unsigned char)(color >> 16);
		break;
	case 2:
		*(unsigned short *)p = (unsigned short)color;
		*(unsigned short *)s = (unsigned short)color;
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
 * Scroll up one character row.  Slide the RAM shadow up one text row, clear the
 * freed row, then push the whole shadow to the framebuffer in one bulk write.
 * No framebuffer reads (those are the slow part); the bcopy's are forward
 * (dst < src) so the overlap is safe.
 */
static void
fb_scroll(void)
{
	unsigned int rowbytes   = FONT_H * fb_pitch;
	unsigned int movebytes  = (fb_rows - 1) * rowbytes;
	unsigned int totalbytes = fb_rows * rowbytes;

	bcopy((const char *)(fb_shadow + rowbytes), (char *)fb_shadow, movebytes);
	bzero((char *)(fb_shadow + movebytes), rowbytes);
	bcopy((const char *)fb_shadow, (char *)fb_base, totalbytes);
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

static void
fb_putchar(unsigned char c)
{
	draw_glyph(c, cur_col, cur_row);
	if (++cur_col >= fb_cols)
		fb_newline();
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
			fb_putchar(' ');
		} while (cur_col & 7);
		return;
	default:
		break;
	}

	fb_putchar(c);
}

void
fbcons_init(void)
{
	vm_size_t	size;
	unsigned int	max_rows;

	if (!fbcons_enabled)
		return;				/* opt-in via '-f'; gpu_server
						 * owns the display otherwise */
	if (!mb2_fb.present || mb2_fb.fb_type != 1 || mb2_fb.addr == 0)
		return;				/* no LFB, or EGA-text type */
	if (mb2_fb.bpp != 32 && mb2_fb.bpp != 24 && mb2_fb.bpp != 16)
		return;				/* unsupported pixel format */
	if (mb2_fb.width < FONT_W || mb2_fb.height < FONT_H)
		return;

	size       = (vm_size_t)mb2_fb.pitch * mb2_fb.height;
	fb_base    = (unsigned char *)io_map((vm_offset_t)mb2_fb.addr, size);

	/*
	 * Re-map the framebuffer write-back instead of uncached.  io_map()
	 * forces INTEL_PTE_NCACHE, which turns every pixel store into a
	 * separate uncached bus transaction — the dominant cost once the slow
	 * scroll-reads are gone.  Clearing NCACHE gives a write-back PTE: on
	 * real UEFI the firmware already marks the GOP aperture write-combining
	 * via MTRR, so WB-PTE-over-WC-MTRR resolves to WC (fast, and the GPU
	 * sees the writes); under QEMU it is plain cached, which is fast and
	 * stays coherent with the emulated scanout.  Only the BSP runs at
	 * cninit(), so a local TLB flush is enough (APs load the PTE fresh).
	 */
	{
		vm_offset_t va, end = (vm_offset_t)fb_base + round_page(size);
		for (va = (vm_offset_t)fb_base; va < end; va += PAGE_SIZE) {
			pt_entry_t *pte = pmap_pte(kernel_pmap, va);
			if (pte != PT_ENTRY_NULL)
				*pte &= ~INTEL_PTE_NCACHE;
		}
		flush_tlb();
	}

	fb_pitch   = mb2_fb.pitch;
	fb_bytespp = mb2_fb.bpp / 8;
	fb_cols    = mb2_fb.width  / FONT_W;
	fb_rows    = mb2_fb.height / FONT_H;

	/* Cap the console to the rows whose pixels fit in the static shadow. */
	max_rows = (unsigned int)(FB_SHADOW_BYTES / (FONT_H * fb_pitch));
	if (fb_rows > max_rows)
		fb_rows = max_rows;
	if (fb_rows == 0)
		return;				/* scanline wider than the shadow */

	cur_col = 0;
	cur_row = 0;

	bzero((char *)fb_shadow, sizeof(fb_shadow));
	bzero((char *)fb_base, size);		/* clear to black */
	fb_active = 1;
}
