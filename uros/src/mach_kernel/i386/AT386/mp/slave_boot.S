/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */

/*
 * MkLinux
 */

/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */

/* CMU_HIST */
/*
 * Revision 2.1.3.2  92/05/26  11:23:30  jeffreyh
 * 	Removed debug trace as it was altering random memory
 * 	when running in real mode (observed on Compaq SystemPro).
 * 	[92/05/07            bernadat]
 * 
 * Revision 2.1.3.1  92/04/30  11:57:56  bernadat
 * 	Moved from cbus to here, applies to both Corollary
 * 	and SystemPro
 * 	[92/04/08            bernadat]
 * 
 * Revision 2.1.9.1  92/02/18  18:34:59  jeffreyh
 * 	Created
 * 	[91/06/27  05:00:51  bernadat]
 * 
 */

#include "machine/asm.h"
#include "machine/AT386/mp/boot.h"

#define CR0_PE_ON	0x1
#define CR0_PE_OFF	0xfffffffe

	.file	"slave_boot.s"

	.text

/*
 * #308: the trampoline below executes in real mode (16-bit) up to the
 * ljmp at the end of real_to_prot, then in 32-bit protected mode from
 * `xprot:` onwards.  The original source used `data16`/`addr16` prefixes
 * to flip operand size on a per-instruction basis, but it never told
 * GAS which base mode to use — GAS defaulted to .code32 for the whole
 * file and the prefixes ended up *reversing* the semantics: every
 * `data16 mov $imm, %eax` was emitted as a 16-bit-immediate move with
 * the high two bytes of the intended 32-bit immediate left in the
 * stream as spurious `00 00` (= `add %al,(%eax)`) bytes that the real
 * CPU happily executed.  The fix is one-line obvious:
 *
 *   .code16        for the part that runs in real mode
 *   .code32        for the part that runs in protected mode
 *
 * With explicit base modes the `data16`/`addr16` prefixes are no
 * longer needed — they are removed below.
 */

	.code16

#define	LJMP(segment,address)	\
	.byte	0xea		;\
	.long	address-EXT(slave_boot_base)		;\
	.word	segment

#define	LJMP16(segment,address)	\
	.byte	0xea		;\
	.word	address-EXT(slave_boot_base)		;\
	.word	segment

#define	LGDT(address)	\
	.word	0x010f ;\
	.byte	0x15 ;\
	.long	address-EXT(slave_boot_base)

ENTRY(slave_boot_base)
	/* code is loaded at 0x0:0x1000 */
	/* ljmp to the next instruction to set up %cs */
	LJMP16(MP_BOOTSEG, EXT(slave_pstart))

ENTRY(slave_pstart)
	/* set up %ds */
	mov	%cs, %ax
	mov	%ax, %ds

	/* set up %ss and %esp */
	mov	$MP_BOOTSEG, %eax
	mov	%ax, %ss
	mov	$MP_BOOTSTACK, %esp

	/* set up %es */
	mov	%ax, %es

	/* change to protected mode */
	call	EXT(real_to_prot)

	.code32
	pushl	MP_MACH_START
	call	EXT(startprog)
	.code16

/
/* real_to_prot() */
/* 	transfer from real mode to protected mode. */
/

ENTRY(real_to_prot)
	/* guarantee that interrupt is disabled when in prot mode */
	cli

	/*
	 * Load the GDTR.  The LGDT() macro emits a 32-bit displacement
	 * (gdtr - slave_boot_base) immediately after the opcode, which
	 * the CPU only decodes correctly if we tell it to use 32-bit
	 * addressing.  In real mode (.code16) the default is 16-bit
	 * addressing, so add an addr32 (0x67) prefix.  Real-mode segment
	 * base (DS = MP_BOOTSEG, base = 0x1000) is still added on top
	 * of the disp32, so effective phys = 0x1000 + (gdtr - base) =
	 * MP_GDT, which is what we want.
	 */
	.byte	0x67			/* addr32 */
	LGDT(EXT(gdtr))

	/* set the PE bit of CR0 */
	mov	%cr0, %eax
	or	$CR0_PE_ON, %eax
	mov	%eax, %cr0

	/* make intrasegment jump to flush the processor pipeline and
	 * reload CS register.  data16 prefix forces 32-bit operand so
	 * the immediate after `ea` is a 32-bit offset (matching what
	 * LJMP() emits), and we land in xprot (D=1 code segment 0x08). */
	data16
	LJMP(0x08, xprot)

	.code32
