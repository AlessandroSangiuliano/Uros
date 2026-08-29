/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	device_machdep.c — x86-64's answers to <device/device_machdep.h> (#457).
 *
 *	Six of the eight are thin, and deliberately: config space, the port
 *	accesses and the masking already existed and were reachable under a
 *	different name, so what was missing there was not machinery but the
 *	place where the device master could ask without knowing which machine
 *	it was on.
 *
 *	The last two are not thin.  Claiming a line here means choosing a
 *	vector, claiming it in the dispatch table and pointing an I/O APIC pin
 *	at it -- three things i386 does by writing a handler into an array the
 *	dispatch reads -- and it is where the one behaviour this machine has
 *	that i386 does not turns out to matter: a device interrupt can be
 *	deferred by the priority level, and a deferred interrupt has already
 *	been acknowledged.  See the trampoline.
 */

#include <device/device_machdep.h>

#include <mach/boolean.h>	/* <ddb/ddb.h> declares in boolean_t */

#include <cpu/acpi.h>
#include <cpu/ioapic.h>
#include <cpu/lapic.h>	/* lapic_eoi, and which processor to route to */
#include <cpu/pci_cfg.h>
#include <cpu/regs.h>	/* inb/outl and the other widths */
#include <ddb/ddb.h>	/* whether a debugger was asked for */
#include <trap/trap.h>	/* the vector table, and the replay path */

extern void	Debugger(const char *message);

unsigned int
device_md_pci_read(unsigned int bus, unsigned int slot, unsigned int func,
		   unsigned int reg)
{
	return pci_cfg_read(0, (uint8_t)bus, (uint8_t)slot, (uint8_t)func,
			    (uint16_t)reg);
}

void
device_md_pci_write(unsigned int bus, unsigned int slot, unsigned int func,
		    unsigned int reg, unsigned int value)
{
	pci_cfg_write(0, (uint8_t)bus, (uint8_t)slot, (uint8_t)func,
		      (uint16_t)reg, (uint32_t)value);
}

/*
 * The MADT's interrupt-source overrides carry a two-bit trigger-mode field:
 * 0 means "as the bus does it", 1 edge, 3 level.  For an ISA interrupt "as
 * the bus does it" is edge, which is why the default is not level.
 *
 * ⚠️ There is no 8259 in this answer.  i386 asks whether an I/O APIC is in
 * use before deciding whom to ask; here there is nothing else to ask, and a
 * machine with no I/O APIC has no interrupts routed at all -- which
 * ioapic_present() reports rather than this guessing from a level.
 */
int
device_md_irq_is_level(unsigned int irq)
{
	if (!ioapic_present())
		return 0;

	return (acpi_irq_flags((uint8_t)irq) & ACPI_TRIGGER_MASK)
		== ACPI_TRIGGER_LEVEL;
}

/*
 * ⚠️ The ISA interrupt number is translated to a global system interrupt
 * before the pin is touched.  They are not the same number: the firmware may
 * say that ISA 0 arrives on GSI 2, and it usually does.  Masking pin 0
 * because the caller said 0 would leave the timer running and silence
 * something else.
 */
void
device_md_irq_mask(unsigned int irq)
{
	if (ioapic_present())
		ioapic_mask(acpi_irq_to_gsi((uint8_t)irq));
}

void
device_md_irq_unmask(unsigned int irq)
{
	if (ioapic_present())
		ioapic_unmask(acpi_irq_to_gsi((uint8_t)irq));
}

unsigned int
device_md_io_read(unsigned int port, unsigned int size)
{
	switch (size) {
	case 1:	return inb((uint16_t)port);
	case 2:	return inw((uint16_t)port);
	case 4:	return inl((uint16_t)port);
	}
	return 0;
}

void
device_md_io_write(unsigned int port, unsigned int size, unsigned int value)
{
	switch (size) {
	case 1:	outb((uint16_t)port, (uint8_t)value); break;
	case 2:	outw((uint16_t)port, (uint16_t)value); break;
	case 4:	outl((uint16_t)port, (uint32_t)value); break;
	}
}

/*
 * ── Claiming a line ───────────────────────────────────────────────────
 *
 * 🔑 There is no vector allocator here, and there does not need to be one.
 * <trap/trap.h> gives 0x40..0x4F to the legacy interrupt requests and
 * <cpu/spl.h> makes a vector's priority its top four bits, so the sixteen
 * vectors of class four are exactly the sixteen lines and the map is
 * vector = 0x40 + irq.  An allocator would be a table deciding what the
 * arithmetic already says, and a second thing to keep in step with the
 * priority.
 *
 * ⚠️ Sixteen because class four holds sixteen vectors, which is a fact about
 * the hardware -- not because device_master.c forwards sixteen lines, which
 * is a fact about that file.  The two numbers agree today and are checked
 * separately, because a limit two files agree on is the limit that stops
 * being agreed on.
 *
 * ⚠️ And sixteen is the ISA lines only.  A PCI device on a Q35 arrives on a
 * global system interrupt of 16 or above, and reaching it needs the routing
 * table in the firmware's AML -- which nothing here interprets.  That is a
 * gap with a shape, not a rounding: see #457.
 */
