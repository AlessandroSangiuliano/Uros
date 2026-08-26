/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Per-CPU data, reached through %gs (#409/#408).
 */

#include <stdint.h>

#include <cpu/desc.h>
#include <cpu/percpu.h>
#include <cpu/regs.h>
#include <kern/syscall_profile.h>	/* #411: the entry stub's clock reading */
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <trap/trap.h>

static uint64_t percpu_va(uint32_t cpu_id)
{
	return PERCPU_BASE + (uint64_t)cpu_id * PAGE_SIZE_4K;
}

void percpu_alloc(uint32_t cpu_id)
{
	uint64_t va = percpu_va(cpu_id);
	uint64_t frame = boot_frame_alloc();

	/*
	 * Returning here would leave %gs based at zero — which the boot
	 * identity map still makes readable, so %gs:0 would quietly hand back
	 * whatever is in low memory instead of faulting.  That happened once
	 * already; it is not a thing to leave available.
	 */
	if (frame == 0)
		panic("percpu: no frame for this CPU's block");

	/*
	 * A page per CPU, at a fixed stride from the region's base, so a
	 * block's address is a calculation rather than a lookup — which
	 * matters at the points where the lookup would need the block it is
	 * trying to find.
	 */
	if (pmap_enter(pmap_kernel(), va, frame,
		       VM_PROT_READ | VM_PROT_WRITE, 0) != PMAP_MAP_OK)
		panic("percpu: could not map a CPU's block");
}

/*
 * Ring 3 does not get to write its own segment bases (#440), and that is a
 * decision made here rather than a setting inherited from the firmware.
 *
 * With CR4.FSGSBASE set, WRGSBASE is an ordinary instruction available to a
 * user program, which would be a real improvement for thread-local storage:
 * a thread could move its own base without a syscall.  It also means the
 * value in IA32_GS_BASE stops being something only the kernel ever wrote.
 *
 * That matters because the entry path for the four vectors that can arrive
 * inside the swapgs window decides what to do by asking whether the loaded
 * base is a kernel address.  The test is sound only while a user base cannot
 * be one — and with FSGSBASE enabled a user program could simply write one,
 * turning the check into a question the attacker answers.
 *
 * Closing that needs the entry to find the per-CPU block *without* %gs: the
 * processor number from RDPID or from the descriptor limit, and the block's
 * address computed from it.  That machinery is worth having on the day the
 * TLS performance is wanted, and it is a different piece of work from this
 * one.  So the bit is cleared, explicitly, on every processor — and cleared
 * rather than merely not set, because "we never enabled it" is not a
 * statement about what CR4 contains when the kernel is handed the machine.
 *
 * ⚠️ Whoever sets this bit owns trap_paranoid() as well.  The two are one
 * decision wearing two hats.
 */
static void deny_user_segment_bases(void)
{
	uint64_t cr4 = read_cr4();

	if (cr4 & CR4_FSGSBASE)
		write_cr4(cr4 & ~CR4_FSGSBASE);
}

void percpu_activate(uint32_t cpu_id)
{
	uint64_t va = percpu_va(cpu_id);
	struct percpu *p = (struct percpu *)(uintptr_t)va;

	/*
	 * The page is already mapped — by the boot processor, before this one
	 * was woken — so nothing here touches shared page tables.
	 */
	p->self = p;
	p->cpu_id = cpu_id;

	/*
	 * The stack a syscall will switch to — the same one a trap from ring
	 * 3 lands on, because the two cannot be in flight at once.  Recorded
	 * here rather than looked up on entry: the entry path has nowhere to
	 * put a lookup, since it has not got a stack yet.
	 */
	/*
	 * ⚠️ Below the reserved frame, not at the stack top (#474).  The TSS
	 * keeps the top -- the processor pushes downward from it -- but the
	 * syscall entry starts USING this value, and it now also writes the
	 * frame's five return words at fixed offsets above it, so there must be
	 * a whole trap frame there.
	 */
	p->kernel_rsp = KERNEL_STACK_USER_FRAME(desc_rsp0(cpu_id));

	deny_user_segment_bases();

	wrmsr(MSR_GS_BASE, va);

	/*
	 * The other half of the pair.  swapgs exchanges the two, so what sits
	 * here is what %gs will hold after the swap — the kernel's block while
	 * user code runs, so that a single instruction on kernel entry brings
	 * it back.  Both halves hold the same value now: with no ring 3 there
	 * is nothing to swap away from, and an entry that swapped would only
	 * find the same block.
	 */
	wrmsr(MSR_KERNEL_GS_BASE, va);
}

#if	SYSCALL_PROFILE
/*
 * What the syscall entry stub left here on its way in (#411, for #392).
 *
 * The one machine-dependent line of the profile: only the stub knows when the
 * trap began, and only this file knows where it put it.  Everything else about
 * the sample is machine-independent and lives in <kern/syscall_profile.h>.
 */
uint64_t
syscall_profile_entry_tsc(void)
{
	return percpu()->syscall_tsc;
}
#endif	/* SYSCALL_PROFILE */

#if	SYSCALL_PROFILE
/*
 * What the return path has cost this processor so far (#411, for #392).
 *
 * The sum and the count rather than a mean, so the caller decides how to
 * report them -- and so that a caller which wants to reset them can.
 */
void
syscall_profile_return_cycles(uint64_t *cycles, uint64_t *count)
{
	*cycles = percpu()->syscall_ret_cycles;
	*count  = percpu()->syscall_ret_count;
}
#endif	/* SYSCALL_PROFILE */
