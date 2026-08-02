/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <kern/boot_modules.h> for x86-64 (#453).
 *
 * The answers come out of the multiboot 2 tag chain.  Multiboot 2 has no
 * module array and no count -- each module is a tag of its own -- so the
 * walking lives in boot/multiboot2.c and this file is the adapter between
 * that shape and the one the machine-independent side asks for.
 */

#include <kern/boot_modules.h>
#include <trap/trap.h>	/* panic() */
#include <boot/multiboot2.h>
#include <pmap/layout.h>

const char *
machine_boot_cmdline(void)
{
	return mb2_cmdline();
}

unsigned int
machine_boot_module_count(void)
{
	return mb2_module_count();
}

boolean_t
machine_boot_module(unsigned int n, vm_offset_t *phys_start, vm_size_t *size)
{
	uint64_t	start, len;

	if (!mb2_module_range(n, &start, &len))
		return FALSE;

	*phys_start = (vm_offset_t) start;
	*size	    = (vm_size_t) len;
	return TRUE;
}

/*
 * What this machine can check, which is not what i386 checks.
 *
 * i386's belt is about its boot page tables: start.S places them above the
 * highest module, so a module reaching past their base means early boot has
 * already written over that module's tail (#241/#359).  Nothing here has
 * that arrangement -- boot.S builds its tables inside the image, before any
 * module is looked at -- so that particular collision cannot happen and
 * asserting it would be checking a condition this machine does not create.
 *
 * What can happen, and what the same class of bug looks like here, is a
 * module the loader placed overlapping the kernel image itself.  GRUB is
 * told where we load and normally puts modules clear of it; "normally" is
 * exactly the word that makes this worth checking, because when it does not,
 * the module reads back as whatever the kernel's .data now holds and the
 * failure surfaces as an unloadable ELF a long way from here.
 *
 * ⚠️ Physical against physical.  The image's linker symbols are virtual --
 * it is linked in the top 2 GiB -- so they are brought down through the same
 * subtraction the boot mapping uses.  Comparing a physical module address
 * against a virtual kernel address would be true for every module and would
 * therefore never fire, which is the worst kind of check.
 */
void
machine_boot_modules_verify(void)
{
	extern char	__image_phys_start[], __kernel_end[];

	/*
	 * __image_phys_start is already physical -- the boot half is linked
	 * at VMA == LMA -- while __kernel_end is virtual, because the kernel
	 * half is linked in the top 2 GiB and loaded low through AT().  So
	 * exactly one of the two is converted, and which one is not a detail
	 * to be inferred from the names.
	 */
	uint64_t	kstart = (uint64_t) __image_phys_start;
	uint64_t	kend   = (uint64_t) __kernel_end - KERNEL_IMAGE_BASE;
	unsigned int	n, count = mb2_module_count();

	for (n = 0; n < count; n++) {
		uint64_t start, len;

		if (!mb2_module_range(n, &start, &len))
			continue;

		if (start < kend && start + len > kstart)
			panic("multiboot2 module %u at phys 0x%llx..0x%llx "
			      "overlaps the kernel image at 0x%llx..0x%llx "
			      "-- its contents are already gone (#453)",
			      n, (unsigned long long) start,
			      (unsigned long long) (start + len),
			      (unsigned long long) kstart,
			      (unsigned long long) kend);
	}
}

/*
 * The register state the bootstrap thread starts in.
 *
 * x86_64_THREAD_STATE, and not a syscall-entry flavor, because this machine
 * has none: the flavors here are the general registers, the floating-point
 * state and the exception state (#408).  A thread that has never run has
 * only the first, and two of its registers matter -- where it begins and
 * what it begins with as a stack.
 *
 * Everything else is left zero, which is a decision and not an omission: a
 * bootstrap task inherits nothing, so any non-zero register would be a value
 * this code invented and the task would have no way to know it had.
 */
static struct x86_64_thread_state	bootstrap_state;

void
machine_bootstrap_thread_state(vm_offset_t entry, int *flavor,
			       thread_state_t *state, unsigned int *count)
{
	__builtin_memset(&bootstrap_state, 0, sizeof bootstrap_state);

	bootstrap_state.rip = entry;

	*flavor = x86_64_THREAD_STATE;
	*state  = (thread_state_t) &bootstrap_state;
	*count  = x86_64_THREAD_STATE_COUNT;
}

void
machine_bootstrap_thread_sp(vm_offset_t sp)
{
	bootstrap_state.rsp = sp;
}
