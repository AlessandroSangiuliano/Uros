/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Where the AP trampoline lives, and what is in it (#438).
 *
 * Shared between the assembly that runs there and the C that copies it and
 * fills its parameters, so the two cannot drift: every offset below is used
 * from both sides.
 */

#ifndef _X86_64_CPU_AP_TRAMPOLINE_H_
#define _X86_64_CPU_AP_TRAMPOLINE_H_

/*
 * The physical page a woken processor starts at.
 *
 * Below 1 MiB because that is the only kind of address a startup interrupt
 * can name, and page-aligned for the same reason: the message carries a
 * vector, not an address.
 *
 * 0x8000 specifically: clear of the interrupt vector table and BIOS data
 * area at the bottom of memory, and clear of the extended BIOS data area
 * that sits just under 640 KiB.  Nothing else in this kernel allocates
 * below 1 MiB — the frame allocator's low-water mark starts there — so the
 * page cannot be handed out from underneath us.
 */
#define AP_TRAMPOLINE_BASE	0x8000

/*
 * A fixed layout inside the page, generously spaced: the assembler refuses
 * to move backwards, so a stage that outgrows its slot stops the build
 * rather than overwriting the next one — which is the failure mode worth
 * having, since the alternative is a trampoline that runs into its own
 * descriptor table.
 *
 * Fixed at all because the assembly has to reach these by absolute address — it is running before anything that could compute
 * one.  Offsets rather than symbols for the same reason: at the point the
 * 16-bit code loads the descriptor table, there is no relocation that could
 * have told it where the table ended up.
 */
#define AP_PROT_OFFSET		0x0040	/* 32-bit entry  */
#define AP_LONG_OFFSET		0x0100	/* 64-bit entry  */
#define AP_GDT_OFFSET		0x0180
#define AP_GDT_PTR_OFFSET	0x01B0
#define AP_PARAM_OFFSET		0x01C0

#define AP_PROT_ENTRY		(AP_TRAMPOLINE_BASE + AP_PROT_OFFSET)
#define AP_LONG_ENTRY		(AP_TRAMPOLINE_BASE + AP_LONG_OFFSET)
#define AP_GDT_ENTRY		(AP_TRAMPOLINE_BASE + AP_GDT_OFFSET)
#define AP_GDT_PTR		(AP_TRAMPOLINE_BASE + AP_GDT_PTR_OFFSET)
#define AP_PARAM_CR3		(AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET)
#define AP_PARAM_ENTRY		(AP_TRAMPOLINE_BASE + AP_PARAM_OFFSET + 8)

#define AP_TRAMPOLINE_SIZE	0x01D0

#ifndef __ASSEMBLER__

#include <stdint.h>

/*
 * Put the trampoline where a startup interrupt can reach it and fill in
 * what it cannot know: the page tables to translate through, and the
 * higher-half address to continue at.
 *
 * Idempotent — every processor is given the identical page, which is what
 * lets them all be woken at once.
 */
void ap_trampoline_install(void);

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_CPU_AP_TRAMPOLINE_H_ */
