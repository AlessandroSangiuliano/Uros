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
 * Revision 2.8.8.3  92/09/15  17:14:47  jeffreyh
 * 	Add volatile declaration to init_fpu to prevent compiler constant
 * 	folding.  From Michael Bushnell (mib@gnu.ai.mit.edu).
 * 	[92/08/06            dlb]
 * 
 * Revision 2.8.8.2  92/03/03  16:15:11  jeffreyh
 * 	Merge up to TRUNK
 * 	[92/03/03  09:55:11  jeffreyh]
 * 
 * Revision 2.11  92/02/26  13:12:30  elf
 * 	Fpinit fixes from dlb.
 * 	[92/02/26            danner]
 * 
 * Revision 2.10  92/02/19  15:08:00  elf
 * 	Make sure the current thread's floating instruction has completed
 * 	before freeing its fpu context and before context switching the FPU.
 * 
 * 	Remember which thread the FPU AST was meant for and delay sending
 * 	exception if a context switch has occurred between the interrupt and
 * 	the AST handling. This can happen if an fpu interrupt arrives after 
 * 	FPU ASTs have been checked for but before thread_block.
 * 
 * 	Mark pending exception for thread in fp_valid (= 2) in fpexterrflt or
 * 	fp_intr if the thread that caused the exception is not running and
 * 	send the exception when the thread next runs. (I'm not really sure 
 * 	this is necessary).
 * 
 * 	Check fp_thread for THREAD_NULL in fp_free and fp_intr before
 * 	accessing its PCB.
 * 
 * 	Don't save FPU context in fpexterrflt. It is already saved in 
 * 	fp_intr. 
 * 	[92/02/01            jvh]
 * 
 * Revision 2.9  92/01/03  20:05:42  dbg
 * 	Move floating-point state manipulation from i386/pcb.c to this
 * 	file.  Add support for floating-point emulator.  Restore
 * 	FPU shuffling between threads if single CPU.  Add locking
 * 	to PCB to avoid losing fp_save structures.
 * 	[91/10/20            dbg]
 * 
 * Revision 2.8  91/06/19  11:55:02  rvb
 * 	cputypes.h->platforms.h
 * 	[91/06/12  13:44:41  rvb]
 * 
 * Revision 2.7  91/05/14  16:07:37  mrt
 * 	Correcting copyright
 * 
 * Revision 2.6  91/05/08  12:31:32  dbg
 * 	Simplify.  Always store FP state on context switch.
 * 	1:	It's unknown what happens when the i386 switches
 * 		mappings while the i387 has a store instruction
 * 		(e.g. fbstp) in progress.  Where does the data go?
 * 	2:	On a multiprocessor, we'd need interprocessor interrupts
 * 		to fetch a thread's FP state from the CPU it last ran on.
 * 	[91/04/26  14:34:04  dbg]
 * 
 * Revision 2.5  91/03/16  14:44:07  rpd
 * 	Pulled i386_fpsave_state out of i386_machine_state.
 * 	Picked up fixes from dbg.
 * 	[91/02/18            rpd]
 * 
 * Revision 2.4  91/02/05  17:11:45  mrt
 * 	Changed to new Mach copyright
 * 	[91/02/01  17:33:57  mrt]
 * 
 * Revision 2.3  91/01/08  15:10:29  rpd
 * 	Split i386_machine_state off of i386_kernel_state.
 * 	[90/12/31            rpd]
 * 	Reorganized the pcb.
 * 	[90/12/11            rpd]
 * 
 * Revision 2.2  90/05/03  15:25:18  dbg
 * 	Created.
 * 	[90/02/11            dbg]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1992-1990 Carnegie Mellon University
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

/*
 * Support for 80387 floating point or FP emulator.
 */
#include <cpus.h>
#include <fpe.h>
#include <platforms.h>

#include <mach/exception.h>
#include <mach/i386/thread_status.h>
#include <mach/i386/fp_reg.h>

#include <kern/mach_param.h>
#include <kern/thread.h>
#include <kern/zalloc.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/assert.h>

#include <i386/thread.h>
#include <i386/fpu.h>
#include <i386/trap.h>
#include <i386/pio.h>
#include <i386/misc_protos.h>

#if 0
#include <i386/ipl.h>
extern int curr_ipl;
#define ASSERT_IPL(L) \
{ \
      if (curr_ipl != L) { \
	      printf("IPL is %d, expected %d\n", curr_ipl, L); \
	      panic("fpu: wrong ipl"); \
      } \
}
#else
#define ASSERT_IPL(L)
#endif

int		fp_kind = FP_FXSR;	/* FXSAVE/FXRSTOR with SSE support */
unsigned int	xsave_area_size;	/* XSAVE area size from CPUID (0 if no XSAVE) */
unsigned int	xcr0_value;		/* current XCR0 value (enabled state components) */
zone_t		ifps_zone;		/* zone for FPU save area */

/*
 * The state the registers hold when they hold nobody's (#560).
 *
 * A thread whose pcb has no save area still has to be switched to, and the
 * registers at that moment hold the thread we are switching away from.
 * Leaving them is the CVE-2018-3665 leak; this is what gets loaded instead.
 *
 * 🔑 On i386 there is no cheaper way to make the registers safe than to
 * load something over them.  fninit() clears the x87 stack but leaves every
 * XMM register untouched, so the scrub costs a restore either way -- which
 * is the whole reason the lazy scheme has no secure variant to fall back to.
 */
static struct i386_fpsave_state	*fp_clean_state;
static unsigned int		ifps_size;	/* one element of ifps_zone */

