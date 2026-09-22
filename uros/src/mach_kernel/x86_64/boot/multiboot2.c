/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Reading the multiboot2 handoff (#406/#407).
 */

#include <stdint.h>

#include <mach/boolean.h>

#include <boot/multiboot2.h>

const struct mb2_tag *mb2_find_tag(uint32_t info_pa, uint32_t type)
{
	const uint8_t *base = (const uint8_t *)(uintptr_t)info_pa;
	uint32_t total;
	uint32_t off = 8;			/* past total_size + reserved */

	if (info_pa == 0)
		return 0;

	total = *(const uint32_t *)base;

	while (off + sizeof(struct mb2_tag) <= total) {
		const struct mb2_tag *tag;

		tag = (const struct mb2_tag *)(base + off);
		if (tag->type == MB2_TAG_END)
			return 0;
		if (tag->type == type)
			return tag;

		/*
		 * A tag shorter than its own header would leave the offset
		 * standing still — refuse rather than loop forever on a
		 * malformed structure we did not write.
		 */
		if (tag->size < sizeof(struct mb2_tag))
			return 0;

		off += (tag->size + 7) & ~7u;	/* tags are 8-byte aligned */
	}

	return 0;
}

/*
 * What display the loader left, if any (#568).
 *
 * 🔴 EVERY FIELD COMES FROM THE TAG.  A framebuffer's pitch is not its width
 * times its depth -- a mode is free to pad each scanline, and firmware often
 * does -- so a console that computed the stride would draw a staircase on the
 * machines that matter and a correct picture on the emulator that does not.
 * The same goes for the address: it is 64-bit in the tag, and truncating it
 * is how a console works everywhere except the machine it was written for.
 *
 * Only a direct-RGB mode is reported as present.  An EGA text tag describes
 * 0xB8000 and character cells, which is a different thing to write and not
 * the thing the caller of this is built to draw into; saying `present' for it
 * would hand a glyph blitter a buffer that is not pixels.
 */
boolean_t mb2_framebuffer(uint32_t info_pa, struct mb2_framebuffer *out)
{
	const struct mb2_tag_framebuffer *fb;

	out->addr = 0;
	out->pitch = 0;
	out->width = 0;
	out->height = 0;
	out->bpp = 0;
	out->fb_type = 0;
	out->present = 0;

	fb = (const struct mb2_tag_framebuffer *)
		mb2_find_tag(info_pa, MB2_TAG_FRAMEBUFFER);
	if (fb == 0 || fb->size < sizeof(*fb))
		return FALSE;

	out->addr = fb->addr;
	out->pitch = fb->pitch;
	out->width = fb->width;
	out->height = fb->height;
	out->bpp = fb->bpp;
	out->fb_type = fb->fb_type;

	/*
	 * And a mode with no pixels in it is not a framebuffer, whatever the
	 * tag says.  A zero width, height, pitch or depth would make every
	 * later calculation divide by nothing or draw outside itself, and the
	 * caller's one test is `present'.
	 */
	if (fb->fb_type != MB2_FB_TYPE_RGB ||
	    fb->width == 0 || fb->height == 0 ||
	    fb->pitch == 0 || fb->bpp == 0)
		return FALSE;

	out->present = 1;
	return TRUE;
}

/*
 * Walk the memory-map entries, calling back with each available region.
 * entry_size comes from the tag rather than sizeof: the format is versioned
 * and explicitly allowed to grow, so striding by our own struct would
 * misread a loader newer than this code.
 */
static void mb2_for_each_ram(uint32_t info_pa,
			     void (*fn)(uint64_t addr, uint64_t len, void *arg),
			     void *arg)
{
	const struct mb2_tag_mmap *mm;
	const uint8_t *p, *end;

	mm = (const struct mb2_tag_mmap *)mb2_find_tag(info_pa,
						       MB2_TAG_MEMORY_MAP);
	if (mm == 0 || mm->entry_size < sizeof(struct mb2_mmap_entry))
		return;

	p = (const uint8_t *)mm + sizeof(*mm);
	end = (const uint8_t *)mm + mm->size;

	for (; p + mm->entry_size <= end; p += mm->entry_size) {
		const struct mb2_mmap_entry *e;

		e = (const struct mb2_mmap_entry *)p;
		if (e->type == MB2_MEM_AVAILABLE)
			fn(e->addr, e->len, arg);
	}
}

static void top_cb(uint64_t addr, uint64_t len, void *arg)
{
	uint64_t *top = arg;

	if (addr + len > *top)
		*top = addr + len;
}

static void sum_cb(uint64_t addr, uint64_t len, void *arg)
{
	uint64_t *sum = arg;

	(void)addr;
	*sum += len;
}

uint64_t mb2_top_of_ram(uint32_t info_pa)
{
	uint64_t top = 0;

	mb2_for_each_ram(info_pa, top_cb, &top);
	return top;
}

uint64_t mb2_usable_ram(uint32_t info_pa)
{
	uint64_t sum = 0;

	mb2_for_each_ram(info_pa, sum_cb, &sum);
	return sum;
}

/*
 * Where the loader's structure is, kept for the questions that come later
 * (#453).  A plain global rather than a parameter threaded through: the
 * caller of <kern/boot_modules.h> is kern/bootstrap.c, which runs long after
 * the boot handoff and has no way to have been handed it.
 *
 * ⚠️ In .data, not .bss, and that is the #337 rule: nothing on this target
 * clears .bss -- the loader zeroes each segment past what the file supplies,
 * which happens before C runs -- but writing it as an initialised object
 * keeps the rule visible where the next boot-time global is written.
 */
static uint32_t mb2_info_pa = 0;

void mb2_remember(uint32_t info_pa)
{
	mb2_info_pa = info_pa;
}

uint32_t mb2_info(void)
{
	return mb2_info_pa;
}

/*
 * The n-th module tag, or NULL.
 *
 * Multiboot 2 has no module array and no count: each module is its own tag
 * in the chain, so this is a walk and not an index.  mb2_find_tag() answers
 * only the first tag of a type, so the walk is here rather than there --
 * teaching it to skip would give every other caller an argument it does not
 * want.
 */
static const struct mb2_tag_module *mb2_module(unsigned int n)
{
	uint32_t	info_pa = mb2_info_pa;
	const uint8_t	*base;
	uint32_t	total, off;
	unsigned int	seen = 0;

	if (info_pa == 0)
		return 0;

	base  = (const uint8_t *) (uintptr_t) info_pa;
	total = *(const uint32_t *) base;
	off   = 8;

	while (off + sizeof(struct mb2_tag) <= total) {
		const struct mb2_tag *tag = (const struct mb2_tag *)(base + off);

		if (tag->type == MB2_TAG_END)
			break;
		if (tag->size < sizeof(struct mb2_tag))
			break;
		if (tag->type == MB2_TAG_MODULE) {
			if (seen == n)
				return (const struct mb2_tag_module *) tag;
			seen++;
		}
		off += (tag->size + 7) & ~7u;
	}

	return 0;
}

unsigned int mb2_module_count(void)
{
	unsigned int n = 0;

	while (mb2_module(n) != 0)
		n++;

	return n;
}

boolean_t mb2_module_range(unsigned int n, uint64_t *start, uint64_t *size)
{
	const struct mb2_tag_module *m = mb2_module(n);

	if (m == 0)
		return FALSE;

	*start = m->mod_start;
	*size  = (uint64_t) m->mod_end - m->mod_start;
	return TRUE;
}

const char *mb2_module_string(unsigned int n)
{
	const struct mb2_tag_module *m = mb2_module(n);

	if (m == 0)
		return "";

	return m->string;
}

const char *mb2_cmdline(void)
{
	const struct mb2_tag_string *t;

	t = (const struct mb2_tag_string *) mb2_find_tag(mb2_info_pa,
							MB2_TAG_CMDLINE);
	if (t == 0)
		return "";

	return t->string;
}
