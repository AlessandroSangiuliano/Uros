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
#include <cpu/iommu.h>	/* #432: what polices DMA, if anything */
#include <cpu/ioapic.h>
#include <cpu/lapic.h>	/* lapic_eoi, and which processor to route to */
#include <cpu/pci_cfg.h>
#include <cpu/pci_msix.h>	/* #457: a device's own table */
#include <cpu/regs.h>	/* inb/outl and the other widths */
#include <ddb/ddb.h>	/* whether a debugger was asked for */
#include <kern/misc_protos.h>	/* printf */
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

/*
 * ── And sixteen more that are not lines ──────────────────────────────
 *
 * Message-signalled interrupts continue the numbering rather than starting a
 * second space: slots 16..31, vectors 0x50..0x5F.  The trampoline below needs
 * no case for them, because `vector - 0x40' indexes both halves -- which is
 * the whole point of a numbering that continues instead of restarting.
 *
 * ⚠️ The block above is the class ABOVE the pinned one, and <cpu/spl.h>'s
 * SPL_DEVICE was raised to five so that "the device level" means both kinds.
 * On this machine the vector IS the priority; putting these somewhere else in
 * the space would have been changing what a spl call excludes.
 */
#define	DEVICE_MD_MSI_BASE	16
#define	DEVICE_MD_MSI_MAX	16
#define	DEVICE_MD_SLOTS		(DEVICE_MD_IRQ_MAX + DEVICE_MD_MSI_MAX)