/*
 * The thread a floating-point error was raised for, when the AST that
 * carries it has not been taken yet.
 *
 * 🔑 This is NOT ownership of the unit.  There used to be a second variable,
 * fp_act, which said whose state the registers held, and it existed only
 * because the restore was lazy: the registers could belong to a thread that
 * was not running.  The switch now leaves the current thread's state in them
 * and nobody else's, so that question has one answer -- current_act() -- and
 * the variable asking it is gone (#560).
 *
 * This one survives because the AST is per-CPU while the error belongs to a
 * thread: a switch between the interrupt and the AST still hands it to the
 * wrong thread, on one processor and on many alike.  So it is no longer
 * behind NCPUS == 1, which had left SMP with no tracking at all.
 *
 * ⚠️ And it is an array, for the same reason need_ast is one: it shadows a
 * per-CPU AST.  A single global would have been a race the moment two
 * processors took a floating-point error at once -- which is exactly why it
 * could be a scalar while it was uniprocessor-only.
 */
volatile thread_act_t	fp_intr_act[NCPUS];

/*
 * Take the unit away from whoever has it.
 *
 * ⚠️ This used to also arm CR0.TS, which is what made the NEXT use of the
 * unit trap into fpnoextflt().  With the restore eager there is no such
 * route, so arming it here would strand the processor: the trap would come,
 * and the handler that answered it is no longer on the path.  The callers
 * that want the registers emptied get fninit(), which is what "no state" now
 * means, and the switch reloads from memory regardless.
 */
#define	clear_fpu() \
    { \
	fninit(); \
    }

/* Forward */

extern void		fpinit(void);
extern void		fp_save(
				thread_act_t	thr_act);
extern void		fp_load(
				thread_act_t	thr_act);

/*
 * Look for FPU and initialize it.
 * Called on each CPU.
 *
 * Detection order:
 *   1. Check for x87 FPU presence
 *   2. Enable FXSAVE/FXRSTOR (SSE) support
 *   3. Probe CPUID for XSAVE capability; if present:
 *      a. Enable CR4.OSXSAVE
 *      b. Set XCR0 to enable detected features (AVX, AVX-512)
 *      c. Read XSAVE area size from CPUID leaf 0Dh
 */
