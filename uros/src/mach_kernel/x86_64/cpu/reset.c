/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Restarting the machine (#373).
 *
 * The way ACPI gives (6.5, 4.8.3.6): the FADT names an 8-bit register and a
 * value, and the machine resets on the write.  Where the register is, is the
 * firmware's to say, and machines differ: OMEGA's is port 0xcf9, the
 * chipset's reset control, given 0x06; omen's is port 0xb2 given 0xeb, a
 * command to the firmware's SMI handler, which does the reset itself.  A
 * kernel that wrote 0xcf9 everywhere would not be doing on omen what its
 * firmware asks for (both read off Linux on 10/10/2026).
 *
 * 4.8.3.6 says the processor does not execute past the write.  If this one
 * does, the register did not do it, and the older ways follow -- 0xcf9 the way
 * Linux writes it, the keyboard controller's reset line, a triple fault --
 * each said before it is tried, so that what survives the reset (#373's zone)
 * says which one it was.
 */

#include <stdint.h>
#include <kern/misc_protos.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <kern/time_out.h>
#include <boot/bootarg.h>
#include <cpu/acpi.h>
#include <cpu/regs.h>
#include <cpu/reset.h>
#include <pmap/pmap.h>
#include <time/delay.h>
#include <time/tsc.h>		/* rdtsc_ordered, for the wait before the rulers */

static struct acpi_reset	rr;
static int			rr_usable;
static const char		*rr_why;	/* why not, when it is not */
static volatile uint8_t		*rr_mmio;	/* in memory: mapped at boot */

#define	PCI_CONFIG_ADDRESS	0x0CF8
#define	PCI_CONFIG_DATA		0x0CFC
#define	PCI_CONFIG_ENABLE	0x80000000u

/* Table 5.2's three fields of a configuration-space address. */
#define	RR_PCI_DEV(a)		((uint32_t)((a) >> 32) & 0xffff)
#define	RR_PCI_FN(a)		((uint32_t)((a) >> 16) & 0xffff)
#define	RR_PCI_OFF(a)		((uint32_t)(a) & 0xffff)

/*
 * How long a way to reset is given before the next is tried.  Far longer than
 * any reset takes to start: a machine that resets is gone in microseconds,
 * and one that is still here half a second later is not going to.
 */
#define	RESET_WAIT_US		500000

#define	CF9			0x0CF9
#define	CF9_SYS_RST		0x02	/* reset the system, with RST_CPU */
#define	CF9_RST_CPU		0x04	/* ... the edge that starts it */
#define	CF9_FULL_RST		0x08	/* and power the platform down first */

#define	KBC_STATUS		0x64
#define	KBC_INPUT_FULL		0x02
#define	KBC_PULSE_RESET		0xFE

void reset_init(void)
{
	acpi_reset_reg(&rr);
	rr_usable = 0;
	rr_why = 0;

	if (!rr.fadt_found)
		rr_why = "there is no FADT";
	else if (!rr.in_table)
		rr_why = "the FADT is older than revision 2, or too short to "
			 "hold one";
	else if (!rr.supported)
		rr_why = "RESET_REG_SUP is clear";
	else if (rr.reg.space_id == ACPI_GAS_IO) {
		if (rr.reg.address == 0 || rr.reg.address > 0xffff)
			rr_why = "its port is not a port";
		else
			rr_usable = 1;
	} else if (rr.reg.space_id == ACPI_GAS_MEMORY) {
		if (rr.reg.address == 0)
			rr_why = "its address is zero";
		else {
			uint64_t page = rr.reg.address & ~0xfffULL;

			rr_mmio = (volatile uint8_t *)(uintptr_t)
				  (pmap_map_device(page, 0x1000)
				   + (rr.reg.address - page));
			rr_usable = 1;
		}
	} else if (rr.reg.space_id == ACPI_GAS_PCI_CONFIG) {
		/*
		 * Through the port pair, which reaches the first 256 bytes of
		 * a function's space on bus 0 -- all a register on the
		 * configuration header needs, and no mapping and no lock to
		 * depend on at the moment of the reset.
		 */
		if ((rr.reg.address >> 48) != 0 || RR_PCI_DEV(rr.reg.address) > 31
		    || RR_PCI_FN(rr.reg.address) > 7
		    || RR_PCI_OFF(rr.reg.address) > 0xff)
			rr_why = "its configuration-space address is not one on "
				 "bus 0's header";
		else
			rr_usable = 1;
	} else
		rr_why = "it is in none of the three address spaces 4.8.3.6 "
			 "allows";

	if (!rr_usable) {
		printf("reset: no reset register to write -- %s (FADT revision "
		       "%u); a reset will try 0xcf9, the keyboard controller "
		       "and a triple fault (#373)\n", rr_why, rr.revision);
		return;
	}

	if (rr.reg.space_id == ACPI_GAS_IO)
		printf("reset: the FADT's reset register is I/O port 0x%llx, "
		       "value 0x%02x", (unsigned long long)rr.reg.address,
		       rr.value);
	else if (rr.reg.space_id == ACPI_GAS_MEMORY)
		printf("reset: the FADT's reset register is memory at 0x%llx, "
		       "value 0x%02x", (unsigned long long)rr.reg.address,
		       rr.value);
	else
		printf("reset: the FADT's reset register is PCI 00:%02x.%x "
		       "offset 0x%02x, value 0x%02x",
		       RR_PCI_DEV(rr.reg.address), RR_PCI_FN(rr.reg.address),
		       RR_PCI_OFF(rr.reg.address), rr.value);
	/*
	 * 4.8.3.6 says 8 bits wide at offset 0.  A table that says otherwise is
	 * still written as one byte at the address: what Windows does, and
	 * Linux after it, so it is what firmware is tested against.
	 */
	if (rr.reg.bit_width != 8 || rr.reg.bit_offset != 0)
		printf(" (its bit width is %u and offset %u, where 4.8.3.6 says "
		       "8 and 0: written as one byte all the same)",
		       rr.reg.bit_width, rr.reg.bit_offset);
	printf(" (FADT revision %u, ACPI 6.5 4.8.3.6) (#373)\n", rr.revision);
}

/*
 * A wait that does not depend on the boot having got far: a reset can come
 * from a panic before the rulers were found, where delay() itself panics.
 * Then the TSC, uncalibrated, as if it ran at 5 GHz: no x86-64 processor's
 * runs faster, so the wait is at least as long as asked, and on a slower one
 * longer -- the safe side for a wait whose only job is to be long enough.
 * Not a count of pauses: under a hypervisor a run of them is what makes the
 * processor exit to it, and how long that takes is not this kernel's to say.
 */
#define	RESET_TSC_PER_US_MAX	5000ULL

void reset_wait_us(unsigned us)
{
	uint64_t	t0;

	if (delay_tsc_us(us) || delay_hpet_us(us) || delay_pmtimer_us(us))
		return;
	t0 = rdtsc_ordered();
	while (rdtsc_ordered() - t0 < (uint64_t)us * RESET_TSC_PER_US_MAX)
		cpu_pause();
}

static void reset_write_register(void)
{
	if (rr.reg.space_id == ACPI_GAS_IO)
		outb((uint16_t)rr.reg.address, rr.value);
	else if (rr.reg.space_id == ACPI_GAS_MEMORY)
		*rr_mmio = rr.value;
	else {
		uint32_t off = RR_PCI_OFF(rr.reg.address);

		outl(PCI_CONFIG_ADDRESS, PCI_CONFIG_ENABLE
		     | (RR_PCI_DEV(rr.reg.address) << 11)
		     | (RR_PCI_FN(rr.reg.address) << 8) | (off & 0xfc));
		outb((uint16_t)(PCI_CONFIG_DATA + (off & 3)), rr.value);
	}
}

/*
 * -b's deadline.  -b restarts the machine after the quiet census, which comes
 * only once ring 3 has gone quiet everywhere: a whole system keeps servers
 * that poll, and boot_probe stays in ring 3 when it is done, so neither ever
 * has one.  A thread asleep for reset_after= seconds covers them, and a boot
 * that stopped somewhere a timer still runs: it takes the processor from any
 * user task, being a kernel thread, and restarts from thread context, the
 * best there is for it.  A boot whose timers have stopped, or with interrupts
 * off for good, is not one it can restart.
 */
#define	RESET_AFTER_DEFAULT	300	/* seconds */

static uint64_t	reset_after;
static int	reset_deadline_event;

static void reset_deadline_thread(void)
{
	assert_wait((event_t) &reset_deadline_event, FALSE);
	thread_set_timeout((int)(reset_after * hz));
	thread_block((void (*)(void)) 0);
	reset_timeout_check(&current_thread()->timer);

	printf("reset: -b: %llu seconds since the scheduler started and the "
	       "boot has not ended; restarting the machine (#373)\n",
	       (unsigned long long)reset_after);
	halt_all_cpus(TRUE);
}

void reset_deadline_start(void)
{
	uint64_t	s;
	int		r;

	if (!boot_flag('b'))
		return;
	r = boot_value("reset_after", &s);
	/* A tick count is an int: a deadline past it is no deadline. */
	if (r == 1 && s > 0 && s <= 0x7fffffffULL / (uint64_t)hz)
		reset_after = s;
	else {
		reset_after = RESET_AFTER_DEFAULT;
		if (r != 0)
			printf("reset: reset_after= is not a number of seconds "
			       "this kernel can wait; -b's deadline is %u "
			       "(#373)\n", RESET_AFTER_DEFAULT);
	}
	printf("reset: -b: the machine restarts after the quiet census, five "
	       "seconds after a panic, or in %llu seconds (#373)\n",
	       (unsigned long long)reset_after);
	(void) kernel_thread(kernel_task, reset_deadline_thread, (char *) 0);
}

void reset_machine(void)
{
	interrupts_disable();

	/*
	 * A reset does not write a write-back cache to memory, and what is read
	 * after one -- #373's zone -- is memory.  The zone itself is mapped
	 * uncached and needs none of this; anything else a reader of this boot
	 * finds afterwards does.
	 */
	wbinvd();

	if (rr_usable) {
		printf("reset: writing 0x%02x to the FADT's reset register\n",
		       rr.value);
		reset_write_register();
		reset_wait_us(RESET_WAIT_US);
		printf("reset: still running %u ms after the FADT's register "
		       "was written\n", RESET_WAIT_US / 1000);
	}

	/*
	 * Linux's order: the system reset bit first, then the processor reset
	 * that starts it.  FULL_RST cleared, and kept cleared: it powers the
	 * platform down first, and memory with it -- the one thing this reset
	 * is for keeping.
	 */
	printf("reset: port 0xcf9, 0x%02x then 0x%02x\n", CF9_SYS_RST,
	       CF9_SYS_RST | CF9_RST_CPU);
	{
		uint8_t v = inb(CF9) & ~(CF9_SYS_RST | CF9_RST_CPU
					 | CF9_FULL_RST);

		outb(CF9, v | CF9_SYS_RST);
		reset_wait_us(50);
		outb(CF9, v | CF9_SYS_RST | CF9_RST_CPU);
		reset_wait_us(RESET_WAIT_US);
	}

	printf("reset: still running -- the keyboard controller's reset "
	       "line\n");
	for (int tries = 0; tries < 10; tries++) {
		for (int i = 0; i < 0x10000
			 && (inb(KBC_STATUS) & KBC_INPUT_FULL); i++)
			cpu_pause();
		outb(KBC_STATUS, KBC_PULSE_RESET);
		reset_wait_us(50);
	}
	reset_wait_us(RESET_WAIT_US);

	/*
	 * An interrupt table of no entries: the breakpoint cannot be
	 * delivered, nor the fault that reports that, nor the double fault
	 * after it, and a processor that faults three times shuts down, which
	 * the platform turns into a reset.
	 */
	printf("reset: still running -- a triple fault\n");
	{
		struct {
			uint16_t	limit;
			uint64_t	base;
		} __attribute__((packed)) none = { 0, 0 };

		__asm__ volatile("lidt %0; int3" : : "m"(none) : "memory");
	}

	for (;;)
		__asm__ volatile("cli; hlt");
}
