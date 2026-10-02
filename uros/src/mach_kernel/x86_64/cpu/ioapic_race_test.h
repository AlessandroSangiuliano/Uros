/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The I/O APIC's window, raced from two processors (#599, -Y).
 */

#ifndef _X86_64_CPU_IOAPIC_RACE_TEST_H_
#define _X86_64_CPU_IOAPIC_RACE_TEST_H_

/*
 * Two processors change the vectors of two unused, masked pins by the
 * read-modify-write that masks and unmasks, and read them back.  Prints
 * "ioapic_race: PASS", "WRONG" or "NOT ASKED", and returns: the boot goes on.
 */
void ioapic_window_race_test(void);

#endif	/* _X86_64_CPU_IOAPIC_RACE_TEST_H_ */
