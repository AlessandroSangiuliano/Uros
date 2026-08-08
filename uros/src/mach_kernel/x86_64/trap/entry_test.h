/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The frame each kind of entry builds, checked against what the interrupted
 * code actually had (#409, MD contract 4/6).
 *
 * ── What this is for, which is not "does the handler run" ─────────────
 *
 * By the time this was written every one of these entries already worked, in
 * the sense that a handler ran and the machine survived: the probes fault and
 * are stepped over, the tick is counted, the device interrupt arrives, the NMI
 * comes back.  None of that reads the frame.  `device_irq' in boot_c.c took
 * one and said `(void)frame;'.
 *
 * A frame that is wrong in a field nobody reads is not a fault that gets
 * reported wrongly — it is a fault that gets reported CONFIDENTLY and wrongly,
 * which is worse, because the report is the primary instrument for everything
 * else on this target.  #458 lost two hours to a general-protection dump whose
 * rsp could not be reconciled with its rip, and the way out was to stop
 * believing the instrument.
 *
 * So each entry here is provoked at a place that publishes its own %rsp and
 * its own address, and the frame is required to agree with both.  Where the
 * two disagree the test says so instead of the reader eventually noticing.
 */

#ifndef _X86_64_TRAP_ENTRY_TEST_H_
#define _X86_64_TRAP_ENTRY_TEST_H_

/*
 * Provoke a kernel fault, a timer interrupt, a device interrupt and an NMI at
 * a known instruction, and check the frame each one produced.
 *
 * Wants the local APIC timer free, the I/O APIC initialised and the 8254
 * available — so, after the tests that establish those and before the clock is
 * handed to the scheduler.
 */
void trap_entry_test(void);

#endif	/* _X86_64_TRAP_ENTRY_TEST_H_ */
