/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * gpu_server/modules/vga.c — Legacy VGA text-mode back-end (#195).
 *
 * First concrete implementation of gpu_module_ops_t: single 80x25
 * text-mode display backed by the VGA framebuffer at physical
 * 0xB8000.  The kernel VGA driver
 * (uros/src/mach_kernel/i386/AT386/vga.c) is being retired in #199;
 * this module owns the same hardware from userspace.
 *
 * What this module does today:
 *   - probe()              — always claim (legacy VGA is always present
 *                            on i386 boards we target; HAL has no
 *                            non-PCI enumeration yet, see design §8.1).
 *   - attach()             — device_mmio_map(0xB8000, 4 KiB) into
 *                            our own task, clear the screen.
 *   - display_get()        — single fixed 80x25 text mode.
 *   - text_puts()          — write characters with internal cursor,
 *                            scroll-by-memmove on overflow per
 *                            design doc §11.3 rule 3.
 *
 * What this module does NOT do (deferred):
 *   - Scrollback, cursor positioning hardware register, color attrs
 *     beyond the default light-grey-on-black (#196 / text_render).
 *   - Buffer-object alloc / scanout / submit (text mode has no BOs).
 *   - Mode set (only one mode exists; trying to set anything else
 *     returns -1).
 *
 * Built as `vga.so` and shipped under `/mach_servers/modules/gpu/` in
 * the bootstrap bundle.  gpu_server discovers it at startup via
 * libmodload (`modload_load_class("gpu", "_module_ops", ...)`), which
 * dlopens every .so in the module pool and dlsyms the
 * `<basename>_module_ops` symbol — `vga_module_ops` here.  Same
 * dynamic-loading machinery already used by block_device_server
 * (#161) and hal_server (pci_scan.so).
 */

#include <mach.h>
#include <mach/kern_return.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#include <gpu/gpu_module_abi.h>
#include <gpu/gpu_types.h>

#include "../gpu_server.h"
#include "device_master.h"	/* MIG: device_mmio_map */

/* ============================================================
 * VGA constants
 * ============================================================ */

#define VGA_TEXT_PHYS		0xB8000u
#define VGA_TEXT_SIZE		0x1000u		/* one page covers 80x25*2 */
#define VGA_COLS		80u
#define VGA_ROWS		25u
#define VGA_ATTR_DEFAULT	0x07u		/* light grey on black */

#define VGA_CELL(c, attr)	(uint16_t)(((uint16_t)(attr) << 8) | (uint8_t)(c))

/* ============================================================
 * Module state — single instance.
 *
 * 0.1.0 ships exactly one VGA card per machine.  Multi-VGA setups (a
 * theoretical concern only on legacy SBCs) would require splitting
 * this state into a per-priv allocation; not worth the complication
 * today.
 * ============================================================ */

#define VGA_NSURFACES	4u			/* VT1..VT4 (#364) */
#define VGA_CELLS	(VGA_COLS * VGA_ROWS)
#define VGA_HIST_ROWS	128u			/* scrollback depth per surface */

/*
 * A virtual text surface (#204/#364): an off-screen 80x25 cell grid in
 * RAM plus its own cursor.  Writes always land in `cells`; the surface
 * that is `active` is additionally blitted to the VGA VRAM at 0xB8000,
 * so exactly one surface is on screen at a time.  A VT switch is a blit
 * of the target grid to VRAM — nothing is lost on the surfaces that
 * scroll off screen.
 */
struct vga_surface {
	uint16_t	cells[VGA_CELLS];	/* RAM shadow of the cell grid */
	unsigned int	cur_col;
	unsigned int	cur_row;
	uint64_t	scroll_count;		/* #203: scrolls on this surface */
	/* UTF-8 fold state (#364 polish): the VGA font is 8-bit CP437, so a
	 * multibyte UTF-8 char would paint as several garbage glyphs.  We
	 * decode across bytes and emit one ASCII glyph — utf8_left counts
	 * continuation bytes still expected, utf8_cp accumulates the code
	 * point. */
	unsigned int	utf8_left;
	uint32_t	utf8_cp;
	/* Scrollback (#364): rows that scroll off the top are pushed into a
	 * ring of VGA_HIST_ROWS lines.  view_off = how many rows the user
	 * has scrolled up from the live bottom (0 = showing live output);
	 * any new output snaps view_off back to 0. */
	uint16_t	hist[VGA_HIST_ROWS][VGA_COLS];
	unsigned int	hist_head;	/* ring write index (next slot) */
	unsigned int	hist_count;	/* valid rows in the ring (<= max) */
	unsigned int	view_off;	/* rows scrolled up from live */
};

struct vga_priv {
	int			attached;
	volatile uint16_t	*fb;		/* mapped VGA VRAM (0xB8000) */
	struct vga_surface	surf[VGA_NSURFACES];
	unsigned int		active;		/* surface currently on screen */
};

/* gpu_display_t is opaque outside core; the module owns its concrete
 * layout.  We only need .mode for query_displays. */
struct gpu_display {
	gpu_mode_t	mode;
};

static struct vga_priv     vga_priv_singleton;
static struct gpu_display  vga_display_singleton = {
	{ GPU_MODE_TEXT, VGA_COLS, VGA_ROWS, 0, 0, 0 }
};

/* ============================================================
 * Framebuffer helpers
 * ============================================================ */

/*
 * #201: hardware cursor sync.  The CRTC pair (0x3D4 index / 0x3D5 data)
 * holds the 16-bit "cursor location" at registers 0x0E (high) / 0x0F
 * (low).  Port-I/O privilege is granted via device_open("iopl") in
 * gpu_server/main.c — without it the outb triggers #GP and the kernel
 * SIGSEGVs us.  The MIG-less inline asm is the same trick char_server's
 * ps2/uart modules use.
 */
#define VGA_CRTC_INDEX	0x3D4
#define VGA_CRTC_DATA	0x3D5
#define VGA_CRTC_CURSOR_HIGH	0x0E
#define VGA_CRTC_CURSOR_LOW	0x0F

static inline void
vga_outb(uint16_t port, uint8_t v)
{
	__asm__ __volatile__ ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static void
vga_cursor_update(const struct vga_surface *s)
{
	uint16_t pos = (uint16_t)(s->cur_row * VGA_COLS + s->cur_col);
	vga_outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_HIGH);
	vga_outb(VGA_CRTC_DATA,  (uint8_t)(pos >> 8));
	vga_outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LOW);
	vga_outb(VGA_CRTC_DATA,  (uint8_t)(pos & 0xFF));
}

static void
vga_clear_surface(struct vga_surface *s)
{
	unsigned int i;
	for (i = 0; i < VGA_CELLS; i++)
		s->cells[i] = VGA_CELL(' ', VGA_ATTR_DEFAULT);
	s->cur_col = 0;
	s->cur_row = 0;
	s->utf8_left = 0;
	s->hist_head = 0;
	s->hist_count = 0;
	s->view_off = 0;
}

static void
vga_scroll_surface(struct vga_surface *s)
{
	/* The row about to fall off the top is preserved in the scrollback
	 * ring before we overwrite it. */
	memcpy(s->hist[s->hist_head], s->cells,
	       VGA_COLS * sizeof(uint16_t));
	s->hist_head = (s->hist_head + 1u) % VGA_HIST_ROWS;
	if (s->hist_count < VGA_HIST_ROWS)
		s->hist_count++;

	/* Move rows [1..ROWS) one row up in the RAM grid, blank the last
	 * row.  The blit to VRAM (for the active surface) happens once per
	 * write batch in vga_text_puts, not here. */
	memmove(s->cells, s->cells + VGA_COLS,
		(size_t)(VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));

	{
		unsigned int i;
		uint16_t *last_row = s->cells + (VGA_ROWS - 1) * VGA_COLS;
		for (i = 0; i < VGA_COLS; i++)
			last_row[i] = VGA_CELL(' ', VGA_ATTR_DEFAULT);
	}

	s->scroll_count++;
}

static uint64_t
vga_get_scroll_count(void *priv)
{
	const struct vga_priv *p = (const struct vga_priv *)priv;
	uint64_t total = 0;
	unsigned int i;

	for (i = 0; i < VGA_NSURFACES; i++)
		total += p->surf[i].scroll_count;
	return total;
}

/*
 * Blit a surface's RAM cell grid to the VGA VRAM and move the hardware
 * cursor to match.  Meaningful only for the active surface — called
 * once per write batch and on every VT switch.  memmove/memcpy on the
 * MMIO mapping is the fastest legal paint (design §11.3 rule 3).
 */
static void
vga_blit(struct vga_priv *p, unsigned int sid)
{
	struct vga_surface *s;

	if (p->fb == NULL || sid >= VGA_NSURFACES)
		return;
	s = &p->surf[sid];

	if (s->view_off == 0) {
		/* Live view: the cell grid is exactly what's on screen. */
		memcpy((void *)p->fb, s->cells, VGA_CELLS * sizeof(uint16_t));
		vga_cursor_update(s);
		return;
	}

	/*
	 * Scrolled back `view_off` rows: compose the visible 25-row window
	 * from the scrollback ring (older rows) and the live cell grid.
	 * Logical rows are [history 0..hist_count-1][live 0..24]; the window
	 * shows combined indices [hist_count-view_off .. +24].
	 */
	{
		uint16_t win[VGA_ROWS][VGA_COLS];
		unsigned int r;
		int base = (int)s->hist_count - (int)s->view_off;

		for (r = 0; r < VGA_ROWS; r++) {
			int ci = base + (int)r;

			if (ci >= (int)s->hist_count) {
				memcpy(win[r],
				       s->cells + (size_t)(ci - (int)s->hist_count)
						  * VGA_COLS,
				       VGA_COLS * sizeof(uint16_t));
			} else if (ci >= 0) {
				unsigned int hi = (s->hist_head + VGA_HIST_ROWS
						   - s->hist_count
						   + (unsigned int)ci)
						  % VGA_HIST_ROWS;
				memcpy(win[r], s->hist[hi],
				       VGA_COLS * sizeof(uint16_t));
			} else {
				unsigned int c;
				for (c = 0; c < VGA_COLS; c++)
					win[r][c] = VGA_CELL(' ', VGA_ATTR_DEFAULT);
			}
		}
		memcpy((void *)p->fb, win, VGA_CELLS * sizeof(uint16_t));
		/* Cursor is meaningless while scrolled back — leave it. */
	}
}

/*
 * Fold a decoded UTF-8 code point to a single CP437/ASCII glyph so the
 * 8-bit VGA font renders it legibly instead of as garbage bytes.
 * Unmapped non-ASCII becomes '?' (#364 polish).
 */
static char
utf8_fold(uint32_t cp)
{
	switch (cp) {
	case 0x2013: case 0x2014:	return '-';	/* en / em dash */
	case 0x2018: case 0x2019:	return '\'';	/* curly single quotes */
	case 0x201C: case 0x201D:	return '"';	/* curly double quotes */
	case 0x2022:			return '*';	/* bullet */
	case 0x2026:			return '.';	/* horizontal ellipsis */
	case 0x2190:			return '<';	/* left arrow */
	case 0x2192:			return '>';	/* right arrow */
	case 0x00A0:			return ' ';	/* no-break space */
	default:			return (cp < 0x80) ? (char)cp : '?';
	}
}

/* Low-level cell write: control chars + one printable glyph, with wrap
 * and scroll.  Called by vga_surf_putc after UTF-8 folding. */
static void
vga_emit(struct vga_surface *s, char c)
{
	switch (c) {
	case '\n':
		s->cur_col = 0;
		s->cur_row++;
		break;
	case '\r':
		s->cur_col = 0;
		break;
	case '\t':
		s->cur_col = (s->cur_col + 8u) & ~7u;
		break;
	case '\b':
		if (s->cur_col > 0)
			s->cur_col--;
		break;
	default: {
		unsigned int off = s->cur_row * VGA_COLS + s->cur_col;
		if (off < VGA_CELLS)
			s->cells[off] = VGA_CELL(c, VGA_ATTR_DEFAULT);
		s->cur_col++;
		break;
	}
	}

	if (s->cur_col >= VGA_COLS) {
		s->cur_col = 0;
		s->cur_row++;
	}
	if (s->cur_row >= VGA_ROWS) {
		vga_scroll_surface(s);
		s->cur_row = VGA_ROWS - 1;
	}
}

/*
 * UTF-8 -> CP437 folding front end (#364 polish).  Bytes < 0x80 pass
 * straight to vga_emit (control chars + ASCII).  Multibyte UTF-8 is
 * decoded across calls and emitted as a single folded glyph, so a stray
 * em-dash / arrow in a log line no longer paints garbage.
 */
static void
vga_surf_putc(struct vga_surface *s, char ch)
{
	uint8_t b = (uint8_t)ch;

	if (s->utf8_left > 0) {
		if ((b & 0xC0) == 0x80) {		/* continuation byte */
			s->utf8_cp = (s->utf8_cp << 6) | (b & 0x3Fu);
			if (--s->utf8_left == 0)
				vga_emit(s, utf8_fold(s->utf8_cp));
			return;
		}
		s->utf8_left = 0;	/* malformed — drop partial, reprocess b */
	}

	if (b < 0x80) {
		vga_emit(s, (char)b);
		return;
	}

	/* UTF-8 lead byte: set up the expected continuation length. */
	if ((b & 0xE0) == 0xC0) { s->utf8_left = 1; s->utf8_cp = b & 0x1Fu; }
	else if ((b & 0xF0) == 0xE0) { s->utf8_left = 2; s->utf8_cp = b & 0x0Fu; }
	else if ((b & 0xF8) == 0xF0) { s->utf8_left = 3; s->utf8_cp = b & 0x07u; }
	else vga_emit(s, '?');		/* stray continuation / invalid lead */
}

/* ============================================================
 * gpu_module_ops_t hooks
 * ============================================================ */

static void *
vga_probe(const struct hal_device_info *dev)
{
	(void)dev;	/* legacy VGA: HAL hint not used — see header */
	if (vga_priv_singleton.attached)
		return NULL;	/* single-instance guard */
	return &vga_priv_singleton;
}

static int
vga_attach(void *priv)
{
	struct vga_priv *p = (struct vga_priv *)priv;
	natural_t uva = 0;
	kern_return_t kr;

	kr = device_mmio_map(gpu_device_port,
			     VGA_TEXT_PHYS, VGA_TEXT_SIZE,
			     mach_task_self(), &uva);
	if (kr != KERN_SUCCESS) {
		printf("vga: device_mmio_map(0x%x) failed (kr=%d)\n",
		       VGA_TEXT_PHYS, kr);
		return -1;
	}

	p->fb = (volatile uint16_t *)(uintptr_t)uva;
	p->attached = 1;
	{
		unsigned int i;
		for (i = 0; i < VGA_NSURFACES; i++)
			vga_clear_surface(&p->surf[i]);
	}
	p->active = 0;
	vga_blit(p, 0);		/* paint the (blank) system console to VRAM */

	printf("vga: text mode %ux%u mapped at uva=0x%08x (%u surfaces)\n",
	       VGA_COLS, VGA_ROWS, (unsigned int)uva, VGA_NSURFACES);
	return 0;
}

static void
vga_detach(void *priv)
{
	struct vga_priv *p = (struct vga_priv *)priv;

	/* #201: release the 0xB8000 mapping.  Today detach only fires at
	 * task exit so the address space tears down anyway, but a future
	 * hot-unplug path can call this safely without leaking VA. */
	if (p->fb != NULL) {
		(void)device_mmio_unmap(gpu_device_port,
					(natural_t)(uintptr_t)p->fb,
					VGA_TEXT_SIZE,
					mach_task_self());
		p->fb = NULL;
	}
	p->attached = 0;
}

static uint32_t
vga_display_count(void *priv)
{
	(void)priv;
	return 1;
}

static gpu_display_t *
vga_display_get(void *priv, uint32_t idx)
{
	(void)priv;
	if (idx != 0)
		return NULL;
	return (gpu_display_t *)&vga_display_singleton;
}

static int
vga_display_set_mode(gpu_display_t *d, const gpu_mode_t *m)
{
	(void)d;
	if (m == NULL)
		return -1;
	/* Only the native 80x25 text mode is supported. */
	if (m->kind != GPU_MODE_TEXT ||
	    m->width != VGA_COLS || m->height != VGA_ROWS)
		return -1;
	return 0;
}

static int
vga_display_scanout(gpu_display_t *d, gpu_bo_t *bo)
{
	(void)d; (void)bo;
	/* In text mode the "scanout" IS the cell grid: there is no
	 * separate buffer object to flip.  Accept silently so a
	 * compositor that calls scanout(NULL) on the implicit text
	 * surface still works. */
	return 0;
}

static int
vga_text_puts(void *priv, uint32_t surface, const char *buf, size_t len)
{
	struct vga_priv *p = (struct vga_priv *)priv;
	struct vga_surface *s;
	size_t i;

	if (!p->attached || p->fb == NULL)
		return -1;
	if (surface >= VGA_NSURFACES)
		surface = 0;		/* clamp unknown surfaces to the console */
	s = &p->surf[surface];

	s->view_off = 0;		/* new output snaps the view to the bottom */

	for (i = 0; i < len; i++)
		vga_surf_putc(s, buf[i]);

	/* Only the on-screen surface touches VRAM; background VTs just
	 * accumulate in their RAM grid until switched to.  Blitting once
	 * per batch (not per char) also syncs the HW cursor (#201). */
	if (surface == p->active)
		vga_blit(p, surface);
	return 0;
}

static uint32_t
vga_text_surface_count(void *priv)
{
	(void)priv;
	return VGA_NSURFACES;
}

static int
vga_text_set_active(void *priv, uint32_t surface)
{
	struct vga_priv *p = (struct vga_priv *)priv;

	if (!p->attached || p->fb == NULL)
		return -1;
	if (surface >= VGA_NSURFACES)
		return -1;
	p->active = surface;
	vga_blit(p, surface);		/* repaint VRAM from the target grid */
	return 0;
}

/*
 * Scroll the on-screen surface's view by `delta` rows through its
 * scrollback (delta > 0 = up into history, < 0 = back down toward live).
 * Clamped to [0, hist_count]; repaints VRAM from the composed window.
 */
static int
vga_text_scroll(void *priv, int delta)
{
	struct vga_priv *p = (struct vga_priv *)priv;
	struct vga_surface *s;
	int off;

	if (!p->attached || p->fb == NULL)
		return -1;
	s = &p->surf[p->active];

	off = (int)s->view_off + delta;
	if (off < 0)
		off = 0;
	if (off > (int)s->hist_count)
		off = (int)s->hist_count;
	s->view_off = (unsigned int)off;

	vga_blit(p, p->active);
	return 0;
}

/* ============================================================
 * Exported entry point.
 *
 * libmodload locates this symbol by concatenating the .so basename
 * with the suffix gpu_server passes to `modload_load_class`
 * (`"_module_ops"`).  For `vga.so` that gives `vga_module_ops`.
 * ============================================================ */

const gpu_module_ops_t vga_module_ops = {
	.name             = "vga",
	.abi_version      = GPU_MODULE_ABI_VERSION,
	.priority         = 0,
	.probe            = vga_probe,
	.attach           = vga_attach,
	.detach           = vga_detach,
	.display_count    = vga_display_count,
	.display_get      = vga_display_get,
	.display_set_mode = vga_display_set_mode,
	.display_scanout  = vga_display_scanout,
	.bo_alloc         = NULL,	/* text mode has no BOs */
	.bo_free          = NULL,
	.bo_map           = NULL,
	.submit           = NULL,	/* no command submission in 0.1.0 */
	.text_puts        = vga_text_puts,
	.get_scroll_count = vga_get_scroll_count,
	.text_surface_count = vga_text_surface_count,
	.text_set_active  = vga_text_set_active,
	.text_scroll      = vga_text_scroll,
};
