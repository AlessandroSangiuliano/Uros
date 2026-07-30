/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Machine-dependent exception codes — x86-64 (#416).
 *
 * When the kernel turns a trap into an exception message, the *code* it sends
 * is the processor's vector.  So this file is one half of a pair: the numbers
 * here and the vector numbers in x86_64/trap/trap.h are the same numbers, and
 * a handler in another task reads them through this header alone.
 *
 * ⚠️ The i386 header carries two sets of names for the same vectors — an
 * older one (EXC_I386_DIV, EXC_I386_INTO, ...) whose values are small ordinals
 * unrelated to the hardware, and a newer one (EXC_I386_DIVERR, EXC_I386_PGFLT,
 * ...) whose values *are* the vectors.  Both exist because the first was kept
 * when the second replaced it.  Only the vector-valued set is here: two
 * spellings of a fault, one of which is not the vector, is how a handler ends
 * up switching on the wrong number.
 *
 * The names are the architecture's own mnemonics rather than i386's spellings,
 * because a #GP is called #GP in the manual a reader of this file will have
 * open.
 */

#ifndef	_MACH_X86_64_EXCEPTION_H_
#define _MACH_X86_64_EXCEPTION_H_

#define	EXC_TYPES_COUNT		10	/* incl. illegal exception 0 */

/* Currently a code and a subcode. */
#define EXCEPTION_CODE_MAX	2

/*
 * The vectors, as the processor numbers them.  Sixteen through twenty-one did
 * not exist on i386 and do here; twenty-one in particular (#CP) only arrives
 * on a processor with control-flow enforcement, which this kernel disables —
 * it is listed so that a handler that meets one can name it rather than
 * report a bare number.
 */
#define EXC_X86_64_DIVERR	0	/* divide error			*/
#define EXC_X86_64_SGLSTP	1	/* debug exception		*/
#define EXC_X86_64_NMIFLT	2	/* non-maskable interrupt	*/
#define EXC_X86_64_BPTFLT	3	/* breakpoint			*/
#define EXC_X86_64_INTOFLT	4	/* overflow			*/
#define EXC_X86_64_BOUNDFLT	5	/* bound range exceeded		*/
#define EXC_X86_64_INVOPFLT	6	/* invalid opcode		*/
#define EXC_X86_64_NOEXTFLT	7	/* device not available		*/
#define EXC_X86_64_DBLFLT	8	/* double fault			*/
#define EXC_X86_64_INVTSSFLT	10	/* invalid TSS			*/
#define EXC_X86_64_SEGNPFLT	11	/* segment not present		*/
#define EXC_X86_64_STKFLT	12	/* stack-segment fault		*/
#define EXC_X86_64_GPFLT	13	/* general protection		*/
#define EXC_X86_64_PGFLT	14	/* page fault			*/
#define EXC_X86_64_EXTERRFLT	16	/* x87 floating-point error	*/
#define EXC_X86_64_ALIGNFLT	17	/* alignment check		*/
#define EXC_X86_64_MCHKFLT	18	/* machine check		*/
#define EXC_X86_64_SSEFLT	19	/* SIMD floating-point error	*/
#define EXC_X86_64_VIRTFLT	20	/* virtualisation exception	*/
#define EXC_X86_64_CTLFLT	21	/* control protection		*/

/*
 * ⚠️ Vector 9 (coprocessor segment overrun) is absent because the processor
 * no longer raises it, and vector 15 is reserved.  Leaving gaps is the point:
 * a number that arrives here and has no name is a fact worth noticing, not a
 * hole to fill with the nearest plausible label.
 */

#define	EXC_MASK_MACHINE	0

#endif	/* _MACH_X86_64_EXCEPTION_H_ */
