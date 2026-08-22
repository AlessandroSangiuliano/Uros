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
 * Revision 2.1.3.2  92/06/24  17:59:35  jeffreyh
 * 	Changes for new at386_io_lock() interface
 * 	[92/06/03            bernadat]
 * 
 * Revision 2.1.3.1  92/04/30  11:58:39  bernadat
 * 	Replaced mistyped 'l' at end of line by ; (Only build
 * 	under ODE detected it).
 * 	[92/04/30            bernadat]
 * 
 * 	MP code for AT386 platforms (Corollary, SystemPro)
 * 	[92/04/08            bernadat]
 * 
 */
/* CMU_ENDHIST */
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

/*
 */

#ifndef _I386AT_MP_H_
#define _I386AT_MP_H_

#include <cpus.h>
#include <platforms.h>
#include <mp_v1_1.h>
#include <mach_rt.h>
#include <mach_assert.h>

#if	NCPUS > 1
#if	MBUS || CBUS

/*
 * Cpu number, no local memory on CPUS, this is a hack, limited to 8 cpus,
 * on the 486, bits 0, 1, 2 and 5, 6, 7, 8, 9, 10, 11 are free. We use bits
 * 0, 1 and 2 If we want to use more cpus, we will have to use bits 5-11
 * with shifts.
 */

#define	CPU_NUMBER(r) \
	movl	%cr3, r ; \
	andl	$0x7, r

#else	/* MBUS || CBUS */
#include <i386/apic.h>
/*
 * #321: read the logical cpu number from the per-CPU data area via %gs:cpu_id.
 * The %gs base is the GDT CPU_DATA descriptor, which each CPU's GDT points at
 * its own &cpu_data[id] (mp_desc_init); the entry stubs load $CPU_DATA into %gs
 * before any per-CPU access (the #280 gs discipline), so the segment is always
 * valid here.  This is a single cached segment-relative load -- no LAPIC MMIO
 * (which traps+emulates under KVM and is uncached on bare metal), no cr3 trick
 * (which capped us at 8 CPUs and clashed with PCID).  CPU_NUMBER_FROM_LAPIC is
 * kept for the boot path (start.S) where %gs is not yet loaded.
 */
#define CPU_NUMBER(r) \
	movl	$CPU_DATA_CPU_ID, r ; \
	movl	%gs:(r), r

#define CPU_NUMBER_FROM_LAPIC(r) \
    	movl	EXT(lapic_id), r  ; \
    	movl	0(r),r		  ; \
    	shrl	$LAPIC_ID_SHIFT, r; \
    	andl	$LAPIC_ID_MASK, r ; \
    	movzbl	EXT(lapic_to_slot)(,r,1), r	/* #344: APIC ID -> dense slot */

#endif	/* MBUS || CBUS */

#define	MP_IPL		SPL6	/* software interrupt level */

/* word describing the reason for the interrupt, one per cpu */

#ifndef	ASSEMBLER
#include <kern/lock.h>
extern	int	cpu_int_word[];
extern	int	real_ncpus;	/* real number of cpus */
extern	int	wncpu;		/* wanted number of cpus */
decl_simple_lock_data(extern,kdb_lock)	/* kdb lock		*/

extern	int	kdb_cpu;		/* current cpu running kdb	*/
extern	int	kdb_debug;
extern	int	kdb_is_slave[];
extern	int	kdb_active[];
#endif	/* ASSEMBLER */

/* Interrupt types */

#define MP_TLB_FLUSH	0x00
#define MP_CLOCK	0x01
#define	MP_KDB		0x02
#define	MP_AST		0x03
#define MP_SOFTCLOCK	0x04
#define MP_INT_AVAIL	0x05
#define MP_AST_URGENT	0x06

#define i_bit(bit, word)	((long)(*(word)) & ((long)1 << (bit)))


/* 
 *	Device driver synchronization. 
 *
 *	at386_io_lock(op) and at386_io_unlock() are called
 *	by device drivers when accessing H/W. The underlying 
 *	Processing is machine dependant. But the op argument
 *	to the at386_io_lock is generic
 */

#define MP_DEV_OP_MAX	  4
#define MP_DEV_WAIT	  MP_DEV_OP_MAX	/* Wait for the lock */

/*
 * If the caller specifies an op value different than MP_DEV_WAIT, the
 * at386_io_lock function must return true if lock was successful else
 * false
 */

#define MP_DEV_OP_START 0	/* If lock busy, register a pending start op */
#define MP_DEV_OP_INTR	1	/* If lock busy, register a pending intr */
#define MP_DEV_OP_TIMEO	2	/* If lock busy, register a pending timeout */
#define MP_DEV_OP_CALLB	3	/* If lock busy, register a pending callback */

#else	/* NCPUS > 1 */
#define at386_io_lock_state()
#ifndef ASSEMBLER
#define AT386_IO_LOCK_DEFINED 1
static inline int at386_io_lock(int op __attribute__((unused))) { return 1; }
#else
#define at386_io_lock(op)	1
#endif /* ASSEMBLER */
#define at386_io_unlock()
/* Keep op constants visible even on UP so callers can compile without guards */
#define MP_DEV_OP_MAX	  4
#define MP_DEV_WAIT	  MP_DEV_OP_MAX
#define MP_DEV_OP_START	0
#define MP_DEV_OP_INTR	1
#define MP_DEV_OP_TIMEO	2
#define MP_DEV_OP_CALLB	3
#if	MP_V1_1
#include	<i386/apic.h>
#endif	/* MP_V1_1 */
#endif	/* NCPUS > 1 */

