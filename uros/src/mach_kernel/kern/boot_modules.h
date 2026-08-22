/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the kernel needs to know from whoever loaded it (#453).
 *
 * kern/bootstrap.c used to read `struct multiboot_info' directly, which
 * meant a file in kern/ knew which boot protocol it had been loaded by.  It
 * had to: there was one machine and one loader.  There are two now, and they
 * do not share a structure -- i386 is booted by multiboot 1 and x86-64 by
 * multiboot 2, whose module list is a tag in a chain rather than an array at
 * a known offset.
 *
 * So this is the whole of what bootstrap needs, and it is small: the command
 * line, and where each module was put.  Everything else it was reading --
 * flags, mem_lower, mem_upper -- went to a debug print.
 *
 * ⚠️ Module addresses are PHYSICAL, as every loader reports them.  The
 * caller converts, because the conversion is the caller's business: on i386
 * it is phystokv(), and the reason bootstrap.c does it at all is that
 * exec_load() and exec_map() dereference the result.  Returning a virtual
 * address here would mean each machine guessing which mapping the caller
 * wanted.
 */

#ifndef	_KERN_BOOT_MODULES_H_
#define	_KERN_BOOT_MODULES_H_

#include <mach/boolean.h>
#include <mach/machine/vm_types.h>
#include <mach/machine/thread_status.h>
#include <mach/thread_status.h>

/*
 * The string the loader was given, or "" if it gave none.  Never NULL: the
 * callers pass it to strlen() and to the boot script, and a machine with no
 * command line has an empty one rather than an absent one.
 */
extern const char *machine_boot_cmdline(void);

/* How many modules the loader placed.  Zero is a fatal condition for the
   caller, not for us: a kernel with no bootstrap module cannot start a task,
   but that judgement belongs to bootstrap.c. */
extern unsigned int machine_boot_module_count(void);

/*
 * Where module N is, in physical addresses.  Answers FALSE if there is no
 * such module, in which case neither output is written.
 */
extern boolean_t machine_boot_module(unsigned int n,
				     vm_offset_t *phys_start,
				     vm_size_t *size);

/*
 * The string the loader was given with module N (#488).
 *
 * Every boot protocol carries one -- GRUB writes everything after the path in
 * `module2 /boot/foo the rest of this line' -- and until #488 the kernel read
 * it from neither, naming the first task with a constant instead.  That made
 * a task's own panic say the name of a program that was not running.
 *
 * ⚠️ Unlike machine_boot_module() above, this returns a pointer the caller may
 * dereference: the conversion from whatever the loader reported is the
 * machine's business here, because the two protocols do not agree on whether
 * there is one to do.  Never NULL -- "" for a module with no string -- because
 * the callers pass it to strlen().
 */
extern const char *machine_boot_module_string(unsigned int n);

/*
 * Check that the modules survived early boot, and panic naming what did not.
 *
 * Each machine checks something different because each one puts different
 * things next to the modules, and the failure this guards against is the
 * #241/#359 class: an early-boot structure written over a module's tail, so
 * that the module loads as a zeroed or truncated ELF and the error surfaces
 * as "unloadable file format" a long way from the cause.
 */
extern void machine_boot_modules_verify(void);

/*
 * ── The state the first thread starts in ─────────────────────────────
 *
 * Also a question for the machine, and for the same reason: kern/bootstrap.c
 * used to carry a `struct thread_syscall_state' and a ladder of #ifs filling
 * in eip/esp, or pc/sp, or iioq_head/dp -- one arm per machine, in a file
 * that should not know any machine's register names.
 *
 * The flavors themselves differ too, not just the fields: i386 starts its
 * bootstrap thread on THREAD_SYSCALL_STATE, a flavor x86-64 does not have,
 * because there is no separate syscall-entry state here -- a thread that has
 * never run has only its general registers.  So the machine answers with the
 * flavor as well as the state.
 *
 * The buffer is the machine's own: it lives as long as the kernel and
 * bootstrap only reads it.
 */
extern void machine_bootstrap_thread_state(vm_offset_t entry,
					   int *flavor,
					   thread_state_t *state,
					   unsigned int *count);

/*
 * And the stack pointer, separately, because it is known separately: the
 * entry point comes out of the ELF header, while the stack pointer is only
 * settled once the argument block has been built at the top of the stack and
 * its address is known.  One call taking both would force the caller to
 * delay the first until it had the second.
 */
extern void machine_bootstrap_thread_sp(vm_offset_t sp);

#endif	/* _KERN_BOOT_MODULES_H_ */
