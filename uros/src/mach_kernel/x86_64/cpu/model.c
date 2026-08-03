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
#include <ddb/cons.h>
#include <cpu/smp.h>
#include <trap/trap.h>		/* x86_64_backtrace on the panic path */
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
 *
 * ⚠️ On the way past, the backtrace -- but only when the machine is dying.
 *
 * kern/debug.c's panic() ends here: with no debugger on this machine (#428)
 * it prints its message and calls halt_cpu(), so this is the last machine-
 * dependent code to run and the last moment the stack still means something.
 * A symbolised call stack is the single most useful thing to have on a
 * machine that is being brought up, and this is where it belongs -- not in a
 * second panic() competing with the real one (#453).
 *
 * panicstr is what distinguishes the two ways in.  An orderly halt -- an
 * operator asking for one, the last processor in halt_all_cpus -- has nothing
 * to report and says nothing.
 */
void
halt_cpu(void)
{
	if (panicstr != (const char *) 0)
		x86_64_backtrace((uint64_t)(uintptr_t)
				 __builtin_frame_address(0));

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

/*
 * A character to the console (#453).
 *
 * The machine-independent printf() reaches the screen through this one name,
 * so it is the whole of what kern/printf.c needs from a machine: everything
 * above it -- format parsing, the log buffer, the %-conversions -- is shared,
 * and everything below is x86_64/ddb/cons.c, which already knows about the
 * serial port and the framebuffer.
 *
 * ⚠️ A thin forwarder and not a rename.  cons_putc() is this target's own
 * interface and is called directly by early boot, before there is a kernel
 * to have a printf() in; keeping the two names distinct is what lets the
 * early path go on working when the machine-independent one is torn out or
 * replaced.
 */
void
cnputc(char c)
{
	cons_putc(c);
}

/*
 * A character from the console, or -1 if there is none waiting.
 *
 * kern/printf.c uses it for the "more" prompt -- the pause a long listing
 * offers so an operator can read a screen before the next one arrives.
 * Nothing else in the kernel reads the console.
 */
int
cngetc(void)
{
	return cons_getc();
}

/*
 * Which processor keeps time (#453).
 *
 * The machine-independent tree reads it to decide who owns the clock and who
 * answers the "master" questions.  Zero because that is the processor the
 * firmware started, and because x86_64/cpu/smp.c numbers the others from its
 * APIC id upwards -- so the boot processor is index zero by construction
 * rather than by convention.
 *
 * ⚠️ It is a variable and not a constant because the interface says so: a
 * machine may in principle hand the clock to another processor.  This one
 * does not, and if it ever does -- for a core that is not parked, say -- the
 * assignment lives here rather than in whoever wanted it moved.
 */
int	master_cpu = 0;

/*
 * Whether the debugger is running.
 *
 * Read by kern/printf.c, which routes its output differently while a debugger
 * has the console, and by anything that must not block when the machine is
 * stopped.  Always false until #428 makes DDB reachable here; it is defined
 * rather than assumed absent because every reader tests it and a missing
 * symbol would not link.
 */
int	db_active = 0;
