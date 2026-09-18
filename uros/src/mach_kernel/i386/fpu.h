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
/* CMU_HIST */
/*
 * Revision 2.1.1.1.1.1  92/03/03  16:15:22  jeffreyh
 * 	Pick up from TRUNK
 * 	[92/02/26  11:09:38  jeffreyh]
 * 
 * Revision 2.3  92/02/19  15:08:04  elf
 * 	Added fwait()
 * 	[92/01/19            jvh]
 * 
 * Revision 2.2  92/01/03  20:05:49  dbg
 * 	Created.
 * 	[91/12/23  16:32:15  dbg]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991 Carnegie Mellon University
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

/*
 */

#ifndef	_I386_FPU_H_
#define	_I386_FPU_H_

/*
 * Macro definitions for routines to manipulate the
 * floating-point processor.
 */

#include <cpus.h>
#include <fpe.h>
#include <i386/proc_reg.h>
#include <i386/thread.h>
#include <kern/kern_types.h>
#include <mach/i386/kern_return.h>
#include <mach/i386/thread_status.h>

/*
 * FPU instructions.
 */
#define	fninit() \
	__asm__ volatile("fninit")

#define	fnstcw(control) \
	__asm__("fnstcw %0" : "=m" (*(unsigned short *)(control)))

#define	fldcw(control) \
	__asm__ volatile("fldcw %0" : : "m" (*(unsigned short *) &(control)) )

static __inline__ unsigned short fnstsw(void)
{
	unsigned short status;
	__asm__ volatile("fnstsw %0" : "=ma" (status));
	return(status);
}

#define	fnclex() \
	__asm__ volatile("fnclex")

#define	fnsave(state) \
	__asm__ volatile("fnsave %0" : "=m" (*state))

#define	frstor(state) \
	__asm__ volatile("frstor %0" : : "m" (state))

#define	fxsave(state) \
	__asm__ volatile("fxsave %0" : "=m" (*(state)))

#define	fxrstor(state) \
	__asm__ volatile("fxrstor %0" : : "m" (*(state)))

/*
 * XSAVE/XRSTOR instructions.
 * Save/restore all state components enabled in XCR0.
 * Uses .byte encoding for assembler compatibility.
 * EDX:EAX = component mask (-1 = all enabled components).
 * ECX = pointer to save area (must be 64-byte aligned).
 */
#define	xsave(state) \
	__asm__ volatile( \
		".byte 0x0f, 0xae, 0x21\n" /* xsave (%ecx) */ \
		: : "c" (state), \
		    "a" ((unsigned int)(-1)), \
		    "d" ((unsigned int)(-1)) \
		: "memory")

#define	xrstor(state) \
	__asm__ volatile( \
		".byte 0x0f, 0xae, 0x29\n" /* xrstor (%ecx) */ \
		: : "c" (state), \
		    "a" ((unsigned int)(-1)), \
		    "d" ((unsigned int)(-1)) \
		: "memory")

#define fwait() \
    	__asm__("fwait");

/*
 * If floating-point instructions are emulated,
 * we must load the floating-point register selector
 * when switching to a new thread.
 */
#if	FPE
extern void	fpe_init(void);
extern boolean_t fp_emul_error(struct i386_saved_state *regs);
extern void	fpe_exception_fixup(int exc,
				    int code,
				    int subcode);
extern void	disable_fpe(void);
extern void	enable_fpe(struct i386_fpsave_state *ifps);

#define	fpu_save_context(thread) \
    { \
	if (fp_kind == FP_SOFT) \
	    disable_fpe(); \
	else \
	    set_ts(); \
    }

#define	fpu_load_context(pcb) \
    { \
	register struct i386_fpsave_state *ifps; \
	if (fp_kind == FP_SOFT && (ifps = pcb->ims.ifps) != 0) \
	    enable_fpe(ifps); \
    }

#else	/* no FPE */

/*
 * The switch carries the FPU state (#560).
 *
 * 🔴 It did not, and that was CVE-2018-3665.  The outgoing thread's registers
 * were left in the unit behind CR0.TS and the incoming thread ran with them
 * still there; the restore came later, from the #NM trap, the first time the
 * new thread touched the unit.  CR0.TS does stop the instruction
 * architecturally, but it does not stop the processor from executing it
 * speculatively against those registers, and the result leaves by a side
 * channel.  What sits in them is a previous thread's data -- AES-NI round
 * keys live in the XMM registers -- so the leak is between any two threads
 * that share a processor.
 *
 * On one CPU it was worse than deferred: fpu_save_context() was only
 * set_ts(), so the state was deliberately LEFT in the registers, across the
 * quanta of an unbounded number of other threads, until somebody else
 * happened to want the unit.
 *
 * These are functions rather than macros now because the restore has a case
 * to decide (see fpu_load_context) and because the save is one decision on
 * both uniprocessor and SMP: the split those two macros used to have existed
 * only to express the laziness, and there is none left to express.
 */
extern void		fpu_save_context(
					thread_t			thread);
extern void		fpu_load_context(
					thread_act_t			thr_act);

#endif	/* no FPE */

extern int	fp_kind;

extern unsigned int	xsave_area_size;	/* runtime XSAVE area size (0 if no XSAVE) */
extern unsigned int	xcr0_value;		/* current XCR0 value (enabled state components) */

extern void		init_fpu(void);
extern void		fpu_module_init(void);
extern void		fp_free(
				struct i386_fpsave_state	* fps);
extern kern_return_t	fpu_set_state(
				thread_act_t			thr_act,
				struct i386_float_state		* st);
extern kern_return_t	fpu_get_state(
				thread_act_t			thr_act,
				struct i386_float_state		* st);
extern void		fpnoextflt(void);
extern void		fpextovrflt(void);
extern void		fpexterrflt(void);
extern void		fpsseflt(void);
extern void		fp_state_alloc(void);
extern void		fp_state_alloc_pcb(
					pcb_t				pcb);
extern void		fpflush(thread_act_t);

/*
 * #309 acceptance: this CPU came up with CR4.OSFXSR (and OSXSAVE where the
 * hardware has XSAVE) actually programmed.  Run once per processor, the BSP
 * from machine_kernel_ready() and each AP from slave_machine_init().
 *
 * Declared here rather than as an `extern' in each of the two callers, which
 * is what it was: two private opinions about a signature, neither ever
 * compared with the definition (#448, #453).
 */
extern void		fpu_sanity_check(void);

#endif	/* _I386_FPU_H_ */