xprot:
	/*
	 * We are in USE32 mode now.  Keep SS pointing at the boot data
	 * segment (selector 0x10, base = MP_BOOT) so the existing 16-bit
	 * stack address remains valid, but flip DS / ES to the flat
	 * "init data" segment (selector 0x20, base = 0, limit = 4 GB).
	 * The caller is about to `pushl MP_MACH_START` (= immediate
	 * 0x800) and that must read phys 0x800 absolute, which only works
	 * if DS has base zero.  Previously DS had base = 0x1000 and the
	 * pushed value came from phys 0x1800 — garbage from the bootstack.
	 */
	mov	$0x10, %eax
	movw	%ax, %ss

	mov	$0x20, %eax
	movw	%ax, %ds
	movw	%ax, %es

	/*
	 * #328: the `call EXT(real_to_prot)` in slave_pstart runs in real mode
	 * (.code16) and therefore pushes a *16-bit* return IP, but this `ret`
	 * sits in .code32 and defaults to a *32-bit* pop — it consumes the
	 * 2-byte return plus 2 extra bytes off the top of the boot stack and
	 * folds them into the high half of EIP.  Those stale bytes are zero by
	 * luck when only one AP boots, but garbage once a second AP has run,
	 * so the over-wide ret jumped to a bad address and the 3rd CPU looped
	 * in the trampoline (system triple-fault on --smp 4 / NCPUS>2).  Force
	 * a 16-bit ret (operand-size prefix 0x66 -> retw) so it pops exactly
	 * the 2 bytes the real-mode call pushed.
	 */
	.byte	0x66			/* operand-size prefix: 16-bit ret (retw) */
	ret
	.code16

/
/* startprog(phyaddr) */
/	start the program on protected mode where phyaddr is the entry point
/

	.code32
ENTRY(startprog)
	push	%ebp
	mov	%esp, %ebp

	mov	0x8(%ebp), %ecx		/* entry offset  */
	mov	$0x28, %ebx		/* segment */
	push	%ebx
	push	%ecx

	/* set up %ds and %es */
	mov	$0x20, %ebx
	movw	%bx, %ds
	movw	%bx, %es

	lret


	. = MP_GDT-MP_BOOT	/* GDT location */
ENTRY(Gdt)

/*  Segment Descriptor
 *
 * 31          24         19   16                 7           0
 * ------------------------------------------------------------
 * |             | |B| |A|       | |   |1|0|E|W|A|            |
 * | BASE 31..24 |G|/|0|V| LIMIT |P|DPL|  TYPE   | BASE 23:16 |
 * |             | |D| |L| 19..16| |   |1|1|C|R|A|            |
 * ------------------------------------------------------------
 * |                             |                            |
 * |        BASE 15..0           |       LIMIT 15..0          |
 * |                             |                            |
 * ------------------------------------------------------------
 */
	.word	0,0		/* 0x0 : null */
	.byte	0,0,0,0

	.word	0xffff,MP_BOOT	/* 0x8 : boot code */
	.byte	0,0x9e,0x40,0

	.word	0xffff,MP_BOOT	/* 0x10 : boot data */
	.byte	0,0x92,0x40,0

	.word	0xffff,MP_BOOT	/* 0x18 : boot code, 16 bits */
	.byte	0,0x9e,0x0,0

	.word	0xffff,0	/* 0x20 : init data */
	.byte	0,0x92,0xcf,0

	.word	0xffff,0	/* 0x28 : init code */
	.byte	0,0x9e,0xcf,0

ENTRY(gdtr)
	.short	48		/* limit (8*6 segs) */
	.short	MP_GDT		/* base low */
	.short	0		/* base high */

ENTRY(slave_boot_end)














