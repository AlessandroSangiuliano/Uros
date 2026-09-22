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
 * machine_init() or later.  Everything printed before that reaches COM1 only,
 * exactly as on i386 -- structural, not an oversight: the framebuffer is above
 * the low identity map and there is no way to reach it earlier.
 */
void fbcons_init(void);

/*
 * One character.  A no-op until fbcons_init() has succeeded, so every writer
 * may call it unconditionally.
 */
void fbcons_putc(char c);

/* Whether anything is being drawn, for the per-boot report. */
int fbcons_present(void);

/* Geometry and cost, for the per-boot report (#568, #551's precedent). */
unsigned fbcons_cols(void);
unsigned fbcons_rows(void);
uint64_t fbcons_cycles(void);		/* cycles spent drawing, this boot */
uint64_t fbcons_glyphs(void);		/* glyphs drawn, this boot */
uint64_t fbcons_scrolls(void);		/* times the screen scrolled */

#endif	/* _X86_64_DDB_FBCONS_H_ */
