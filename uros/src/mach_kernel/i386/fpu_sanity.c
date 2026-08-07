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
 * Issuing a stray SSE instruction from generic kernel code is unsafe:
 * the kernel runs with CR0.TS armed for lazy FPU context switching,
 * and the trap handler for it (fpnoextflt) expects a real FPU thread
 * context, which we don't have during early init.  Instead we verify
 * exactly the bit the #309 fix is responsible for setting on each
 * CPU: CR4.OSFXSR, plus CR4.OSXSAVE on hardware that has XSAVE.
 *
 * This is the per-CPU acceptance criterion in mechanical form: panic
 * if the AP came up without ap_machine_init() / init_fpu() having
 * programmed those bits, which is what we observed before the fix.
 */
#define	CR4_OSFXSR	0x00000200
#define	CR4_OSXMMEXCPT	0x00000400
#define	CR4_OSXSAVE	0x00040000

extern unsigned int get_cr4(void);
extern void panic(const char *, ...);

void
fpu_sanity_check(void)
{
	unsigned int cr4 = get_cr4();

	if ((cr4 & CR4_OSFXSR) == 0)
		panic("fpu_sanity: cpu %d CR4.OSFXSR not set (cr4=0x%x) — "
		      "#309 ap_machine_init / init_fpu missed",
		      current_cpu_id(), cr4);
	if ((cr4 & CR4_OSXMMEXCPT) == 0)
		panic("fpu_sanity: cpu %d CR4.OSXMMEXCPT not set",
		      current_cpu_id());

	printf("fpu_sanity: cpu %d CR4=0x%x OSFXSR+OSXMMEXCPT%s ok (#309)\n",
	       current_cpu_id(), cr4,
	       (cr4 & CR4_OSXSAVE) ? "+OSXSAVE" : "");
}