#define	DEVICE_MD_IRQ_MAX	16
#define	DEVICE_MD_VECTOR(irq)	(IOAPIC_ISA_VECTOR_BASE + (irq))

static device_md_intr_t	irq_handler[DEVICE_MD_IRQ_MAX];

/*
 * One trampoline for all sixteen, because the frame carries the vector and
 * the vector carries the line.  Sixteen near-identical functions written by a
 * macro would be sixteen places for the arithmetic to be wrong in one of them.
 *
 * 🔴 THE ACKNOWLEDGEMENT IS NOT GIVEN ON THE REPLAY PATH.  A deferred
 * interrupt was acknowledged when it arrived -- trap_dispatch() does it, so
 * that the local APIC does not hold the class busy for the length of the
 * deferral -- and this handler is entered a second time when the level drops.
 * EOI clears the highest bit in service at the moment it is written, so a
 * second one here would dismiss whatever interrupt the processor is actually
 * in, early and silently.
 *
 * This is the first handler in the tree that can genuinely be deferred: the
 * timer and the cross-processor messages live at class fifteen, which SPLHI
 * is deliberately below.  <cpu/spl.h> said a handler could not be made
 * deferrable without being read; this is what reading one found.
 */
static void
device_md_irq_trampoline(struct trap_frame *frame)
{
	unsigned int		irq = (unsigned int)frame->vector
					- IOAPIC_ISA_VECTOR_BASE;
	device_md_intr_t	handler;

	/*
	 * The vector is the one field a replayed frame is allowed to carry
	 * (trap_replay_vector()), which is what lets this trampoline be
	 * reached both ways.
	 */
	if (irq < DEVICE_MD_IRQ_MAX) {
		handler = irq_handler[irq];
		if (handler != 0)
			handler((int)irq);
	}

	if (!trap_in_replay())
		lapic_eoi();
}

/*
 * ⚠️ Nothing is ever displaced on this machine, and the contract's promise to
 * put back what was there is kept by there being nothing.  i386 means it --
 * ddb's keyboard handler claims IRQ 1 in the kernel and the userspace driver
 * takes the line over from it -- but no in-kernel driver here claims a device
 * line: the clock is on the local APIC's own timer, not a pin.  The day one
 * does, this is where the saving goes, and the overwrite below becomes wrong
 * in a way that would otherwise be silent.
 */
int
device_md_irq_register(unsigned int irq, device_md_intr_t handler)
{
	uint32_t	gsi;

	if (irq >= DEVICE_MD_IRQ_MAX || handler == 0)
		return 0;

	if (!ioapic_present())
		return 0;

	/*
	 * The pin, which is not the line number.  The firmware is entitled to
	 * say that ISA 0 arrives on GSI 2, and on a PC it does.
	 */
	gsi = acpi_irq_to_gsi((uint8_t)irq);
	if (gsi >= ioapic_pin_count())
		return 0;

	/*
	 * Handler, then vector, then pin -- each one is only reachable through
	 * the next, so this order is the one where nothing is reachable before
	 * it is ready.  Routing is what makes the interrupt arrive, and on a
	 * machine with more than one processor it can arrive before this
	 * function returns.
	 */
	irq_handler[irq] = handler;
	trap_set_handler(DEVICE_MD_VECTOR(irq), device_md_irq_trampoline);
	ioapic_route(gsi, (uint8_t)DEVICE_MD_VECTOR(irq), lapic_id(),
		     acpi_irq_flags((uint8_t)irq));

	return 1;
}

void
device_md_irq_unregister(unsigned int irq)
{
	if (irq >= DEVICE_MD_IRQ_MAX)
		return;

	/*
	 * And the mirror: the pin first, because until it is masked the line
	 * can still deliver, and it must deliver to something.
	 */
	if (ioapic_present())
		ioapic_mask(acpi_irq_to_gsi((uint8_t)irq));

	irq_handler[irq] = 0;
	trap_set_handler(DEVICE_MD_VECTOR(irq), 0);
}

/*
 * The console break (#382, and #428 for the debugger it reaches).
 *
 * ⚠️ NO PARK HERE, and its absence is the answer rather than a gap.  i386
 * stops the other processors before entering, because its debugger does not;
 * this one does it itself on the way in -- ddb_stop_others(), with the owner
 * taken by compare-and-swap so that two processors arriving together cannot
 * both think they are the operator.  Parking them here as well would be a
 * park racing the park that follows it.
 *
 * ⚠️ And the flag is `-r' and not i386's `-K'.  Two debuggers, two arming
 * flags, and the flag belongs to the debugger rather than to the console
 * driver that noticed the key.
 */
int
device_md_debugger_break(void)
{
	if (!ddb_enabled())
		return 0;

	Debugger("console break");
	return 1;
}