/*
 * ⚠️ EVERYTHING BELOW NEEDS MACH_RT, AND NO TARGET SETS IT.
 *
 * cmake/kernel-config.cmake gives MACH_RT the value 0 on both i386 and x86-64,
 * so the arm that compiles is the `#else' at the bottom of this file, where all
 * six macros are nothing.  i386 does not preempt in kernel mode; the machinery
 * here is switched off, not absent, and x86_64/cpu_data.h explains why that is
 * a decision rather than an omission.
 *
 * 🔴 Which is why the fork below selects on MACH_PREEMPT_DEBUG and not on
 * MACH_ASSERT, as it did until #486.  It is a choice about whether the assembly
 * routes through the checked C entry points -- debugging apparatus, in the
 * classification #485 arrived at, so its own switch and off by default.  Named
 * after assertions it read as a statement about SCHEDULING, and was read that
 * way: #485 reported that a build option decided whether i386 takes urgent ASTs
 * on interrupt exit.  It never did, because none of this compiles.
 */
#ifndef	MACH_PREEMPT_DEBUG
#define	MACH_PREEMPT_DEBUG	0
#endif

#if	MACH_RT
#define _DISABLE_PREEMPTION(r) 					\
	movl	$CPD_PREEMPTION_LEVEL,r			;	\
	incl	%gs:(r)

#define _ENABLE_PREEMPTION(r) 					\
	movl	$CPD_PREEMPTION_LEVEL,r			;	\
	decl	%gs:(r)					;	\
	jne	99f					;	\
	pushl	%eax					;	\
	pushl	%ecx					;	\
	pushl	%edx					;	\
	call	EXT(kernel_preempt_check)		;	\
	popl	%edx					;	\
	popl	%ecx					;	\
	popl	%eax					;	\
99:	

#define _ENABLE_PREEMPTION_NO_CHECK(r)				\
	movl	$CPD_PREEMPTION_LEVEL,r			;	\
	decl	%gs:(r)

#if	MACH_PREEMPT_DEBUG
#define DISABLE_PREEMPTION(r)					\
	pushl	%eax;						\
	pushl	%ecx;						\
	pushl	%edx;						\
	call	EXT(_disable_preemption);			\
	popl	%edx;						\
	popl	%ecx;						\
	popl	%eax
#define ENABLE_PREEMPTION(r)					\
	pushl	%eax;						\
	pushl	%ecx;						\
	pushl	%edx;						\
	call	EXT(_enable_preemption);			\
	popl	%edx;						\
	popl	%ecx;						\
	popl	%eax
#define ENABLE_PREEMPTION_NO_CHECK(r)				\
	pushl	%eax;						\
	pushl	%ecx;						\
	pushl	%edx;						\
	call	EXT(_enable_preemption_no_check);		\
	popl	%edx;						\
	popl	%ecx;						\
	popl	%eax
#if	NCPUS > 1
#define MP_DISABLE_PREEMPTION(r)					\
	pushl	%eax;						\
	pushl	%ecx;						\
	pushl	%edx;						\
	call	EXT(_mp_disable_preemption);			\
	popl	%edx;						\
	popl	%ecx;						\
	popl	%eax
#define MP_ENABLE_PREEMPTION(r)					\
	pushl	%eax;						\
	pushl	%ecx;						\
	pushl	%edx;						\
	call	EXT(_mp_enable_preemption);			\
	popl	%edx;						\
	popl	%ecx;						\
	popl	%eax
#define MP_ENABLE_PREEMPTION_NO_CHECK(r)				\
	pushl	%eax;						\
	pushl	%ecx;						\
	pushl	%edx;						\
	call	EXT(_mp_enable_preemption_no_check);		\
	popl	%edx;						\
	popl	%ecx;						\
	popl	%eax
#else	/* NCPUS > 1 */
#define MP_DISABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION_NO_CHECK(r)
#endif	/* NCPUS > 1 */
#else	/* MACH_PREEMPT_DEBUG */
#define DISABLE_PREEMPTION(r)			_DISABLE_PREEMPTION(r)
#define ENABLE_PREEMPTION(r)			_ENABLE_PREEMPTION(r)
#define ENABLE_PREEMPTION_NO_CHECK(r)		_ENABLE_PREEMPTION_NO_CHECK(r)
#if	NCPUS > 1
#define MP_DISABLE_PREEMPTION(r)		_DISABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION(r)			_ENABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION_NO_CHECK(r) 	_ENABLE_PREEMPTION_NO_CHECK(r)
#else	/* NCPUS > 1 */
#define MP_DISABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION_NO_CHECK(r)
#endif	/* NCPUS > 1 */
#endif	/* MACH_PREEMPT_DEBUG */

#else	/* MACH_RT */
#define DISABLE_PREEMPTION(r)
#define ENABLE_PREEMPTION(r)
#define ENABLE_PREEMPTION_NO_CHECK(r)
#define MP_DISABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION(r)
#define MP_ENABLE_PREEMPTION_NO_CHECK(r)
#endif	/* MACH_RT */

#endif /* _I386AT_MP_H_ */
