/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The other output (#568).
 *
 * Until now this target had one: COM1.  Every self-test verdict, every panic,
 * every printf left through it, which is why the serial port could not be
 * given to the userspace driver it belongs to (#497) and why a machine with no
 * COM port -- every recent laptop, and the one scripts/make-omen-boot.sh
 * exists for -- could not be booted here at all while i386 could.
 *
 * This draws the same bytes into the linear framebuffer the loader set up.
 * i386 has had it since #342; the names elsewhere are `earlycon=efifb' on
 * Linux, `vt_efifb' on FreeBSD, `genfb' on NetBSD.
 *
 * ── What it is not ────────────────────────────────────────────────────
 *
 * 🔴 NOT A DISPLAY DRIVER.  The graphics stack is gpu_server's, in user space.
 * This owns the framebuffer only until something with a better claim asks for
 * it, and what that handover looks like is #369's question on both targets.
 *
 * ⚠️ And not input.  Output only; a keyboard on a machine with no serial port
 * is #335.
 *
 * ── Why it can be trusted when nothing else can ───────────────────────
 *
 * No locks, no allocation after init, no interrupts, no scheduler, and no
 * reads back from the framebuffer.  That is the whole of its dependency list,
 * and it is the property that makes it worth having: the moment output matters
 * most is the moment the rest of the machine is least trustworthy.
 */

#ifndef _X86_64_DDB_FBCONS_H_
#define _X86_64_DDB_FBCONS_H_

#include <stdint.h>

#include <boot/multiboot2.h>

/*
 * Keep what the loader said, for an init that happens much later.
 *
 * 🔑 Called from the boot narration, while GRUB's tag list is still where GRUB
 * left it and identity-mapped.  fbcons_init() cannot read it itself: it runs
 * after the pmap is up, by which time that memory is nobody's in particular.
 */
void fbcons_remember(const struct mb2_framebuffer *fb);

/*
 * Map the framebuffer and clear the screen.  A no-op when the loader gave us
 * none, or gave us one this cannot draw into -- and it says which, once,
 * because a boot that drew nothing because there was nothing to draw on and a
 * boot that drew nothing because this is broken look identical otherwise.
 *
 * ⚠️ Must run after the pmap can map device memory, which on this target means
 * machine_init() or later.  Nothing printed before that can be drawn when it
 * is printed -- structural, not an oversight: the framebuffer is above the low
 * identity map and there is no way to reach it earlier.  It is kept instead,
 * and with -f this draws it, a line at a time and slowly enough for a camera,
 * before the console goes on live; without -f it says only how many bytes
 * went to COM1 alone (#666).
 */
void fbcons_init(void);

/*
 * One character, so every writer may call it unconditionally.  Drawn once
 * fbcons_init() has succeeded; before that, kept for fbcons_init() to draw
 * (the first 64 KiB, #666); after an fbcons_init() that found no screen, a
 * no-op.
 */
void fbcons_putc(char c);

/*
 * The bytes kept while there was no screen, so far (#666), for #373's zone to
 * start from: how many are kept, the bytes themselves in *bytes, and in
 * *offered how many were handed over in all -- more than kept once the 64 KiB
 * filled.
 */
uint32_t fbcons_early(const char **bytes, uint32_t *offered);

/*
 * Make sure what has been drawn is actually on the panel.
 *
 * 🔴 WRITE-COMBINING MEANS STORES MAY STILL BE IN A BUFFER.  That is the point
 * of it and it is fine while the machine goes on -- the buffer drains on the
 * next fence, locked instruction, port access or interrupt, of which a running
 * kernel has no shortage.  It is not fine at the end: a panic's last line
 * sitting in a store buffer when the processor halts is the one line that
 * mattered, lost to the optimisation that made the rest cheap.
 *
 * Called on every newline, which costs a fence per LINE against a hundred
 * glyphs, and on the way down.
 */
void fbcons_flush(void);

/* Whether anything is being drawn, for the per-boot report. */
int fbcons_present(void);

/* Geometry and cost, for the per-boot report (#568, #551's precedent). */
unsigned fbcons_cols(void);
unsigned fbcons_rows(void);
uint64_t fbcons_cycles(void);		/* cycles spent drawing, this boot */
uint64_t fbcons_glyphs(void);		/* glyphs drawn, this boot */
uint64_t fbcons_scrolls(void);		/* times the screen scrolled */

#endif	/* _X86_64_DDB_FBCONS_H_ */