void
init_fpu(void)
{
	unsigned short	status, control;
	unsigned int	eax, ebx, ecx, edx;

#if !FPE
	/*
	 * Check for FPU by initializing it,
	 * then trying to read the correct bit patterns from
	 * the control and status registers.
	 */
	set_cr0(get_cr0() & ~(CR0_EM|CR0_TS));	/* allow use of FPU */

	fninit();
	status = fnstsw();
	fnstcw(&control);

	if ((status & 0xff) == 0 &&
	    (control & 0x103f) == 0x3f)
	{
	    fp_kind = FP_FXSR;

	    /*
	     * Enable SSE/SSE2 support:
	     * - CR4.OSFXSR: tells CPU that OS supports FXSAVE/FXRSTOR
	     * - CR4.OSXMMEXCPT: enables #XM exception for unmasked SIMD exceptions
	     * - CR0.EM must be clear (no FPU emulation)
	     * - CR0.MP: WAIT/FWAIT honours CR0.TS
	     *
	     * 🔴 CR0.TS is CLEARED here and never armed again (#560).  It used
	     * to be set, so that the first use of the unit by each thread
	     * trapped and the restore could be deferred until then -- which
	     * left the previous thread's registers in place for the new thread
	     * to read speculatively (CVE-2018-3665).  The switch now carries
	     * the state, so there is nothing to defer and nothing to trap on.
	     */
	    set_cr4(get_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
	    set_cr0((get_cr0() & ~(CR0_EM|CR0_TS)) | CR0_MP);

	    /*
	     * Probe CPUID.1:ECX for XSAVE support (bit 26).
	     */
	    do_cpuid(1, 0, &eax, &ebx, &ecx, &edx);

	    if (ecx & CPUID_ECX_XSAVE) {
		unsigned int xcr0_lo, xcr0_hi;
		unsigned int supported_xcr0;

		/*
		 * XSAVE is supported.  Enable CR4.OSXSAVE so that
		 * XSAVE/XRSTOR and XGETBV/XSETBV instructions work.
		 */
		set_cr4(get_cr4() | CR4_OSXSAVE);

		/*
		 * Read the XCR0 mask of supported state components
		 * from CPUID leaf 0Dh, sub-leaf 0.
		 */
		do_cpuid(0x0d, 0, &eax, &ebx, &ecx, &edx);
		supported_xcr0 = eax;	/* lower 32 bits */

		/*
		 * Build our desired XCR0 value.
		 * Always enable x87 + SSE (mandatory for XSAVE).
		 */
		xcr0_lo = XCR0_X87 | XCR0_SSE;

		/* Enable AVX (YMM) if supported */
		if (supported_xcr0 & XCR0_AVX) {
		    xcr0_lo |= XCR0_AVX;
		    printf("FPU: AVX (YMM, 256-bit) detected\n");
		}

		/*
		 * Enable AVX-512 if all required components are supported.
		 * AVX-512 requires opmask (bit 5) + ZMM_Hi256 (bit 6).
		 * Hi16_ZMM (bit 7) is only for 64-bit mode (ZMM8-ZMM15),
		 * but the CPU may still require it as a group — enable if
		 * all three are supported.
		 */
		if ((supported_xcr0 & XCR0_AVX512) == XCR0_AVX512) {
		    /* CPU supports all AVX-512 state bits */
		    xcr0_lo |= XCR0_AVX512;
		    printf("FPU: AVX-512 (ZMM, 512-bit) detected\n");
		} else if ((supported_xcr0 & XCR0_AVX512_I386) ==
			   XCR0_AVX512_I386) {
		    /* i386 subset: opmask + ZMM_Hi256 without Hi16_ZMM */
		    xcr0_lo |= XCR0_AVX512_I386;
		    printf("FPU: AVX-512 (ZMM, i386 subset) detected\n");
		}

		xcr0_hi = 0;

		/*
		 * Write our chosen XCR0 value.
		 */
		xsetbv(0, xcr0_lo, xcr0_hi);
		xcr0_value = xcr0_lo;

		/*
		 * Re-read CPUID leaf 0Dh to get the XSAVE area size
		 * for the features we just enabled.
		 * EBX = size required for currently enabled XCR0 features.
		 * ECX = size required for all supported XCR0 features.
		 */
		do_cpuid(0x0d, 0, &eax, &ebx, &ecx, &edx);
		xsave_area_size = ebx;

		if (xsave_area_size > XSAVE_AREA_MAX_SIZE) {
		    printf("FPU: WARNING: XSAVE area %u exceeds max %u,"
			   " clamping\n",
			   xsave_area_size, XSAVE_AREA_MAX_SIZE);
		    xsave_area_size = XSAVE_AREA_MAX_SIZE;
		}

		fp_kind = FP_XSAVE;
		printf("FPU: XSAVE enabled, area size %u bytes,"
		       " XCR0=0x%x\n",
		       xsave_area_size, xcr0_value);
	    }
	}
	else
#endif /* !FPE */
	{
#if	FPE
	    /*
	     * Use the floating-point emulator.
	     */
	    fp_kind = FP_SOFT;
	    fpe_init();
#else	/* no fpe */
	    /*
	     * NO FPU.
	     */
	    fp_kind = FP_NO;
	    set_cr0(get_cr0() | CR0_EM);
#endif
	}
}

/*
 * Initialize FP handling.
 */
void
fpu_module_init(void)
{
	unsigned int fps_size;

	/*
	 * Choose zone element size based on FPU type.
	 * For XSAVE, the save area is variable-size; use fp_pad + actual size.
	 * For FXSAVE, use the full struct (which includes the max union).
	 * Round up to 64-byte boundary so all elements in a zone page
	 * maintain 64-byte alignment (required by XSAVE).
	 */
	if (fp_kind == FP_XSAVE && xsave_area_size > 0) {
	    /* 64 bytes (fp_valid + fp_pad) + xsave_area_size */
	    fps_size = 64 + xsave_area_size;
	} else {
	    /* 64 bytes (fp_valid + fp_pad) + 512 (fx_save_state) */
	    fps_size = 64 + sizeof(struct i386_fx_save);
	}
	/* Round up to 64-byte alignment for zone element placement */
	fps_size = (fps_size + 63) & ~63U;

	ifps_zone = zinit(fps_size,
			  THREAD_MAX * fps_size,
			  THREAD_CHUNK * fps_size,
			  "i386 fpsave state");
	ifps_size = fps_size;		/* fp_state_alloc_pcb copies a whole one */

	/*
	 * Build the image loaded over the registers when the thread being
	 * switched to has no state of its own (#560).  It is taken FROM the
	 * unit rather than written by hand, so it is by construction a state
	 * this processor accepts: fpinit() leaves the control word and MXCSR
	 * the way a fresh thread gets them, and the save records exactly that.
	 *
	 * init_fpu() has already run -- machine_init() is ahead of
	 * thread_init() in setup_main() -- so fp_kind and xsave_area_size are
	 * the real ones here, which is also why the zone above is sized right.
	 */
	fp_clean_state = (struct i386_fpsave_state *) zalloc(ifps_zone);
	bzero((char *) fp_clean_state, fps_size);
	fpinit();
	if (fp_kind == FP_XSAVE)
	    xsave(&fp_clean_state->fx_save_state);
	else
	    fxsave(&fp_clean_state->fx_save_state);
	fp_clean_state->fp_valid = TRUE;

	/*
	 * Said once, because it is what every thread now starts with and it
	 * used to be established somewhere else entirely -- by fpinit(), off
	 * the #NM trap, the first time a thread touched the unit (#560).
	 * 0x037f is every numeric exception masked, which is what threads got
	 * before and what they must keep getting.
	 */
	printf("fpu: threads start from the unit's own post-fpinit state "
	       "(control 0x%04x, MXCSR 0x%04x)\n",
	       fp_clean_state->fx_save_state.fx_control,
	       fp_clean_state->fx_save_state.fx_MXCSR);
}

/*
 * Free a FPU save area.
 * Called only when thread terminating - no locking necessary.
 */
void
fp_free(fps)
	struct i386_fpsave_state *fps;
{
ASSERT_IPL(SPL0);
	/*
	 * If the area being freed is the live state of the thread running
	 * right now, the registers still hold it.  Empty them: a freed area's
	 * contents must not be readable from the unit afterwards, and there
	 * is no longer a #NM trap between here and the next user of the
	 * registers that would have replaced them.
	 *
	 * 🔑 This used to ask fp_act, and therefore only on a uniprocessor.
	 * The question is now "is it the current thread's", which has the
	 * same answer on both -- the registers hold the current thread's
	 * state and nobody else's.
	 */
	if (current_act() != THR_ACT_NULL &&
	    current_act()->mact.pcb->ims.ifps == fps) {
		fwait();		/* wait for a possible interrupt */
		clear_fpu();
	}
	zfree(ifps_zone, (vm_offset_t) fps);
}

/*
 * Set the floating-point state for a thread.
 * If the thread is not the current thread, it is
 * not running (held).  Locking needed against
 * concurrent fpu_set_state or fpu_get_state.
 */
kern_return_t
fpu_set_state(
	thread_act_t		thr_act,
	struct i386_float_state	*state)
{
	register pcb_t	pcb;
	register struct i386_fpsave_state *ifps;
	register struct i386_fpsave_state *new_ifps;

ASSERT_IPL(SPL0);
	if (fp_kind == FP_NO)
	    return KERN_FAILURE;

	assert(thr_act != THR_ACT_NULL);
	pcb = thr_act->mact.pcb;

	/*
	 * If this thread`s state is in the FPU, discard it; we are replacing
	 * the entire FPU state.  (Asked of current_act() rather than of a
	 * lazy owner -- see fp_free.)
	 */
	if (thr_act == current_act()) {
	    fwait();			/* wait for possible interrupt */
	    clear_fpu();		/* no state in FPU */
	}

	if (state->initialized == 0) {
	    /*
	     * new FPU state is 'invalid'.
	     * Deallocate the fp state if it exists.
	     */
	    simple_lock(&pcb->lock);
	    ifps = pcb->ims.ifps;
	    pcb->ims.ifps = 0;
	    simple_unlock(&pcb->lock);

	    if (ifps != 0) {
		zfree(ifps_zone, (vm_offset_t) ifps);
	    }
	}
	else {
	    /*
	     * Valid state.  Allocate the fp state if there is none.
	     */
	    register struct i386_fp_save *user_fp_state;
	    register struct i386_fp_regs *user_fp_regs;

	    user_fp_state = (struct i386_fp_save *) &state->hw_state[0];
	    user_fp_regs  = (struct i386_fp_regs *)
			&state->hw_state[sizeof(struct i386_fp_save)];

	    new_ifps = 0;
	    for (;;) {
		simple_lock(&pcb->lock);
		ifps = pcb->ims.ifps;
		if (ifps == 0 && new_ifps == 0) {
		    simple_unlock(&pcb->lock);
		    new_ifps = (struct i386_fpsave_state *) zalloc(ifps_zone);
		    continue;
		}
		break;
	    }
	    if (ifps == 0) {
		ifps = new_ifps;
		new_ifps = 0;
		pcb->ims.ifps = ifps;
	    }

	    /*
	     * Ensure that reserved parts of the environment are 0.
	     */
	    if (fp_kind == FP_XSAVE)
		bzero((char *)&ifps->fx_save_state, xsave_area_size);
	    else
		bzero((char *)&ifps->fx_save_state, sizeof(struct i386_fx_save));

	    ifps->fx_save_state.fx_control = user_fp_state->fp_control;
	    ifps->fx_save_state.fx_status  = user_fp_state->fp_status;
	    /*
	     * Convert full tag word (FSAVE, 16-bit, 2 bits per reg)
	     * to abridged tag word (FXSAVE, 8-bit, 1 bit per reg).
	     * In FXSAVE: bit=0 means empty, bit=1 means valid.
	     * In FSAVE: 11 means empty, anything else means valid.
	     */
	    {
		unsigned short full_tag = user_fp_state->fp_tag;
		unsigned char abridged = 0;
		int i;
		for (i = 0; i < 8; i++) {
		    if (((full_tag >> (i * 2)) & 3) != 3)
			abridged |= (1 << i);
		}
		ifps->fx_save_state.fx_tag = abridged;
	    }
	    ifps->fx_save_state.fx_eip     = user_fp_state->fp_eip;
	    ifps->fx_save_state.fx_cs      = user_fp_state->fp_cs;
	    ifps->fx_save_state.fx_opcode  = user_fp_state->fp_opcode;
	    ifps->fx_save_state.fx_dp      = user_fp_state->fp_dp;
	    ifps->fx_save_state.fx_ds      = user_fp_state->fp_ds;
	    ifps->fx_save_state.fx_MXCSR   = MXCSR_DEFAULT;

	    /*
	     * Copy FP registers: FSAVE has 10 bytes per reg packed,
	     * FXSAVE has 16 bytes per reg (10 used + 6 reserved).
	     */
	    {
		int i;
		unsigned char *src = (unsigned char *)user_fp_regs;
		for (i = 0; i < 8; i++) {
		    bcopy(src + i * 10,
			  ifps->fx_save_state.fx_reg_word[i],
			  10);
		}
	    }

	    /*
	     * For XSAVE: set xstate_bv to indicate which components
	     * are present.  We set x87 + SSE since we initialized
	     * the legacy area and MXCSR.
	     */
	    if (fp_kind == FP_XSAVE) {
		struct i386_xsave_header *hdr =
		    (struct i386_xsave_header *)
		    ((unsigned char *)&ifps->fx_save_state + XSAVE_HDR_OFFSET);
		hdr->xstate_bv_lo = XCR0_X87 | XCR0_SSE;
		hdr->xstate_bv_hi = 0;
	    }

	    /*
	     * The state lives in memory and the registers do not have it.
	     *
	     * 🔴 This was never written down, and it mattered: ifps can come
	     * straight from zalloc(), which does not zero, so fp_valid held
	     * whatever the last user of that element left -- including the 2
	     * that means "a floating-point exception is pending for this
	     * thread".  Under the lazy scheme fp_load() read it on the #NM;
	     * the switch reads it now, on every thread.
	     */
	    ifps->fp_valid = TRUE;

	    simple_unlock(&pcb->lock);
	    if (new_ifps != 0)
		zfree(ifps_zone, (vm_offset_t) ifps);

	    /*
	     * If we just replaced the state of the thread running right now,
	     * put it in the registers: it used to get there from the #NM
	     * trap that clear_fpu()'s CR0.TS would have caused, and there is
	     * no such trap any more (#560).
	     */
	    if (thr_act == current_act())
		fpu_load_context(thr_act);
	}

	return KERN_SUCCESS;
}

/*
 * Get the floating-point state for a thread.
 * If the thread is not the current thread, it is
 * not running (held).  Locking needed against
 * concurrent fpu_set_state or fpu_get_state.
 */
kern_return_t
fpu_get_state(
	thread_act_t				thr_act,
	register struct i386_float_state	*state)
{
	register pcb_t	pcb;
	register struct i386_fpsave_state *ifps;

ASSERT_IPL(SPL0);
	if (fp_kind == FP_NO)
	    return KERN_FAILURE;

	assert(thr_act != THR_ACT_NULL);
	pcb = thr_act->mact.pcb;

	simple_lock(&pcb->lock);
	ifps = pcb->ims.ifps;
	if (ifps == 0) {
	    /*
	     * No valid floating-point state.
	     */
	    simple_unlock(&pcb->lock);
	    bzero((char *)state, sizeof(struct i386_float_state));
	    return KERN_SUCCESS;
	}

	/* Make sure we`ve got the latest fp state info */
	/* If the live fpu state belongs to our target */
	if (thr_act == current_act()) {
	    fp_save(thr_act);
	    /*
	     * 🔴 The registers are NOT emptied here, and that is a change.
	     * clear_fpu() used to arm CR0.TS, which cost nothing because the
	     * thread's next use of the unit trapped and reloaded from the
	     * copy fp_save() had just made.  With the restore eager there is
	     * no reload before the thread goes on running, so emptying them
	     * would throw away the state we were asked to READ.
	     *
	     * They stay live, which means the memory copy is stale again --
	     * otherwise fpu_save_context() would skip the save at the next
	     * switch and lose whatever the thread does from here.
	     */
	    ifps->fp_valid = FALSE;
	}

	state->fpkind = fp_kind;
	state->exc_status = 0;

	{
	    register struct i386_fp_save *user_fp_state;
	    register struct i386_fp_regs *user_fp_regs;

	    state->initialized = ifps->fp_valid;

	    user_fp_state = (struct i386_fp_save *) &state->hw_state[0];
	    user_fp_regs  = (struct i386_fp_regs *)
			&state->hw_state[sizeof(struct i386_fp_save)];

	    /*
	     * Ensure that reserved parts of the environment are 0.
	     */
	    bzero((char *)user_fp_state,  sizeof(struct i386_fp_save));

	    user_fp_state->fp_control = ifps->fx_save_state.fx_control;
	    user_fp_state->fp_status  = ifps->fx_save_state.fx_status;
	    /*
	     * Convert abridged tag word (FXSAVE, 8-bit) back to
	     * full tag word (FSAVE, 16-bit, 2 bits per reg).
	     * Abridged: bit=0 means empty (11), bit=1 means valid (00).
	     */
	    {
		unsigned char abridged = ifps->fx_save_state.fx_tag;
		unsigned short full_tag = 0;
		int i;
		for (i = 0; i < 8; i++) {
		    if (!(abridged & (1 << i)))
			full_tag |= (3 << (i * 2));  /* empty */
		    /* else: 00 = valid */
		}
		user_fp_state->fp_tag = full_tag;
	    }
	    user_fp_state->fp_eip     = ifps->fx_save_state.fx_eip;
	    user_fp_state->fp_cs      = ifps->fx_save_state.fx_cs;
	    user_fp_state->fp_opcode  = ifps->fx_save_state.fx_opcode;
	    user_fp_state->fp_dp      = ifps->fx_save_state.fx_dp;
	    user_fp_state->fp_ds      = ifps->fx_save_state.fx_ds;

	    /*
	     * Copy FP registers: FXSAVE has 16 bytes per reg,
	     * FSAVE expects 10 bytes per reg packed.
	     */
	    {
		int i;
		unsigned char *dst = (unsigned char *)user_fp_regs;
		for (i = 0; i < 8; i++) {
		    bcopy(ifps->fx_save_state.fx_reg_word[i],
			  dst + i * 10,
			  10);
		}
	    }
	}
	simple_unlock(&pcb->lock);

	return KERN_SUCCESS;
}

/*
 * Initialize FPU.
 *
 * Raise exceptions for:
 *	invalid operation
 *	divide by zero
 *	overflow
 *
 * Use 53-bit precision.
 */
void
fpinit(void)
{
	unsigned short	control;

ASSERT_IPL(SPL0);
	clear_ts();
	fninit();
	fnstcw(&control);
	control &= ~(FPC_PC|FPC_RC); /* Clear precision & rounding control */
	control |= (FPC_PC_53 |		/* Set precision */ 
			FPC_RC_RN | 	/* round-to-nearest */
			FPC_ZE |	/* Suppress zero-divide */
			FPC_OE |	/*  and overflow */
			FPC_UE |	/*  underflow */
			FPC_IE |	/* Allow NaNQs and +-INF */
			FPC_DE |	/* Allow denorms as operands  */
			FPC_PE);	/* No trap for precision loss */
	fldcw(control);

	/*
	 * Initialize MXCSR: mask all SSE exceptions (default safe state).
	 */
	if (fp_kind >= FP_FXSR) {
	    unsigned int mxcsr = MXCSR_DEFAULT;
	    __asm__ volatile("ldmxcsr %0" : : "m" (mxcsr));
	}
}

/*
 * Coprocessor not present.
 */

void
fpnoextflt(void)
{
ASSERT_IPL(SPL0);
	/*
	 * 🔴 #560: this is no longer a route to anything.
	 *
	 * It used to be THE restore: the switch armed CR0.TS, the new
	 * thread's first floating-point instruction trapped here, and this
	 * function took the unit away from whoever had it and loaded the
	 * current thread's state.  That deferral is what left one thread's
	 * registers readable to the next one (CVE-2018-3665), so the switch
	 * carries the state now and CR0.TS is never armed.
	 *
	 * What is left for this handler is the case it is actually named
	 * for -- "coprocessor not present".  Two things can still raise a
	 * #NM and neither is a context switch:
	 *
	 *  - CR0.EM is set, which init_fpu() does only when it found no FPU
	 *    at all.  The thread asked for an instruction this machine does
	 *    not have, and gets told so.
	 *
	 *  - CR0.TS is set, which nothing does any more.  If that happens
	 *    the invariant this file now rests on has been broken somewhere
	 *    else, and loading a thread's state here would paper over it:
	 *    the registers would be right and the reason would be gone.
	 */
	if (fp_kind == FP_NO) {
	    i386_exception(EXC_BAD_INSTRUCTION, EXC_I386_NOEXTFLT, 0);
	    /*NOTREACHED*/
	}

	panic("fpnoextflt: #NM with an FPU present and CR0.TS clear (cr0=0x%x)",
	      get_cr0());
}

/*
 * FPU overran end of segment.
 * Re-initialize FPU.  Floating point state is not valid.
 */

void
fpextovrflt(void)
{
	register thread_act_t	thr_act = current_act();
	register pcb_t		pcb;
	register struct i386_fpsave_state *ifps;

	/*
	 * The check that used to be here asked whether the exception was for
	 * the thread running right now, by comparing against the lazy owner.
	 * It is gone with the owner: the unit holds the current thread's
	 * state and nobody else's, so the answer is yes by construction
	 * (#560).
	 */

	/*
	 * This is a non-recoverable error.
	 * Invalidate the thread`s FPU state.
	 */
	pcb = thr_act->mact.pcb;
	simple_lock(&pcb->lock);
	ifps = pcb->ims.ifps;
	pcb->ims.ifps = 0;
	simple_unlock(&pcb->lock);

	/*
	 * Re-initialize the FPU.  The thread has no save area any more, so
	 * the registers must not be left holding what it had: the next
	 * switch to it will load the clean image, but it is still running
	 * until the exception below unwinds it.
	 */
	fninit();

	if (ifps)
	    zfree(ifps_zone, (vm_offset_t) ifps);

	/*
	 * Raise exception.
	 */
	i386_exception(EXC_BAD_ACCESS, VM_PROT_READ|VM_PROT_EXECUTE, 0);
	/*NOTREACHED*/
}

/*
 * FPU error. Called by AST.
 */

void
fpexterrflt(void)
{
	register thread_act_t	thr_act = current_act();

	int			mycpu;
	register thread_act_t	owner;

ASSERT_IPL(SPL0);
	mp_disable_preemption();
	mycpu = cpu_number();
	owner = fp_intr_act[mycpu];
	fp_intr_act[mycpu] = THR_ACT_NULL;
	mp_enable_preemption();

	if (owner == THR_ACT_NULL)
		panic("fpexterrflt: AST taken with no thread recorded");

	/*
	 * A context switch can happen between the interrupt and the AST, and
	 * the AST is per-CPU while the error belongs to a thread -- so the
	 * thread that reaches this point is not necessarily the one the
	 * error is for.
	 *
	 * 🔑 The remembered condition (fp_valid == 2) used to be delivered by
	 * fp_load(), off the #NM trap the owner would take the next time it
	 * touched the unit.  There is no such trap now, so fpu_load_context()
	 * delivers it: the restore is the one thing that is guaranteed to
	 * happen before that thread runs again (#560).
	 *
	 * ⚠️ This whole arm used to be uniprocessor-only, which left SMP
	 * raising the arithmetic exception against whatever thread the AST
	 * happened to land on.
	 */
	if (owner != thr_act) {
		register struct i386_fpsave_state *ifps;

		ifps = owner->mact.pcb->ims.ifps;
		if (ifps != 0)
			ifps->fp_valid = 2;
		/*
		 * If it has no save area the error dies with it: the thread
		 * was told its state is uninitialized, or it is on its way
		 * out.  Nothing to raise it against.
		 */
		return;
	}

	/*
	 * Save the FPU state: the registers are the live copy, and the
	 * status word read below has to be the one that faulted.
	 */
	fp_save(thr_act);

	/*
	 * Raise FPU exception.
	 * Locking not needed on pcb->ims.ifps,
	 * since thread is running.
	 */
	i386_exception(EXC_ARITHMETIC,
		       EXC_I386_EXTERR,
		       thr_act->mact.pcb->ims.ifps->fx_save_state.fx_status);
	/*NOTREACHED*/
}

/*
 * SIMD numeric error -- #XF, vector 19 (#515).
 *
 * 🔴 There has been nothing here at all.  init_fpu() has set CR4.OSXMMEXCPT
 * since SSE was enabled on this target, and that bit is precisely what makes
 * an unmasked SIMD exception arrive as this fault instead of as an invalid
 * opcode -- while <i386/trap.h> stopped at 17, so user_trap() fell through to
 * its default and panicked.  Two instructions from ring 3, `ldmxcsr' with a
 * mask bit cleared and a divide by zero, stopped the machine.
 *
 * 🔑 Nothing like fpexterrflt()'s machinery above is needed, and the reason is
 * architectural rather than a simplification: #XF is PRECISE.  It is raised by
 * the instruction that caused it, in the thread that executed it, at the
 * moment it executed it -- so there is no interval in which a switch can hand
 * it to somebody else, and nothing to remember between the fault and the
 * report.  Every line of the deferred-error machinery exists because the x87's
 * report is NOT precise and, on this kernel, does not even arrive as a fault.
 *
 * ⚠️ And the MXCSR exception flags are deliberately NOT cleared, which is the
 * opposite of what the x87 path has to do.  There the flag is what raises the
 * fault, so leaving it set means the thread faults again at the same wait for
 * ever; here the DIVISION raises it, the flag is only a record, and a handler
 * that resumes this thread re-executes the division whatever we do.  Clearing
 * would destroy the one piece of evidence and change nothing.
 */
void
fpsseflt(void)
{
	register thread_act_t	thr_act = current_act();

ASSERT_IPL(SPL0);
	/*
	 * The registers are the live copy and MXCSR is in them, so the value
	 * reported has to come from a save made here rather than from whatever
	 * the last switch happened to leave in memory.
	 */
	fp_save(thr_act);

	i386_exception(EXC_ARITHMETIC,
		       EXC_I386_SSEFLT,
		       thr_act->mact.pcb->ims.ifps->fx_save_state.fx_MXCSR);
	/*NOTREACHED*/
}

/*
 * Save FPU state.
 *
 * Locking not needed:
 * .	if called from fpu_get_state, pcb already locked.
 * .	if called from fpnoextflt or fp_intr, we are single-cpu
 * .	otherwise, thread is running.
 */

void
fp_save(
	thread_act_t	thr_act)
{
	register pcb_t pcb = thr_act->mact.pcb;
	register struct i386_fpsave_state *ifps = pcb->ims.ifps;

	if (ifps != 0 && !ifps->fp_valid) {
	    /* registers are in FPU */
	    ifps->fp_valid = TRUE;
	    if (fp_kind == FP_XSAVE)
		xsave(&ifps->fx_save_state);
	    else
		fxsave(&ifps->fx_save_state);
	}
}

/*
 * Restore FPU state from PCB.
 *
 * Locking not needed; always called on the current thread.
 */

void
fp_load(
	thread_act_t	thr_act)
{
	register pcb_t pcb = thr_act->mact.pcb;
	register struct i386_fpsave_state *ifps;

ASSERT_IPL(SPL0);
	ifps = pcb->ims.ifps;
	if (ifps == 0) {
	    unsigned int clear_size;
	    ifps = (struct i386_fpsave_state *) zalloc(ifps_zone);
	    /* Clear fp_valid + fp_pad + save area */
	    clear_size = 64;  /* fp_valid + fp_pad */
	    if (fp_kind == FP_XSAVE)
		clear_size += xsave_area_size;
	    else
		clear_size += sizeof(struct i386_fx_save);
	    bzero((char *)ifps, clear_size);
	    pcb->ims.ifps = ifps;
	    fpinit();
#if 1
/* 
 * I'm not sure this is needed. Does the fpu regenerate the interrupt in
 * frstor or not? Without this code we may miss some exceptions, with it
 * we might send too many exceptions.
 */
	} else if (ifps->fp_valid == 2) {
		/* delayed exception pending */

		ifps->fp_valid = TRUE;
		clear_fpu();
		/*
		 * Raise FPU exception.
		 * Locking not needed on pcb->ims.ifps,
		 * since thread is running.
		 */
		i386_exception(EXC_ARITHMETIC,
		       EXC_I386_EXTERR,
		       thr_act->mact.pcb->ims.ifps->fx_save_state.fx_status);
		/*NOTREACHED*/
#endif
	} else {
	    if (fp_kind == FP_XSAVE)
		xrstor(&ifps->fx_save_state);
	    else
		fxrstor(&ifps->fx_save_state);
	}
	ifps->fp_valid = FALSE;		/* in FPU */
}

/*
 * Save the outgoing thread's FPU state.  Called from switch_context() and
 * machine_switch_act(), before the map and the pcb change.
 *
 * 🔴 This does NOT arm CR0.TS, and that absence is the point of #560: the
 * registers must not be left holding this thread's data while another one
 * runs.  fpu_load_context() below puts something else in them.
 */
void
fpu_save_context(
	thread_t	thread)
{
	register struct i386_fpsave_state *ifps;

	ifps = thread->top_act->mact.pcb->ims.ifps;
	if (ifps == 0 || ifps->fp_valid)
	    return;

	/* registers are in the FPU - save to memory */
	ifps->fp_valid = TRUE;
	if (fp_kind == FP_XSAVE)
	    xsave(&ifps->fx_save_state);
	else
	    fxsave(&ifps->fx_save_state);
}

/*
 * Load the incoming thread's FPU state.  Called from
 * act_machine_switch_pcb(), which is the tail of every switch.
 *
 * 🔑 The place for this call has always existed and has always been right
 * here -- it was fpu_load_context(), an empty macro.  What was missing was
 * the restore, not somewhere to put it.
 */
void
fpu_load_context(
	thread_act_t	thr_act)
{
	register struct i386_fpsave_state *ifps;

	ifps = thr_act->mact.pcb->ims.ifps;
	if (ifps == 0)
	    /*
	     * This thread has no state of its own, and the registers at this
	     * moment hold the thread we are switching away from.  Leaving
	     * them there is exactly the leak, so they get the clean image.
	     *
	     * Reached in earnest: the thread setup_main() runs on never went
	     * through pcb_init(), and fpu_set_state() frees the area of a
	     * thread told its state is uninitialized.
	     */
	    ifps = fp_clean_state;
	if (ifps == 0)
	    /*
	     * Before fpu_module_init() built the clean image.  Nothing has
	     * had FPU state yet at this point, so there is none to leave.
	     */
	    return;

	if (ifps->fp_valid == 2) {
	    /*
	     * A floating-point error was raised for this thread while
	     * another one was running, and the AST that would have carried
	     * it was consumed by that other thread (fpexterrflt).  fp_load()
	     * used to raise the exception from the #NM trap, which is a
	     * place where one CAN be raised; here we are inside the switch
	     * with preemption disabled, and it is not.
	     *
	     * So it is re-armed against the thread being switched TO, which
	     * is the one it belongs to, and i386_astintr() delivers it on
	     * the way out to user mode.
	     */
	    ifps->fp_valid = TRUE;
	    fp_intr_act[cpu_number()] = thr_act;
	    ast_on(cpu_number(), AST_I386_FP);
	}

	if (fp_kind == FP_XSAVE)
	    xrstor(&ifps->fx_save_state);
	else
	    fxrstor(&ifps->fx_save_state);

	/*
	 * A thread's own area is now stale -- the registers are the live
	 * copy.  The clean image is shared and read-only in practice, so it
	 * stays marked valid.
	 */
	if (ifps != fp_clean_state)
	    ifps->fp_valid = FALSE;		/* in FPU */
}

/*
 * Allocate and initialize FP state for a thread's pcb.  Don't load state.
 *
 * #560: this used to be reachable only for the thread running right now,
 * because the only thing that ever called it was the emulator linkage and,
 * on real hardware, fp_load() off the #NM trap.  The switch now carries the
 * state eagerly, so there is no #NM to allocate on, and pcb_init() asks for
 * the area when the thread is built instead.  That is a thread which is not
 * current and is not yet running, hence the pcb argument.
 *
 * Locking not needed: the caller either owns the current thread's pcb, or
 * holds a pcb nobody else can reach yet.
 */
void
fp_state_alloc_pcb(
	pcb_t	pcb)
{
	struct i386_fpsave_state *ifps;

	if (fp_clean_state == 0)
	    panic("fp_state_alloc_pcb: no clean image yet");

	/*
	 * A copy of the image the unit gave us after fpinit(), which is the
	 * state a thread used to start with.
	 *
	 * 🔴 THIS USED TO BUILD THE IMAGE BY HAND, and doing that for every
	 * thread would have changed what every thread starts with.  The hand
	 * built control word was
	 *
	 *	(0x037f & ~(FPC_IM|FPC_ZM|FPC_OM|FPC_PC)) | FPC_PC_53|FPC_IC_AFF
	 *
	 * which leaves invalid-operation, zero-divide and overflow UNMASKED.
	 * Nothing reached it: the only caller was the emulator linkage, and on
	 * real hardware a thread's first use of the unit trapped into
	 * fp_load(), which called fpinit() -- and fpinit() masks everything.
	 * So threads have always started masked, and allocating this at
	 * pcb_init() for every one of them would have silently made a
	 * floating-point divide by zero trap where it used to answer infinity.
	 *
	 * 🔑 Taking the image FROM the unit rather than writing one is also
	 * why it is right: it is whatever this processor produces after
	 * fpinit(), including the XSAVE header, rather than a second opinion
	 * about what that should look like.
	 */
	ifps = (struct i386_fpsave_state *) zalloc(ifps_zone);
	bcopy((char *) fp_clean_state, (char *) ifps, ifps_size);
	ifps->fp_valid = TRUE;		/* in memory, not in the unit */

	pcb->ims.ifps = ifps;
}

/*
 * Allocate and initialize FP state for the current thread.  The emulator
 * linkage asks for it this way; everything else names the pcb.
 */
void
fp_state_alloc(void)
{
	fp_state_alloc_pcb(current_act()->mact.pcb);
}


/*
 * fpflush(thread_act_t)
 *	Flush the current act's state, if needed
 *	(used by thread_terminate_self to ensure fp faults
 *	aren't satisfied by overly general trap code in the
 *	context of the reaper thread)
 *
 * ⚠️ The SMP arm of this used to read "not needed on MP x86s; fp not lazily
 * evaluated", which was not true: on MP the SAVE was eager and the RESTORE
 * was as lazy as anywhere else.  A comment asserting the property that was
 * missing is how the property stayed missing (#560).
 */
void
fpflush(thread_act_t thr_act)
{
	/*
	 * The registers hold the current thread's state, so they hold this
	 * thread's only if it is the current one.  Empty them, so that a
	 * floating-point fault raised after this point cannot be answered
	 * with a dying thread's data.
	 */
	if (thr_act == current_act()) {
	    fwait();
	    clear_fpu();
	}
}


#if	AT386
/*
 *	Handle a coprocessor error interrupt on the AT386.
 *	This comes in on line 5 of the slave PIC at SPL1.
 */

void
fpintr(void)
{
	spl_t	s;
	thread_act_t thr_act = current_act();

ASSERT_IPL(SPL1);
	/*
	 * Turn off the extended 'busy' line.
	 */
	outb(0xf0, 0);

	/*
	 * Save the FPU context to the thread using it.
	 *
	 * 🔑 Which thread that is no longer has to be looked up.  The two
	 * arms this used to open with -- "the unit belongs to nobody" and
	 * "the unit belongs to a thread that is not running" -- were both
	 * consequences of the lazy restore, and neither can arise now: the
	 * registers hold the current thread's state (#560).
	 */
	fp_save(thr_act);

	/*
	 * Clear the error condition in the unit so it is not signalled
	 * again.  The saved copy keeps the status word that faulted, which
	 * is what fpexterrflt() reports, and it stays the authoritative copy
	 * -- the next switch to this thread restores from it.
	 */
	fninit();

	/*
	 * Since we are running on the interrupt stack, we must
	 * signal the thread to take the exception when we return
	 * to user mode.  Use an AST to do this.
	 *
	 * ⚠️ The AST is per-CPU and the error belongs to a thread, so which
	 * thread it was is recorded alongside it.  If a switch happens
	 * before the AST is taken, fpexterrflt() hands the condition back to
	 * the owner through fp_valid == 2, and fpu_load_context() delivers
	 * it -- the restore being the one thing guaranteed to happen before
	 * that thread runs again.  It used to be the #NM trap, which no
	 * longer comes.
	 */
	s = splsched();
	mp_disable_preemption();
	fp_intr_act[cpu_number()] = thr_act;
	ast_on(cpu_number(), AST_I386_FP);
	mp_enable_preemption();
	splx(s);
}
#endif	/* AT386 */
