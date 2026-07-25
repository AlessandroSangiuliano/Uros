/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Local APIC (#438).
 *
 * Each processor has one, all of them behind the same physical address —
 * so a CPU reading the local APIC reads *its own*, and the same instruction
 * gives a different answer on each.  That property is what makes it usable
 * as identity, and it is also what makes it the only way one CPU can wake
 * another.
 */

#ifndef _X86_64_CPU_LAPIC_H_
#define _X86_64_CPU_LAPIC_H_

#include <stdint.h>

/*
 * Map the registers and take note of where they are.
 *
 * The base comes from the MSR rather than the MADT, deliberately: the MADT
 * says where the firmware put it, the MSR says where it is now, and they
 * differ on any machine where it has been relocated.  The MADT value is
 * still worth having as a cross-check, which is why the caller passes it.
 */
void lapic_init(uint64_t madt_base);

/*
 * The vector the local APIC delivers when it has decided, after the fact,
 * that it had nothing to deliver — a line that dropped between the decision
 * to interrupt and the interrupt itself.
 *
 * It is not optional and it is not configurable away: the register that
 * software-enables the APIC is the same register that names this vector, so
 * a kernel that wants interrupts at all has to nominate one.  0xFF because
 * it must not collide with anything real, and the top of the space is the
 * one place guaranteed not to be handed out.
 */
#define LAPIC_SPURIOUS_VECTOR	0xFF

/*
 * Turn this processor's local APIC on and let every priority through.
 *
 * Per processor, not once: the enable bit and the priority threshold are in
 * this CPU's own registers, and an application processor comes out of a
 * startup interrupt with its own copies in whatever state reset left them.
 *
 * Until this runs the APIC accepts nothing.  An interrupt sent to a
 * processor that has not done it is not delayed or refused — it is
 * discarded, with the sender's message register reporting it was delivered.
 */
void lapic_enable(void);

/*
 * Say the current interrupt has been dealt with.
 *
 * Not a formality: the APIC holds the vector's priority level busy until
 * this is written, and refuses anything of equal or lower priority in the
 * meantime.  A handler that returns without it stops the *next* one of its
 * kind from ever arriving, which looks like a sender that failed rather
 * than a receiver that never finished.
 */
void lapic_eoi(void);

/* Whether lapic_init() found a usable local APIC. */
int lapic_present(void);

/* This CPU's APIC id, read from its own local APIC. */
uint32_t lapic_id(void);

/* Whether this CPU is the one the firmware started — the MSR says so. */
int lapic_is_bsp(void);

/*
 * The wake-up sequence, INIT then two start-up interrupts.
 *
 * `entry_pa` is where the target begins executing, in real mode, and must
 * be page-aligned below 1 MiB: the message carries a vector, and the CPU
 * turns it into a physical address by shifting it left by twelve, so only
 * the page number fits.  It is the addressing limit of the mechanism, not a
 * convention.
 */
void lapic_send_init(uint32_t apic_id);
void lapic_send_startup(uint32_t apic_id, uint64_t entry_pa);

/*
 * Interrupt this same processor at `vector`.
 *
 * The shortest path through the delivery hardware there is — no
 * destination, no bus arbitration, no second processor that has to be
 * awake and configured — which is exactly what makes it the right first
 * test.  If a self-interrupt does not arrive, nothing about the receiving
 * side works, and there is no point looking at the sending side yet.
 */
void lapic_send_self(uint8_t vector);

/*
 * Interrupt one other processor, or every processor but this one.
 *
 * The broadcast is not a convenience wrapper around a loop: it is one write
 * to the command register instead of one per processor, and the command
 * register takes a single message at a time — so a loop over thirty-one
 * processors is thirty-one waits for the previous message to be accepted,
 * with the last of them delivered long after the first.
 *
 * Neither waits for the interrupt to be *handled*, only for the message to
 * be accepted for delivery.  What the target did about it is the caller's
 * question, and the reason the layer above this one counts answers.
 */
void lapic_send_ipi(uint32_t apic_id, uint8_t vector);
void lapic_broadcast_ipi(uint8_t vector);

#endif	/* _X86_64_CPU_LAPIC_H_ */
