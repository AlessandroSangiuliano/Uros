/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Making another processor do something (#438).
 *
 * A message between processors carries a vector and nothing else — eight
 * bits, no payload, no queue.  Everything above that is an arrangement in
 * memory that the sender writes and the receiver reads, with the interrupt
 * serving only as the prod that makes it look.
 *
 * One mechanism rather than several.  The TLB shootdown that #407 is
 * waiting on is the first thing that needs a processor to do work on
 * command, and it will not be the last: a scheduler wanting a processor to
 * reconsider what it is running, a debugger wanting them all to stop.
 * Those differ in what they ask for, not in how the asking works, and a
 * cross-call is the shape all of them have.
 *
 * ── What the caller must guarantee ────────────────────────────────────
 *
 * Interrupts enabled.  A processor waiting for answers has to be able to
 * give one: while it waits, another processor may be waiting for the lock
 * below, and both of them need to be able to take the message that releases
 * the other.  A cross-call issued with interrupts off is a deadlock that
 * happens only when two processors ask at once, which is to say rarely and
 * on somebody else's machine — so it is checked, not documented.
 */

#ifndef _X86_64_CPU_IPI_H_
#define _X86_64_CPU_IPI_H_

#include <stdint.h>

/*
 * The vector cross-calls arrive at.  High in the space on purpose: the
 * controller arbitrates by vector number, and a processor that another one
 * is standing still waiting for should outrank a device that nobody is.
 */
#define IPI_VECTOR_CALL		0xF1

/* Claim the vector.  Boot processor, once, before anybody is woken. */
void ipi_init(void);

/*
 * Run fn(arg) on every other online processor, and return once all of them
 * have finished doing it.
 *
 * Finished, not started: the shootdown's whole meaning is that when this
 * returns, no other processor is still using the translation that has been
 * taken away.  A call that returned at delivery would be no guarantee at
 * all.
 *
 * `fn` runs in interrupt context on the target, so it may not block, may
 * not take a lock the interrupted code could be holding, and should be
 * short — every processor in the machine is standing still until the last
 * one is done.
 */
void ipi_call_others(void (*fn)(void *), void *arg);

/*
 * How many cross-calls this processor has served.  For the boot-time proof
 * that the messages arrive at all, and afterwards for anyone wondering
 * whether a processor has stopped listening.
 */
uint64_t ipi_calls_served(uint32_t apic_id);

#endif	/* _X86_64_CPU_IPI_H_ */
