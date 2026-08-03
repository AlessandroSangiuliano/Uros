/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <kern/boot_modules.h> for i386 (#453).
 *
 * The answers all come out of `mb_info', which model_dep.c fills from
 * whatever the loader left -- parse_multiboot() reads boot_magic and handles
 * both multiboot 1 and the multiboot 2 a modern GRUB may hand an i386
 * kernel, so this file does not have to know which arrived.
 *
 * Nothing here is new behaviour.  It is the code that was inside
 * kern/bootstrap.c, moved to the side of the boundary that knows what a
 * multiboot module is.
 */

#include <kern/boot_modules.h>
#include <kern/misc_protos.h>	/* panic() */
#include <mach/machine/vm_param.h>
#include <i386/multiboot.h>

extern struct multiboot_info	mb_info;

const char *
machine_boot_cmdline(void)
{
	/*
	 * Exactly what the loader left, with no translation.
	 *
	 * ⚠️ Two other candidates look right and are not.  phystokv() is wrong
	 * because whether the pointer is physical depends on which protocol
	 * delivered it -- multiboot 2 leaves one into an already-mapped tag --
	 * and kern_args_start is wrong because the startup assembly writes it
	 * from a different source entirely.  Both were tried, and both changed
	 * a value that had been right (#453).
	 *
	 * ⚠️ Whether dereferencing this without translation is correct on every
	 * i386 boot path is a question this file does not answer: it is what
	 * kern/bootstrap.c has always done, and preserving it exactly is the
	 * job here.  Auditing it belongs with #415.
	 */
	if (mb_info.cmdline == 0)
		return "";

	return (const char *) mb_info.cmdline;
}

unsigned int
machine_boot_module_count(void)
{
	if (mb_info.mods_addr == 0)
		return 0;

	return mb_info.mods_count;
}

boolean_t
machine_boot_module(unsigned int n, vm_offset_t *phys_start, vm_size_t *size)
{
	struct multiboot_module	*mods;

	if (mb_info.mods_addr == 0 || n >= mb_info.mods_count)
		return FALSE;

	mods = (struct multiboot_module *) phystokv(mb_info.mods_addr);

	*phys_start = mods[n].mod_start;
	*size	    = mods[n].mod_end - mods[n].mod_start;
	return TRUE;
}

/*
 * The #359 belt.
 *
 * start.S places the boot page directory and tables above the highest module
 * end, walking the module list on both the mb1 and mb2 paths.  If a module
 * still reaches past the page-table base then early boot has ALREADY written
 * over that module's tail, and the symptom arrives much later as a zeroed or
 * truncated ELF -- "unloadable file format" (#241/#359).  Refusing to limp on
 * and saying exactly what happened is worth more than the boot.
 */
void
machine_boot_modules_verify(void)
{
	extern vm_offset_t	(kvtophys)(vm_offset_t addr);
	extern char		*kpde;	/* VA of the boot page directory */

	struct multiboot_module	*mods;
	vm_offset_t		ptbase;
	unsigned int		i;

	if (mb_info.mods_addr == 0)
		return;

	ptbase = kvtophys((vm_offset_t) kpde);
	mods   = (struct multiboot_module *) phystokv(mb_info.mods_addr);

	for (i = 0; i < mb_info.mods_count; i++)
		if (mods[i].mod_end > ptbase)
			panic("multiboot mod[%d] ends at phys 0x%x, past the "
			      "boot page tables at 0x%x -- module tail "
			      "clobbered (#359)",
			      i, mods[i].mod_end, (unsigned int) ptbase);
}

/*
 * The register state the bootstrap thread starts in.
 *
 * THREAD_SYSCALL_STATE because that is how i386 enters a thread that has
 * never run: the state a system call return would restore, with the entry
 * point where the return address goes.  Moved here from kern/bootstrap.c,
 * where it sat inside `#if defined(i386)' beside two other machines' arms.
 */
static struct thread_syscall_state	bootstrap_state;

void
machine_bootstrap_thread_state(vm_offset_t entry, int *flavor,
			       thread_state_t *state, unsigned int *count)
{
	bzero((char *) &bootstrap_state, sizeof bootstrap_state);

	bootstrap_state.eip = entry;

	*flavor = THREAD_SYSCALL_STATE;
	*state  = (thread_state_t) &bootstrap_state;
	*count  = i386_THREAD_SYSCALL_STATE_COUNT;
}

void
machine_bootstrap_thread_sp(vm_offset_t sp)
{
	bootstrap_state.esp = sp;
}
