/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The clock's pins are not a line's to take (#600).
 */

#ifndef _X86_64_CPU_CLOCK_PINS_TEST_H_
#define _X86_64_CPU_CLOCK_PINS_TEST_H_

/*
 * Once the clock backend is chosen (machine_init()), and with the tick on the
 * HPET's LegacyReplacement: asks device_md_irq_register() for every ISA line
 * that arrives on one of the clock's two pins, and expects each refused.  One
 * line of verdict, or "NOT ASKED" when LegacyReplacement is off.  A line
 * GRANTED has been routed over the tick or the RTC's silenced pin, so the boot
 * panics there, naming it, rather than stopping later without a word.
 */
void clock_pins_test(void);

#endif	/* _X86_64_CPU_CLOCK_PINS_TEST_H_ */
