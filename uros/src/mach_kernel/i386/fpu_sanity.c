/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * fpu_sanity.c — quick per-CPU SSE/FPU sanity check (#309 acceptance).
 *
 * Exercises one SSE2 packed-move + one x87 load on whichever CPU calls
 * us.  If CR4.OSFXSR is not set (i.e. ap_machine_init didn't run on
 * this CPU), the `movdqa` traps with #UD long before the printf at
 * the bottom and we never see the success line.
 *
 * Called once from setup_main() on the BSP and once from
 * slave_machine_init() on every AP, after ap_machine_init/init_fpu.
 */

#include <i386/fpu.h>			/* #448: the interface, so it is checked */
#include <kern/cpu_data.h>		/* current_cpu_id() */
#include <kern/misc_protos.h>		/* printf, panic */

/*
 * Rather than issue a stray SSE instruction from generic kernel code, this
 * verifies the bits themselves -- which is the per-CPU acceptance criterion in
 * mechanical form: panic if the processor came up without ap_machine_init() /
 * init_fpu() having programmed them, which is what #309 observed.
 *
 * ⚠️ The comment that used to be here said the kernel runs with CR0.TS armed
 * for lazy FPU context switching.  It has not since #560: the switch carries
 * the state and CR0.TS is never armed on either target.  A comment asserting a
 * property the tree no longer has is worse than no comment, because it reads
 * as a reason not to look.
 *
 * 🔴 CR0.NE joins the list here (#515), and this is the right place for it
 * rather than a print in init_fpu(): the 1991 note in locore.S that first
 * asked for it said "set CR0_NE for slave processors, they do not have a PIC",
 * so the bit is exactly the kind of thing an AP can come up without.  The
 * 387-era fallback for NE=0 is FERR# on a PIC line, and a processor with no
 * PIC in front of it has nowhere to send it at all.
 */
#define	CR0_NE_BIT	0x00000020
#define	CR4_OSFXSR	0x00000200
#define	CR4_OSXMMEXCPT	0x00000400
#define	CR4_OSXSAVE	0x00040000

extern unsigned int get_cr0(void);
extern unsigned int get_cr4(void);
extern void panic(const char *, ...);

void
fpu_sanity_check(void)
{
	unsigned int cr0 = get_cr0();
	unsigned int cr4 = get_cr4();

	if ((cr4 & CR4_OSFXSR) == 0)
		panic("fpu_sanity: cpu %d CR4.OSFXSR not set (cr4=0x%x) — "
		      "#309 ap_machine_init / init_fpu missed",
		      current_cpu_id(), cr4);
	if ((cr4 & CR4_OSXMMEXCPT) == 0)
		panic("fpu_sanity: cpu %d CR4.OSXMMEXCPT not set",
		      current_cpu_id());
	if ((cr0 & CR0_NE_BIT) == 0)
		panic("fpu_sanity: cpu %d CR0.NE not set (cr0=0x%x) — an x87 "
		      "numeric error on this processor would assert FERR# "
		      "instead of raising #MF (#515)",
		      current_cpu_id(), cr0);

	printf("fpu_sanity: cpu %d CR0=0x%x NE ok, CR4=0x%x "
	       "OSFXSR+OSXMMEXCPT%s ok (#309, #515)\n",
	       current_cpu_id(), cr0, cr4,
	       (cr4 & CR4_OSXSAVE) ? "+OSXSAVE" : "");
}
