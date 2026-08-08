/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Ring 3, entered on purpose (#411, MD contract 6/6).
 *
 * Everything this kernel does happens in ring 0, and that is the reason the
 * syscall path cannot be tested: a syscall is an instruction whose entire
 * meaning is the privilege change, so exercising it from ring 0 would prove
 * that the instruction exists and nothing else.  There has to be somewhere
 * less privileged to come from first.
 *
 * So this is a few instructions of genuine user-mode code, living in the
 * image and copied into a page an address space owns, entered the way any
 * return to user mode is entered.  It is not a simulation of ring 3 and it
 * is not a stub: the processor really does drop privilege, and what it does
 * next it does with no rights.
 *
 * It goes away when there is a real userland (#414, #421).  Until then it is
 * the only ring-3 code that exists, and the only thing capable of proving
 * that the mechanisms built for ring 3 work.
 */

#ifndef _X86_64_SYSCALL_PROBE_H_
#define _X86_64_SYSCALL_PROBE_H_

/*
 * Where the probe is mapped in the address space it runs in.  Two pages,
 * chosen in the lower half well clear of anything the boot identity map
 * covers, so nothing has to be moved to make room.
 *
 * The data page carries the witness at its base and the user stack at its
 * top — one page doing both, which is safe here because the probe pushes
 * nothing and could not reach across the page if it tried.
 */
#define USER_PROBE_CODE_VA	0x0000700000000000ULL
#define USER_PROBE_DATA_VA	0x0000700000001000ULL
#define USER_PROBE_STACK_TOP	(USER_PROBE_DATA_VA + 0x1000)

/*
 * What the probe writes before it faults.
 *
 * A recognisable value rather than a flag, so that finding it proves the
 * store came from the probe rather than from a page that happened to be
 * non-zero.  Finding it also proves something the fault alone would not:
 * that user code *ran*, rather than faulting on entry and never executing
 * an instruction at all.
 */
#define USER_PROBE_WITNESS	0x5eeded1eafULL

/*
 * The arguments the probe passes, and the answer it must get back.
 *
 * One to six, one per byte, lowest argument in the lowest byte — so the
 * result names every register individually.  A sum would be shorter and
 * would hide the failure worth catching: two arguments delivered into each
 * other's registers add to the same total, and putting six registers in the
 * right places is the entry path's entire job.
 */
#define USER_PROBE_SYSCALL_RESULT	0x060504030201ULL

#ifndef __ASSEMBLER__

#include <stdint.h>

/* The probe's instructions, to be copied into the page that will run them. */
extern char __user_probe_start[], __user_probe_end[];

/*
 * The second entry point in that same page (#440): two instructions asking
 * whether ring 3 may read its own segment base.
 *
 * Its address here is the one it has in the kernel image, which is not the
 * one it will have when it runs — so what is useful is the distance from the
 * start of the section, and the caller adds that to USER_PROBE_CODE_VA.
 */
extern char user_probe_fsgsbase[];

#define USER_PROBE_FSGSBASE_VA					\
	(USER_PROBE_CODE_VA + (uint64_t)(user_probe_fsgsbase	\
					 - __user_probe_start))

/*
 * And the two addresses the entry paths must report (#409).
 *
 * The syscall saves the address of the instruction AFTER it, in rcx, and the
 * kernel pushes that; the general protection saves the address of the HLT
 * itself, because it is a fault.  Both are known here, exactly, which is what
 * turns "a fault arrived from ring 3" into "the frame describes the
 * instruction that raised it".
 */
extern char user_probe_after_syscall[], user_probe_fault[];

#define USER_PROBE_AFTER_SYSCALL_VA				\
	(USER_PROBE_CODE_VA + (uint64_t)(user_probe_after_syscall \
					 - __user_probe_start))

#define USER_PROBE_FAULT_VA					\
	(USER_PROBE_CODE_VA + (uint64_t)(user_probe_fault	\
					 - __user_probe_start))

/*
 * Where a fault from the probe comes back to.  Declared here only so the
 * kernel can recognise it in a report; the arming is done by the entry
 * itself, which is the only code that knows the stack the return needs.
 */
extern char user_probe_return[];

/*
 * Drop to ring 3 at `entry` with `stack`, and return here when the kernel
 * has taken a fault from it and sent us back.
 *
 * There is no other way back yet: the probe cannot return, because a return
 * needs a syscall and that is what this exists to make testable.  So the way
 * out is a fault, and trap_expect_user() is what turns that fault into a
 * return instead of a report and a halt.
 */
void user_probe_enter(uint64_t entry, uint64_t stack);

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_SYSCALL_PROBE_H_ */
