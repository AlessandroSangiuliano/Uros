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
 * (osfmk/src/mach_kernel/i386/AT386/vga.c) is being retired in #199;
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

struct vga_priv {
	int		attached;
	volatile uint16_t *fb;		/* mapped 0xB8000 */
	unsigned int	cur_col;
	unsigned int	cur_row;
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

static void
vga_clear(struct vga_priv *p)
{
	unsigned int i;
	for (i = 0; i < VGA_COLS * VGA_ROWS; i++)
		p->fb[i] = VGA_CELL(' ', VGA_ATTR_DEFAULT);
	p->cur_col = 0;
	p->cur_row = 0;
}

static void
vga_scroll_one(struct vga_priv *p)
{
	/* Move rows [1..ROWS) one row up, blank the last row.
	 * memmove on the MMIO mapping is the fastest legal way: design
	 * doc §11.3 rule 3 (~320 µs / scroll on KVM, ~600 µs on HW). */
	memmove((void *)p->fb,
		(const void *)(p->fb + VGA_COLS),
		(size_t)(VGA_ROWS - 1) * VGA_COLS * sizeof(uint16_t));

	{
		unsigned int i;
		volatile uint16_t *last_row = p->fb + (VGA_ROWS - 1) * VGA_COLS;
		for (i = 0; i < VGA_COLS; i++)
			last_row[i] = VGA_CELL(' ', VGA_ATTR_DEFAULT);
	}
}

static void
vga_putc(struct vga_priv *p, char c)
{
	switch (c) {
	case '\n':
		p->cur_col = 0;
		p->cur_row++;
		break;
	case '\r':
		p->cur_col = 0;
		break;
	case '\t':
		p->cur_col = (p->cur_col + 8u) & ~7u;
		break;
	case '\b':
		if (p->cur_col > 0)
			p->cur_col--;
		break;
	default: {
		unsigned int off = p->cur_row * VGA_COLS + p->cur_col;
		if (off < VGA_COLS * VGA_ROWS)
			p->fb[off] = VGA_CELL(c, VGA_ATTR_DEFAULT);
		p->cur_col++;
		break;
	}
	}

	if (p->cur_col >= VGA_COLS) {
		p->cur_col = 0;
		p->cur_row++;
	}
	if (p->cur_row >= VGA_ROWS) {
		vga_scroll_one(p);
		p->cur_row = VGA_ROWS - 1;
	}
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
	vga_clear(p);

	printf("vga: text mode %ux%u mapped at uva=0x%08x\n",
	       VGA_COLS, VGA_ROWS, (unsigned int)uva);
	return 0;
}

static void
vga_detach(void *priv)
{
	struct vga_priv *p = (struct vga_priv *)priv;
	/* MIG device_mmio_unmap exists; releasing it on detach is
	 * cosmetically correct but the module is also tearing the
	 * gpu_server task down so leaving the page mapped is harmless.
	 * A future hotplug path will wire the unmap. */
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
vga_text_puts(void *priv, const char *buf, size_t len)
{
	struct vga_priv *p = (struct vga_priv *)priv;
	size_t i;

	if (!p->attached || p->fb == NULL)
		return -1;
	for (i = 0; i < len; i++)
		vga_putc(p, buf[i]);
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
};
