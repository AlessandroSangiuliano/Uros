/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The clock's pins are not a line's to take (#600).
 *
 * The HPET's LegacyReplacement sends timer 0 to ISA 0's pin and silences the
 * RTC's (#593), and the clock holds both pins (<cpu/ioapic.h>).
 * device_md_irq_register() used to refuse lines 0 and 8 by their numbers, and
 * a PC's MADT sends ISA 0 to GSI 2 and says nothing about ISA 2 -- so line 2,
 * registered by a task holding the master port, was routed over the tick and
 * stopped every clock in the machine.  This asks the question the way that task
 * would: every line a task may ask for whose pin is one of the clock's, through
 * the function the master port's RPC calls.
 *
 * 🔑 Asked when LegacyReplacement is ON, and not when the clock is found to own
 * the pins: a kernel that forgot to take them would otherwise be NOT ASKED, a
 * guard that holds by not asking.  Asked after the clock is chosen and before
 * any task exists, so the answer is the claim's alone.
 * ablations/600-line-by-number.patch puts the refusal back on the numbers, which
 * is how this is shown able to fail.
 */

#include <stdint.h>

#include <device/device_machdep.h>
#include <device/device_master.h>	/* IRQ_FORWARD_LINES: what a task may ask */
#include <kern/misc_protos.h>		/* printf, panic */

#include <cpu/acpi.h>
#include <cpu/clock_pins_test.h>
#include <cpu/ioapic.h>
#include <time/hpet.h>

static void
clock_pins_handler(int irq)
{
	(void) irq;
}

void
clock_pins_test(void)
{
	uint32_t	tick = acpi_irq_to_gsi(0), rtc = acpi_irq_to_gsi(8);
	unsigned	line, asked = 0, refused = 0;

	if (!ioapic_present() || !hpet_legacy_routed()) {
		printf("clock_pins: NOT ASKED -- the tick is not on the HPET's "
		       "LegacyReplacement (#600)\n");
		return;
	}

	if (ioapic_owner(tick) != IOAPIC_OWNER_CLOCK
	    || ioapic_owner(rtc) != IOAPIC_OWNER_CLOCK) {
		printf("clock_pins: LegacyReplacement is on, and the clock does "
		       "not hold GSI %u and %u -- owners %u and %u -- WRONG "
		       "(#600)\n", tick, rtc, ioapic_owner(tick),
		       ioapic_owner(rtc));
		return;
	}

	for (line = 0; line < IRQ_FORWARD_LINES; line++) {
		uint32_t gsi = acpi_irq_to_gsi((uint8_t)line);

		if (gsi != tick && gsi != rtc)
			continue;
		asked++;
		if (!device_md_irq_register(line, clock_pins_handler)) {
			refused++;
			continue;
		}

		/*
		 * Granted: the tick's pin, or the RTC's silenced one, now routes
		 * to a line's vector.  Either every clock in the machine has
		 * stopped or the line waits for interrupts that cannot come, and
		 * the boot would stop later with nothing saying why.
		 */
		panic("clock_pins: ISA line %u arrives on GSI %u, the clock's, and "
		      "was GRANTED -- WRONG (#600)", line, gsi);
	}

	printf("clock_pins: the clock holds GSI %u and %u; %u ISA lines arrive "
	       "on them, %u refused -- %s (#600)\n", tick, rtc, asked, refused,
	       asked > 0 && refused == asked ? "PASS" : "WRONG");
}
