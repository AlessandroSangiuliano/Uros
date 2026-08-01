/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <mach/machine/vm_param.h> for x86-64 (#450).
 *
 * The tenth and last of the mach/machine/ headers; #413 landed the other
 * nine.  Sixteen machine-independent sources stop on this one.
 *
 * It is the public half of the address-space layout #405 decided and #407
 * implements: page size and the bounds of the two halves.  The private half
 * lives in <pmap/layout.h>, which the kernel builds against, and the two
 * have to agree about where the kernel half starts.  Two constants that must
 * match is exactly the shape of defect this port keeps finding, so they are
 * checked against each other below rather than kept matching by attention.
 *
 * The user half ends at 2^47 and the kernel half begins at the far end of
 * the canonical hole: with 48-bit virtual addresses the middle of the range
 * is not addressable at all, so unlike i386 these two bounds do not meet.
 * Code that walks from VM_MAX_ADDRESS expecting to arrive at
 * VM_MIN_KERNEL_ADDRESS is wrong here, and that is a property of the
 * architecture rather than of this layout.
 */

#ifndef _MACH_X86_64_VM_PARAM_H_
#define _MACH_X86_64_VM_PARAM_H_

#define BYTE_SIZE		8	/* byte size in bits */

#define X86_64_PGBYTES		4096	/* bytes per page */
#define X86_64_PGSHIFT		12	/* bits to shift for pages */

#define x86_64_btop(x)		(((unsigned long)(x)) >> X86_64_PGSHIFT)
#define x86_64_ptob(x)		(((unsigned long)(x)) << X86_64_PGSHIFT)
#define machine_btop(x)		x86_64_btop(x)

#define x86_64_round_page(x)	((((unsigned long)(x)) + X86_64_PGBYTES - 1) \
					& ~(X86_64_PGBYTES - 1))
#define x86_64_trunc_page(x)	(((unsigned long)(x)) & ~(X86_64_PGBYTES - 1))

/*
 * The canonical low half: 2^47 bytes of user address space.  The hardware
 * rejects anything between here and VM_MIN_KERNEL_ADDRESS.
 */
#define VM_MIN_ADDRESS		((vm_offset_t) 0)
#define VM_MAX_ADDRESS		((vm_offset_t) 0x0000800000000000UL)

#define VM_MIN_KERNEL_ADDRESS	((vm_offset_t) 0xffff800000000000UL)
#define VM_MAX_KERNEL_ADDRESS	((vm_offset_t) 0xffffffffffffffffUL)

/*
 * Where a loaded kernel server may sit: the top 2 GiB, which is also the
 * only range -mcmodel=kernel can reach with a 32-bit displacement.
 */
#define VM_MIN_KERNEL_LOADED_ADDRESS	((vm_offset_t) 0xffffffff80000000UL)
#define VM_MAX_KERNEL_LOADED_ADDRESS	((vm_offset_t) 0xffffffffffffffffUL)

#ifdef	MACH_KERNEL
#include <norma_vm.h>
#include <xkmachkernel.h>
#include <task_swapper.h>
#include <thread_swapper.h>

/*
 * The agreement promised above, made into a build failure.  <pmap/layout.h>
 * carries the same boundary for the code that maps it; if the two are ever
 * edited apart, this stops the build instead of letting the public header
 * and the page tables describe different machines.
 */
#include <pmap/layout.h>
_Static_assert(VM_MIN_KERNEL_ADDRESS == (vm_offset_t) KERNEL_HALF_BASE,
	       "mach/x86_64/vm_param.h and x86_64/pmap/layout.h disagree "
	       "about where the kernel half begins");

/*
 * A full page rather than i386's half.  Every saved register doubled in
 * width and there are four more of them, so the frames this stack has to
 * hold are the reason, not caution.
 */
#if !NORMA_VM && !XKMACHKERNEL
#define KERNEL_STACK_SIZE	(X86_64_PGBYTES * 4)
#define INTSTACK_SIZE		(X86_64_PGBYTES * 4)
#else
#define KERNEL_STACK_SIZE	(X86_64_PGBYTES * 8)
#define INTSTACK_SIZE		(X86_64_PGBYTES * 8)
#endif
#endif	/* MACH_KERNEL */

#define trunc_x86_64_to_vm(p)	(atop(trunc_page(x86_64_ptob(p))))
#define round_x86_64_to_vm(p)	(atop(round_page(x86_64_ptob(p))))
#define vm_to_x86_64(p)		(x86_64_btop(ptoa(p)))

/*
 * There is no high memory here, so every physical page is low (#453).
 *
 * On i386 pa_is_lowmem() separates the physical memory the kernel can reach
 * through a permanent mapping from the memory it cannot, and vm_resident.c
 * uses it to set vm_page.highmem -- the flag that later forces a temporary
 * mapping for anything above the line.  The direct map removes the line: all
 * of physical memory is addressable through it, so the answer is TRUE for
 * every page and no page is ever marked high.
 *
 * This is one of the deletions #407 predicted rather than a shim standing in
 * for work not done.  It is written as a macro yielding TRUE, and not as a
 * comparison against some very large limit, because a limit would invite the
 * question of what value it should hold -- and there is no such value: the
 * concept does not apply to this target.
 */
#define	pa_is_lowmem(pa)	((void)(pa), TRUE)

#endif /* _MACH_X86_64_VM_PARAM_H_ */
