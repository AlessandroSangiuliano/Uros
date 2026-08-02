/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the machine-independent kernel asks about this machine as a whole
 * (#453): how much memory it has, what it was booted with, and how to stop
 * it.
 *
 * i386 keeps these in AT386/model_dep.c along with its console, its
 * interrupt tables and its boot-argument parser.  They are separated here
 * because they have nothing to do with each other beyond being the answers a
 * machine gives about itself.
 */

#include <stdint.h>

#include <mach/machine/vm_param.h>
#include <kern/misc_protos.h>

#include <boot/multiboot2.h>
#include <cpu/ipi.h>
#include <cpu/smp.h>
#include <kern/boot_modules.h>

/*
 * How much physical memory this machine has, in bytes.
 *
 * ⚠️ Usable memory and not the top of it.  mb2_top_of_ram() answers where
 * memory ends, which counts every hole and every region the firmware kept --
 * ACPI tables, the framebuffer, whatever a UEFI machine reserved.  The
 * machine-independent VM subtracts this from what it manages to work out how
 * much it has wired (vm_resident.c: `vm_page_wire_count = atop(mem_size) -
 * vm_page_free_count'), so counting memory that was never ours would make
 * the kernel believe it had wired pages it does not have -- an unsigned
 * subtraction that goes the wrong way, and a wired count near four billion.
 *
 * Set once, at boot, from what the loader reported.  i386 computes the same
 * quantity by subtracting its BIOS hole from the last address.
 */
vm_size_t	mem_size;

void
machine_mem_size_init(void)
{
	mem_size = (vm_size_t) mb2_usable_ram(mb2_info());
}

/*
 * The string the machine was booted with, copied into the caller's buffer.
 *
 * Copied and not returned by pointer because the caller owns the buffer and
 * the interface says so; the command line itself lives in the loader's
 * structure, which nothing here promises to keep.
 */
char *
machine_boot_info(char *buf, vm_size_t buf_len)
{
	const char	*cmdline = machine_boot_cmdline();
	vm_size_t	i;

	if (buf_len == 0)
		return buf;

	for (i = 0; i + 1 < buf_len && cmdline[i] != '\0'; i++)
		buf[i] = cmdline[i];

	buf[i] = '\0';
	return buf;
}

/*
 * Stop this processor, and do not come back.
 *
 * `cli' before `hlt', and the loop around them, are all three necessary and
 * for different reasons: without cli an interrupt wakes the halt and
 * execution continues past it; without the loop a non-maskable interrupt --
 * which cli does not stop -- does the same; and hlt rather than a spin so a
 * halted processor stops drawing power and stops competing for the memory
 * bus with whichever one is still doing something.
 */
void
halt_cpu(void)
{
	for (;;)
		__asm__ volatile("cli; hlt");
}

/* What the other processors are asked to run.  See halt_all_cpus below. */
static void
halt_this_cpu(void *unused)
{
	(void) unused;
	halt_cpu();
}

/*
 * Stop the machine, or restart it.
 *
 * ⚠️ The reboot half is not implemented and says so rather than halting
 * quietly.  A caller that asked for a reboot and got a halt is left looking
 * at a machine that stopped, with no way to tell whether the reboot failed
 * or the halt was what it asked for -- and on a headless machine that is the
 * difference between a reboot loop and a service that never comes back.
 *
 * When it arrives it is a triple fault or the ACPI reset register, and which
 * one is a decision: the reset register is the correct mechanism and is not
 * present on every machine, so the real answer is to try it and fall back.
 */
void
halt_all_cpus(boolean_t reboot)
{
	if (reboot)
		panic("halt_all_cpus: reboot is not implemented on this "
		      "machine yet (#453)");

	/*
	 * The others first, then this one: stopping this processor first would
	 * leave the rest running with nobody to stop them.
	 *
	 * Over the general call-others IPI rather than a halt-specific one:
	 * the message this machine has is "run this function", and halting is
	 * a function.  A dedicated halt IPI would be a second path through the
	 * same hardware for no gain, and one used once at shutdown would be
	 * the least exercised code in the kernel.
	 */
	ipi_call_others(halt_this_cpu, (void *) 0);
	halt_cpu();
}

/*
 * Enter the debugger with a message.
 *
 * ⚠️ Not implemented, and a panic rather than a return.  Debugger() is called
 * from places that have decided they cannot continue and expect a human to
 * take over; returning would carry on into code written on the assumption
 * that somebody had looked.  #428 is where this becomes an entry into DDB.
 */
void
Debugger(const char *message)
{
	panic("Debugger(\"%s\"): no debugger on this machine yet (#428)",
	      message ? message : "");
}
