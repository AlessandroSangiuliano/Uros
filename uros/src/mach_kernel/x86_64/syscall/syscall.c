/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The syscall path (#411, MD contract 6/6).
 */

#include <stdint.h>

#include <cpu/desc.h>
#include <cpu/regs.h>
#include <syscall/syscall.h>

#define MSR_STAR	0xC0000081
#define MSR_LSTAR	0xC0000082
#define MSR_FMASK	0xC0000084

/*
 * The flags SYSCALL clears on the way in.
 *
 * This register is worth more than it looks: every bit named here is an
 * instruction the entry path does not have to execute, on every call in the
 * system, and two of them are correctness rather than speed.
 *
 *   IF   no CLI — the kernel is entered with interrupts already off
 *   DF   no CLD — and the C ABI *requires* the direction flag clear on
 *        entry to a function, so a kernel that did not clear it would be
 *        one string instruction away from a user program deciding which
 *        way its memcpy runs
 *   AC   user code can set this to switch SMAP off for itself; clearing it
 *        here is what stops a syscall from carrying that permission into
 *        the kernel
 *   TF   otherwise the first kernel instruction single-steps into a debug
 *        exception chosen by the caller
 *   NT   stale nested-task state has no business surviving into ring 0
 */
#define RFLAGS_TF	(1UL << 8)
#define RFLAGS_DF	(1UL << 10)
#define RFLAGS_NT	(1UL << 14)
#define RFLAGS_AC	(1UL << 18)

#define SYSCALL_FMASK	(RFLAGS_IF | RFLAGS_DF | RFLAGS_AC | RFLAGS_TF | RFLAGS_NT)

extern void syscall_entry(void);

uint64_t syscall_probe(uint64_t a1, uint64_t a2, uint64_t a3,
		       uint64_t a4, uint64_t a5, uint64_t a6)
{
	return  (a1 & 0xFF)
	     | ((a2 & 0xFF) << 8)
	     | ((a3 & 0xFF) << 16)
	     | ((a4 & 0xFF) << 24)
	     | ((a5 & 0xFF) << 32)
	     | ((a6 & 0xFF) << 40);
}

/*
 * What each call number does.  Read from ring 3's index, so its length is
 * the only thing standing between a user program and an arbitrary call
 * through kernel memory — which is why the check against it is in the entry
 * path and not here.
 */
void *const syscall_table[SYSCALL_NR_MAX] = {
	[SYSCALL_NR_PROBE] = (void *)syscall_probe,
};

void syscall_init(void)
{
	/*
	 * STAR carries two selector bases and no selectors: the processor
	 * derives four from them by addition, which is why <cpu/desc.h> lays
	 * the table out the way it does rather than the way it reads.
	 */
	wrmsr(MSR_STAR, ((uint64_t)SYSRET_SELECTOR_BASE << 48)
			| ((uint64_t)KERNEL_CS_SELECTOR << 32));

	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
	wrmsr(MSR_FMASK, SYSCALL_FMASK);

	/*
	 * Last, and it is the switch: without this bit SYSCALL is not a slow
	 * instruction, it is not an instruction at all, and a program using
	 * one takes an invalid-opcode fault.  Enabled only once everything it
	 * would jump to is in place.
	 */
	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);
}
