/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The I/O APIC: where device interrupts come in (#409).
 *
 * ── Why not the 8259 ──────────────────────────────────────────────────
 *
 * The legacy controller is one device with one mask for the whole machine,
 * and interrupt priority is per processor. So a processor sitting at a high
 * priority masks device interrupts for everybody — which is not a theory: it
 * is what starved the disk on i386 and hung the boot in #304. The I/O APIC
 * delivers to a *vector*, and each processor masks vectors for itself
 * through its own local APIC, so the coupling does not exist.
 *
 * The 8259 is already remapped and masked before any of this runs
 * (<cpu/pic.h>), which makes it a control rather than a competitor: with it
 * silent, an interrupt arriving on a vector this file programmed cannot have
 * come from anywhere else.
 *
 * ── The registers are behind a window ─────────────────────────────────
 *
 * Only two addresses are mapped. One selects a register by number and the
 * other reads or writes whichever is selected, so every access is two stores
 * or a store and a load, and none of them is atomic with respect to another
 * processor doing the same thing. That is a lock the day two processors
 * program pins at once; today only the boot processor does, before the
 * others are started, and saying so is what makes the absence of a lock a
 * decision.
 *
 * ── ⚠️ The trap that #381 already paid for ────────────────────────────
 *
 * The I/O APIC does not latch an edge that arrives while the pin is masked.
 * The 8259 did — it held the front in its interrupt request register and
 * delivered it on unmask — so code that masks a line per event, which was
 * safe there, loses interrupts here. A device that goes on asserting
 * afterwards (a 16550 with a full receive FIFO) then never fires again.
 *
 * Level-triggered lines are safe to mask that way, because the line is still
 * asserted when it is unmasked and the interrupt simply happens again. So
 * the trigger mode is not decoration and has to come from the firmware,
 * which is what the overrides in <cpu/acpi.h> carry.
 */

#ifndef _X86_64_CPU_IOAPIC_H_
#define _X86_64_CPU_IOAPIC_H_

#include <stdint.h>

/*
 * Where the legacy lines land in the vector space: pin n on vector 0x40+n.
 *
 * Above the architectural exceptions and well below the vectors the kernel
 * keeps for itself, and the same base i386 chose (#311) — not from habit,
 * but because the number is arbitrary and two architectures picking the same
 * arbitrary number is one fewer thing to remember.
 */
#define IOAPIC_ISA_VECTOR_BASE	0x40

/*
 * Map the first I/O APIC the firmware described and put every one of its
 * pins in a known state — masked.
 *
 * Masked rather than left alone, because the firmware does not hand over a
 * blank controller: it has been routing interrupts for its own purposes up
 * to this moment, and a pin it left enabled delivers to a vector this kernel
 * never chose. That arrives as an unclaimed interrupt at best.
 *
 * Returns zero if the firmware described no controller, in which case
 * nothing else here may be called.
 */
int ioapic_init(void);

/* Whether a controller was found and mapped. */
int ioapic_present(void);

/* Its identity and how many pins it has, as the controller itself reports. */
uint32_t ioapic_id(void);
uint32_t ioapic_version(void);
unsigned ioapic_pin_count(void);

/*
 * Point one pin at one vector on one processor, and unmask it.
 *
 * `flags` is the MADT encoding — see ACPI_TRIGGER_* and ACPI_POLARITY_* in
 * <cpu/acpi.h> — because that is where the answer comes from and translating
 * it at the call site would mean every caller translating it the same way.
 * Zero in either field means the ISA default, which is edge triggered and
 * active high.
 *
 * Physical destination: the interrupt goes to exactly one processor, named
 * by its APIC id. Broadcast and lowest-priority delivery both exist and
 * neither is wanted yet — one because an interrupt handled twice is worse
 * than one handled slowly, the other because it hands the choice to the
 * hardware before there is a scheduler with an opinion.
 */
void ioapic_route(uint32_t gsi, uint8_t vector, uint32_t apic_id,
		  uint16_t flags);

/*
 * Mask and unmask one pin.
 *
 * ⚠️ Read the note above before masking an edge-triggered line per event.
 * The controller does not hold the front while the pin is masked, and the
 * interrupt is gone rather than deferred.
 */
void ioapic_mask(uint32_t gsi);
void ioapic_unmask(uint32_t gsi);

/* Whether a pin is currently masked, so the state can be checked and not
 * merely assumed to be what it was last set to. */
int ioapic_is_masked(uint32_t gsi);

#endif	/* _X86_64_CPU_IOAPIC_H_ */