static device_md_intr_t	irq_handler[DEVICE_MD_SLOTS];

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
	if (irq < DEVICE_MD_SLOTS) {
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

/*
 * ── An interrupt that is not a wire ──────────────────────────────────
 *
 * 🔑 The whole of "delivering" an MSI is an ordinary 32-bit store to an
 * ordinary physical address.  What makes it an interrupt is only where that
 * address points: 0xFEE00000 and up is the region the local APICs answer, and
 * the bits of the address say WHICH processor while the bits of the value say
 * WHICH vector.  There is nothing to route and no controller to program --
 * which is exactly why a device with MSI needs no entry in the firmware's
 * interrupt routing table, and why sixteen ISA lines stop being the ceiling.
 *
 * ⚠️ And exactly why an MSI-X table a driver could write is the same hole as
 * a DMA engine a driver could aim: from the device's side this is a store, and
 * the address it stores to is whatever the table says.  A driver that could
 * write 0x0 there would have the device scribble on page zero instead; one
 * that could write another processor's APIC address would deliver interrupts
 * nobody asked for.  It is the kernel that programs the table (#432, #511).
 *
 * The encodings below are the architecture's, in its "compatibility" format,
 * which is the one that exists on every x86 with a local APIC:
 *
 *   address   0xFEE00000 | (destination APIC id << 12)
 *             the low bits select redirection hints this does not use --
 *             physical destination, no redirection, for the reason
 *             ioapic_route() gives: an interrupt handled twice is worse than
 *             one handled slowly, and lowest-priority delivery hands the
 *             choice to the hardware before there is a scheduler with an
 *             opinion.
 *
 *   data      the vector, with delivery mode 000 (fixed) and the trigger bits
 *             clear.  ⚠️ An MSI is always edge -- the spec says level-trigger
 *             in the data word is for a bridge translating a wire, and a real
 *             MSI has no wire to be level ON.  So the #381 note about masking
 *             an edge line applies here in full.
 */
#define	MSI_ADDRESS_BASE	0xFEE00000ULL
#define	MSI_ADDRESS_DEST(id)	(((unsigned long long)(id) & 0xFFu) << 12)

static unsigned int	msi_next;	/* slots are handed out in order */

/*
 * Claim a vector and say what a device must write to reach it.
 *
 * 🔑 Exported for the boot self-test and for device_md_msi_register() below,
 * and for nothing else.  It hands out an ADDRESS, which is the thing the
 * contract above refuses to hand out -- the difference between the two is
 * exactly who is trusted with it, and the answer is "this file and the test
 * that has no device".
 */
int
msi_claim_vector(device_md_intr_t handler, unsigned int *slot_out,
		 unsigned long long *addr_out, unsigned int *data_out)
{
	unsigned int	slot;
	unsigned int	vector;

	if (handler == 0 || slot_out == 0 || addr_out == 0 || data_out == 0)
		return 0;

	/*
	 * ⚠️ No local APIC means no address that means anything.  Answering an
	 * address anyway would hand back a number a device would faithfully
	 * write to, and the write would land in whatever is at 0xFEE00000 on a
	 * machine that has no APIC there.
	 */
	if (!lapic_present())
		return 0;

	/*
	 * Handed out in order and never reclaimed by search, because a search
	 * would need the table to say which slots are free and the handler
	 * pointer is not that: a slot whose handler is zero may be one that was
	 * never used or one a device is still programmed to write to.  ⚠️ Which
	 * makes unregister leave the slot spent -- see below.
	 */
	if (msi_next >= DEVICE_MD_MSI_MAX)
		return 0;

	slot = DEVICE_MD_MSI_BASE + msi_next;
	msi_next++;
	vector = DEVICE_MD_VECTOR(slot);

	/*
	 * The handler before the address, for the reason register does it in
	 * that order for a line: the address is what makes the interrupt
	 * possible, and a caller that programmed a device with it could see the
	 * first one arrive before this call returns.
	 */
	irq_handler[slot] = handler;
	trap_set_handler(vector, device_md_irq_trampoline);

	*slot_out = slot;
	*addr_out = MSI_ADDRESS_BASE | MSI_ADDRESS_DEST(lapic_id());
	*data_out = vector;

	return 1;
}

/*
 * 🔴 THE SLOT IS SPENT, NOT FREED, and that is a decision rather than an
 * omission.
 *
 * Giving up a line means masking a pin: after that the controller will not
 * deliver, whatever the device does.  There is no equivalent here.  The
 * address and the value are in the device's own table, this side cannot see
 * them, and nothing this function does can stop a device that still has them
 * from writing.  Handing the slot to a second caller would mean two
 * subsystems sharing a vector, one of which does not know the other exists.
 *
 * So the handler goes -- an arriving message finds nothing to run and is
 * acknowledged, which is a lost interrupt and not a wild call -- and the slot
 * is not reused.  Sixteen of them, and reclaiming one honestly means the
 * kernel owning the device's table, which is where #457 is going anyway.
 */
static void
msi_release_vector(unsigned int slot)
{
	if (slot < DEVICE_MD_MSI_BASE || slot >= DEVICE_MD_SLOTS)
		return;

	irq_handler[slot] = 0;
	trap_set_handler(DEVICE_MD_VECTOR(slot), 0);
}

/*
 * ── And the operation the contract actually names ────────────────────
 *
 * The caller names a device and one of its table entries.  This finds the
 * capability, claims a vector, decides the address and the value, and writes
 * them into the device's table -- so no address ever crosses back, which is
 * the whole reason the contract is shaped that way (<device/device_machdep.h>).
 *
 * ⚠️ The device is REMEMBERED, because unregister has to reach it.  Giving up
 * a line means masking a pin and the controller stops delivering whatever the
 * device does; here the address is in the device's own table and the only way
 * to stop it is to put the mask back there.  Which this side can do, and only
 * because it is the side that wrote them.
 */
static struct pci_msix	msi_device[DEVICE_MD_MSI_MAX];
static unsigned int	msi_entry_of[DEVICE_MD_MSI_MAX];

int
device_md_msi_register(unsigned int bus, unsigned int dev, unsigned int func,
		       unsigned int entry, device_md_intr_t handler,
		       unsigned int *slot_out)
{
	struct pci_msix		m;
	unsigned int		slot = 0, data = 0;
	unsigned long long	addr = 0;

	if (slot_out == 0 || handler == 0)
		return 0;

	if (!pci_msix_probe(0, (uint8_t)bus, (uint8_t)dev, (uint8_t)func, &m))
		return 0;

	if (entry >= m.vectors)
		return 0;

	if (!msi_claim_vector(handler, &slot, &addr, &data))
		return 0;

	/*
	 * The table before the enable, so the device cannot be let loose on an
	 * entry that has not been written yet -- and the entry is armed by
	 * pci_msix_arm()'s last store, which is what makes "written" a moment
	 * rather than a stretch.
	 */
	pci_msix_arm(&m, entry, addr, data);
	pci_msix_enable(&m);

	msi_device[slot - DEVICE_MD_MSI_BASE] = m;
	msi_entry_of[slot - DEVICE_MD_MSI_BASE] = entry;

	*slot_out = slot;
	return 1;
}

void
device_md_msi_unregister(unsigned int slot)
{
	unsigned int i;

	if (slot < DEVICE_MD_MSI_BASE || slot >= DEVICE_MD_SLOTS)
		return;

	i = slot - DEVICE_MD_MSI_BASE;

	/*
	 * 🔴 The DEVICE first, and the handler second.  Between the two an
	 * arriving message finds a handler that still knows what to do with it;
	 * the other order leaves a window where the device is still armed at a
	 * vector nobody claims, and an unclaimed vector halts the machine.
	 */
	if (msi_device[i].table != 0) {
		struct pci_msix	gone = msi_device[i];
		unsigned int	j;
		int		last = 1;

		pci_msix_disarm(&msi_device[i], msi_entry_of[i]);
		msi_device[i].table = 0;

		/*
		 * 🔴 AND THE FUNCTION'S OWN ENABLE BIT, which nothing used to
		 * put back (#520).  Disarming the entry stops the interrupt;
		 * it leaves the device MSI-X-enabled, and a device left in a
		 * state its driver did not choose is a device whose registers
		 * may not be where the driver looks for them.  A legacy
		 * virtio-pci device moves its configuration by four bytes on
		 * exactly this bit.
		 *
		 * ⚠️ Only when this was the LAST entry of that function.  The
		 * enable is per function and the entries are per vector, so
		 * clearing it with another vector still armed would silence an
		 * interrupt nobody asked to silence.  Compared by bus address
		 * rather than by mapping, because two slots of one device are
		 * two records of the same function.
		 */
		for (j = 0; j < DEVICE_MD_MSI_MAX; j++)
			if (msi_device[j].table != 0
			    && msi_device[j].segment == gone.segment
			    && msi_device[j].bus == gone.bus
			    && msi_device[j].dev == gone.dev
			    && msi_device[j].func == gone.func) {
				last = 0;
				break;
			}

		if (last)
			pci_msix_disable(&gone);
	}

	msi_release_vector(slot);
}

/*
 * ── What polices this machine's DMA (#432 stage 3d) ──────────────────
 *
 * The three calls device_master.c needs to confine a device, and the whole of
 * what the machine-independent side has to know about remapping hardware: it
 * asks whether there is any, and grants and revokes ranges by physical
 * address.  Which vendor, how many engines, what a page table looks like and
 * how an engine is made to forget are all below this line.
 *
 * 🔑 A DEVICE IS NAMED BY ITS BUS ADDRESS AND NOTHING ELSE.  That is the name
 * an IOMMU indexes by, so it is the name that crosses here -- no handle, no
 * pointer, nothing this kernel invented and would then have to translate.
 */
int
device_md_dma_isolates(void)
{
	return iommu_can_isolate();
}

int
device_md_dma_grant(unsigned int bdf, unsigned long pa, unsigned long size,
		    int read, int write, unsigned long *dma_addr)
{
	uint64_t iova = 0;
	unsigned before;
	int ok;

	/*
	 * ⚠️ The width is checked here rather than trusted from the caller.  A
	 * bdf is sixteen bits on the bus and the RPC that carries it is a
	 * natural_t, so a caller with a stale idea of the encoding would index
	 * the device table with a number that is not a device -- which on AMD
	 * lands in a 65536-entry table and reaches whatever entry that is.
	 */
	if (bdf > 0xFFFFu)
		return 0;

	/*
	 * 🔑 SAID ONCE PER DEVICE, on the grant that moves it.  A device
	 * leaving pass-through is the single most consequential thing #432
	 * does -- from that instant it reaches what it was given and nothing
	 * else -- and it happens deep inside an RPC, at a moment no boot log
	 * would otherwise mention.  Printing it on every grant would bury it;
	 * printing it when the count of domains goes up names each device
	 * exactly once.
	 */
	before = iommu_domain_count();
	ok = iommu_grant((uint16_t)bdf, (uint64_t)pa, (uint64_t)size,
			 read, write, &iova);

	if (ok && iommu_domain_count() != before)
		printf("iommu: %02x:%02x.%u is now in a domain of its own, "
		       "and sees its first buffer at 0x%lx — an address that "
		       "is not where the memory is\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7), (unsigned long)iova);

	if (ok && dma_addr != 0)
		*dma_addr = (unsigned long)iova;

	return ok;
}

int
device_md_dma_grant_pages(unsigned int bdf, const unsigned long *pa,
			  unsigned int n, int read, int write,
			  unsigned long *dma_addr)
{
	uint64_t iova = 0;
	unsigned before;
	int ok;

	if (bdf > 0xFFFFu)
		return 0;

	/*
	 * 🔑 The list arrives as unsigned long and the engine wants uint64_t,
	 * and on this machine those are the same width -- which is exactly why
	 * the cast is written rather than the array passed straight through.
	 * The day a target has them differ, this is the line that will not
	 * compile instead of the one that silently reads half of each entry.
	 */
	_Static_assert(sizeof(unsigned long) == sizeof(uint64_t),
		       "the page list is handed to the iommu unconverted");

	before = iommu_domain_count();
	ok = iommu_grant_pages((uint16_t)bdf, (const uint64_t *)pa, n,
			       read, write, &iova);

	if (ok && iommu_domain_count() != before)
		printf("iommu: %02x:%02x.%u is now in a domain of its own, "
		       "and sees its first buffer at 0x%lx — an address that "
		       "is not where the memory is\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7), (unsigned long)iova);

	if (ok && dma_addr != 0)
		*dma_addr = (unsigned long)iova;

	return ok;
}

int
device_md_dma_revoke(unsigned int bdf, unsigned long pa, unsigned long size)
{
	if (bdf > 0xFFFFu)
		return 0;

	return iommu_revoke((uint16_t)bdf, (uint64_t)pa, (uint64_t)size);
}

unsigned
device_md_dma_faults(unsigned int bdf, unsigned long *last)
{
	uint64_t address = 0;
	unsigned n;

	if (bdf > 0xFFFFu)
		return 0;

	/*
	 * 🔴 DRAINED HERE, AND THAT IS THE POINT OF THE CALL.  The engines'
	 * fault records are otherwise read when a processor next goes idle,
	 * which is after the driver has given up -- so a driver asking "was I
	 * refused" would be told no about the refusal it is asking about.
	 * Reporting as well as draining, because the kernel's log is where
	 * anybody reading this afterwards will look.
	 */
	(void) iommu_fault_report();

	n = iommu_faults_for((uint16_t)bdf, &address);
	if (n != 0 && last != 0)
		*last = (unsigned long)address;

	return n;
}

int
device_md_dma_confined(unsigned int bdf)
{
	if (bdf > 0xFFFFu)
		return 0;

	return iommu_domain_of((uint16_t)bdf) != 0;
}

int
device_md_dma_identity(unsigned int bdf)
{
	if (bdf > 0xFFFFu)
		return 0;

	return iommu_domain_identity((uint16_t)bdf);
}

int
device_md_dma_release(unsigned int bdf)
{
	if (bdf > 0xFFFFu)
		return 0;

	return iommu_domain_release((uint16_t)bdf);
}
