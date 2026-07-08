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
 * keep a RAM *character-cell* grid of the on-screen glyphs (#371): scrolling
 * slides that tiny grid up and redraws from it, so the framebuffer is only ever
 * *written*, never read.  (An earlier version shadowed every pixel, which had
 * to be sized for the largest panel and could not reach 4K within the ~16 MB
 * early boot map; the cell grid is resolution-independent and ~64 KB.)
 *
 * Colours are monochrome white-on-black, so the pixel channel order (RGB vs
 * BGR) does not matter — white and black are identical under any permutation.
 */

#include <mach/vm_param.h>
#include <string.h>			/* bcopy, bzero */
#include <i386/io_map_entries.h>		/* io_map */
#include <i386/pmap.h>			/* pmap_pte, INTEL_PTE_NCACHE, flush_tlb */
#include <i386/proc_reg.h>		/* rdmsr (#372 fb memtype probe) */
#include "fbcons.h"
#include "fbcons_font.h"

#define FONT_W		8
#define FONT_H		16

#define FB_FG		0x00ffffffu	/* white */
#define FB_BG		0x00000000u	/* black */

#define FB_PTE_PAT	0x00000080u	/* PAT bit in a 4K PTE (bit 7), #372 */

/* #371: character-cell scroll buffer — only the on-screen glyph codes, not the
 * pixels.  Static (so fbcons works at cninit, before the kernel allocator
 * exists) and resolution-independent: sized for a 4K panel at the base 8x16
 * font (3840/8 x 2160/16).  A panel larger than that is clamped to the top-left
 * that fits.  ~64 KB, vs the old 3 MB pixel shadow that a 4K console could not
 * grow past the early boot map. */
#define FB_MAX_COLS	480u
#define FB_MAX_ROWS	135u
static unsigned char	fb_char[FB_MAX_ROWS * FB_MAX_COLS];

static unsigned char	*fb_base;	/* io_map'd framebuffer (kernel virt) */
static unsigned int	fb_pitch;	/* bytes per scanline                 */
static unsigned int	fb_bytespp;	/* bytes per pixel                    */
static unsigned int	fb_cols, fb_rows;	/* size in characters         */
static unsigned int	fb_scale;	/* glyph magnification (1 or 2, #371) */
static unsigned int	cur_col, cur_row;
static int		fb_active;
static int		utf8_left;	/* UTF-8 continuation bytes still due (#372) */
static unsigned int	utf8_cp;	/* accumulated UTF-8 codepoint (#372)      */

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

/* #372: report the MTRR memory type covering the framebuffer aperture, to
 * settle whether it is uncached (UC) -- which makes every pixel store a
 * separate slow bus transaction and is the prime suspect for the crawling 4K
 * console -- or write-combining (WC).  One line at init; removed once the WC
 * fix lands.  Reads only the low 32 bits of each MSR (the aperture is < 4 GB). */
static void
fbcons_report_memtype(unsigned int phys)
{
	static const char *const nm[8] =
		{ "UC", "WC", "?2", "?3", "WT", "WP", "WB", "?7" };
	unsigned int cap_lo, hi, def_lo, i, vcnt, deftype, mtype = 8;

	rdmsr(0x0fe, &cap_lo, &hi);		/* IA32_MTRRCAP     */
	rdmsr(0x2ff, &def_lo, &hi);		/* IA32_MTRR_DEF_TYPE */
	vcnt    = cap_lo & 0xff;
	deftype = def_lo & 0x07;

	for (i = 0; i < vcnt; i++) {
		unsigned int base_lo, mask_lo;

		rdmsr(0x200 + 2 * i, &base_lo, &hi);	/* PHYSBASEi */
		rdmsr(0x201 + 2 * i, &mask_lo, &hi);	/* PHYSMASKi */
		if (!(mask_lo & 0x800))			/* V (valid) bit */
			continue;
		if ((phys & (mask_lo & 0xfffff000)) ==
		    (base_lo & (mask_lo & 0xfffff000))) {
			mtype = base_lo & 0x07;
			break;
		}
	}

	printf("#372 fbcons: fb phys=0x%x  MTRR=%s (default %s)  -> forcing WC via "
	       "PAT7\n", phys, (mtype < 8) ? nm[mtype] : "none", nm[deftype]);
}

/*
 * #372: make the framebuffer aperture write-combining (WC).
 *
 * The GOP aperture is covered by an MTRR of type UC (confirmed on hardware),
 * so a plain write-back PTE still resolves to UC and every 4-byte pixel store
 * is a separate uncached bus transaction -- the reason the 4K console crawls.
 * A UC MTRR cannot be overridden to WC by another MTRR (UC wins on overlap),
 * but it CAN by PAT: with PAT enabled the effective type is combine(MTRR,PAT),
 * and MTRR-UC + PAT-WC = WC (Intel SDM Table 11-7).  So repurpose IA32_PAT
 * entry 7 -- selected by a PTE with PAT+PCD+PWT set, default UC, and otherwise
 * unused -- as WC, and map the framebuffer through it.
 *
 * IA32_PAT is per-CPU; this runs on the BSP, which does essentially all console
 * drawing.  An AP that prints keeps UC for now (a rare path; extending this to
 * ap_machine_init is tracked in #372).  Follow the SDM cache-control change
 * sequence (CR0.CD=1, WBINVD, flush TLB, write MSR, WBINVD, flush TLB, restore).
 */
static void
fbcons_pat_enable_wc(void)
{
	unsigned int	lo, hi, cr0;
	unsigned long	eflags;

	__asm__ volatile("pushf ; pop %0 ; cli" : "=r" (eflags));

	cr0 = get_cr0();
	set_cr0((cr0 & ~CR0_NW) | CR0_CD);	/* no-fill cache mode */
	__asm__ volatile("wbinvd");
	flush_tlb();

	rdmsr(0x277, &lo, &hi);			/* IA32_PAT */
	hi = (hi & 0x00ffffffu) | 0x01000000u;	/* PA7 = 0x01 (WC) */
	wrmsr(0x277, lo, hi);

	__asm__ volatile("wbinvd");
	flush_tlb();
	set_cr0(cr0);				/* restore cache mode */

	__asm__ volatile("push %0 ; popf" : : "r" (eflags));
}

static void
put_pixel(unsigned int x, unsigned int y, unsigned int color)
{
	unsigned char	*p = fb_base + y * fb_pitch + x * fb_bytespp;

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

/* Blit one glyph to the framebuffer, magnified by fb_scale (#371): each font
 * pixel becomes an fb_scale x fb_scale block, so 4K text is drawn 2x.
 *
 * The 32bpp path (the common UEFI/GOP case) writes each glyph scanline as a run
 * of consecutive 32-bit stores straight into the mapped framebuffer — no
 * per-pixel function call or bytespp switch, and contiguous runs so the CPU's
 * write-combining buffers coalesce them.  At 4K/2x that is the difference
 * between a snappy console and a crawling one.  16/24bpp fall back to put_pixel. */
static void
render_glyph(unsigned char c, unsigned int col, unsigned int row)
{
	const unsigned char	*g = fbcons_font8x16[c];
	unsigned int		x0 = col * FONT_W * fb_scale;
	unsigned int		y0 = row * FONT_H * fb_scale;
	unsigned int		gy, gx, sy, sx;

	if (fb_bytespp == 4) {
		for (gy = 0; gy < FONT_H; gy++) {
			unsigned char bits = g[gy];

			for (sy = 0; sy < fb_scale; sy++) {
				unsigned int *p = (unsigned int *)
					(fb_base + (y0 + gy * fb_scale + sy) *
					 fb_pitch + x0 * 4);

				for (gx = 0; gx < FONT_W; gx++) {
					unsigned int color =
						(bits & (0x80 >> gx)) ? FB_FG : FB_BG;

					for (sx = 0; sx < fb_scale; sx++)
						*p++ = color;
				}
			}
		}
		return;
	}

	for (gy = 0; gy < FONT_H; gy++) {
		unsigned char bits = g[gy];
		for (gx = 0; gx < FONT_W; gx++) {
			unsigned int color = (bits & (0x80 >> gx)) ? FB_FG : FB_BG;

			for (sy = 0; sy < fb_scale; sy++)
				for (sx = 0; sx < fb_scale; sx++)
					put_pixel(x0 + gx * fb_scale + sx,
						  y0 + gy * fb_scale + sy,
						  color);
		}
	}
}

/* Render a glyph AND record it in the cell grid, so fb_scroll() can redraw the
 * screen without reading the framebuffer back. */
static void
draw_glyph(unsigned char c, unsigned int col, unsigned int row)
{
	fb_char[row * FB_MAX_COLS + col] = c;
	render_glyph(c, col, row);
}

/*
 * Scroll up one character row.  Slide the cell grid up one row in RAM, blank the
 * freed bottom row, then redraw the whole screen from the grid.  No framebuffer
 * reads (those are the slow part); the framebuffer is only written.
 */
static void
fb_scroll(void)
{
	unsigned int r, c;

	/* Forward copy (dst < src) so the row overlap is safe, like the old
	 * pixel-shadow slide; the kernel has bcopy but not memmove. */
	bcopy((const char *)&fb_char[FB_MAX_COLS], (char *)&fb_char[0],
	      (fb_rows - 1) * FB_MAX_COLS);
	memset(&fb_char[(fb_rows - 1) * FB_MAX_COLS], ' ', fb_cols);

	for (r = 0; r < fb_rows; r++)
		for (c = 0; c < fb_cols; c++)
			render_glyph(fb_char[r * FB_MAX_COLS + c], c, r);
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

	/* #372: the font is 8-bit but boot strings carry UTF-8 (e.g. the em-dash
	 * "—" = E2 80 94), which would render as three garbage glyphs ("OCo").
	 * Decode the sequence: map the en/em/horizontal-bar dashes to the CP437
	 * full-width line glyph 0xC4 (a real long dash), anything else to '-'. */
	if (c >= 0xc0) {			/* UTF-8 lead byte -- start a sequence */
		if (c >= 0xf0)		{ utf8_left = 3; utf8_cp = c & 0x07; }
		else if (c >= 0xe0)	{ utf8_left = 2; utf8_cp = c & 0x0f; }
		else			{ utf8_left = 1; utf8_cp = c & 0x1f; }
		return;
	}
	if (c >= 0x80) {			/* UTF-8 continuation byte */
		if (utf8_left == 0)
			return;			/* stray continuation, ignore */
		utf8_cp = (utf8_cp << 6) | (c & 0x3f);
		if (--utf8_left != 0)
			return;			/* more bytes still due */
		c = (utf8_cp == 0x2013 || utf8_cp == 0x2014 ||
		     utf8_cp == 0x2015) ? 0xc4 : '-';
		/* sequence complete -- fall through and render c */
	}

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
	 * #372: make the framebuffer write-combining (WC).  io_map() maps it
	 * INTEL_PTE_NCACHE (PCD), and the covering MTRR is UC, so every pixel
	 * store is a separate uncached bus transaction — the reason the 4K
	 * console crawls on real hardware (a plain write-back PTE does NOT help:
	 * MTRR-UC wins over a WB PTE).  Program PAT entry 7 as WC (above) and
	 * point the aperture's PTEs at it — PAT+PCD+PWT select PA7, and
	 * MTRR-UC + PAT-WC resolves to WC, so stores coalesce in the
	 * write-combining buffers.  Only the BSP runs at cninit(), so a local
	 * TLB flush is enough (APs load the PTE fresh).
	 */
	fbcons_pat_enable_wc();
	{
		vm_offset_t va, end = (vm_offset_t)fb_base + round_page(size);
		for (va = (vm_offset_t)fb_base; va < end; va += PAGE_SIZE) {
			pt_entry_t *pte = pmap_pte(kernel_pmap, va);
			if (pte != PT_ENTRY_NULL)
				*pte |= FB_PTE_PAT | INTEL_PTE_NCACHE |
					INTEL_PTE_WTHRU;
		}
		flush_tlb();
	}

	fb_pitch   = mb2_fb.pitch;
	fb_bytespp = mb2_fb.bpp / 8;

	/* #371: magnify glyphs on a wide panel so 4K text stays readable — the
	 * base 8x16 font at 3840 wide is 480 tiny columns.  Small panels keep 1x. */
	fb_scale   = (mb2_fb.width >= 2560) ? 2 : 1;

	fb_cols    = mb2_fb.width  / (FONT_W * fb_scale);
	fb_rows    = mb2_fb.height / (FONT_H * fb_scale);

	/* Clamp to the cell grid; a panel larger than 4K uses the top-left. */
	if (fb_cols > FB_MAX_COLS)
		fb_cols = FB_MAX_COLS;
	if (fb_rows > FB_MAX_ROWS)
		fb_rows = FB_MAX_ROWS;
	if (fb_cols == 0 || fb_rows == 0)
		return;

	cur_col = 0;
	cur_row = 0;

	memset((char *)fb_char, ' ', sizeof(fb_char));
	bzero((char *)fb_base, size);		/* clear to black */
	fb_active = 1;

	/* #372: now that fb_active is set this prints on-screen (OMEGA has no
	 * serial) -- tells us if the aperture is UC (slow) or WC. */
	fbcons_report_memtype((unsigned int) mb2_fb.addr);
}
