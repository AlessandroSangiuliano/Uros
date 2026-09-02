/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	device_machdep.c — i386's answers to <device/device_machdep.h> (#457).
 *
 *	🔑 This is a MOVE and not a port.  Every line below was in
 *	device/device_master.c, calling the same functions with the same
 *	arguments; the only change is that it is now behind a name the RPC file
 *	can use without including <i386/...> outside a conditional.
 *
 *	⚠️ Which is what makes it checkable: i386's behaviour has to be
 *	unchanged, and "unchanged" is a claim about generated code rather than
 *	about intent.
 */

#include <device/device_machdep.h>

#include <chips/busses.h>	/* take_irq / reset_irq / intr_t */
#include <i386/pci/pci.h>
#include <i386/pci/pcibios.h>
#include <i386/ioapic.h>
#include <i386/ipl.h>		/* SPL6 */
#include <i386/misc_protos.h>
#include <i386/pic.h>		/* NINTR */
#include <i386/pio.h>

unsigned int
device_md_pci_read(unsigned int bus, unsigned int slot, unsigned int func,
		   unsigned int reg)
{
	pcici_t tag = pcitag((unsigned char)bus,
			     (unsigned char)slot,
			     (unsigned char)func);

	/*
	 * A tag the BIOS interface could not form names no device, and the bus
	 * answers for an absent function with all ones.  Saying so here keeps
	 * the caller from having to know that pcitag has a failure at all.
	 */
	if (!tag.cfg1)
		return 0xFFFFFFFFu;

	return (unsigned int)pci_conf_read(tag, reg);
}

void
device_md_pci_write(unsigned int bus, unsigned int slot, unsigned int func,
		    unsigned int reg, unsigned int value)
{
	pcici_t tag = pcitag((unsigned char)bus,
			     (unsigned char)slot,
			     (unsigned char)func);

	if (!tag.cfg1)
		return;

	pci_conf_write(tag, reg, value);
}

/*
 * ⚠️ Asked of the I/O APIC only when one is in use.  With the 8259 alone the
 * trigger mode is whatever the bus does, and this machine has no table that
 * says otherwise -- so it answers edge, which is the direction that does not
 * lose interrupts if it is wrong.
 */
int
device_md_irq_is_level(unsigned int irq)
{
	if (ioapic_active())
		return ioapic_irq_is_level(irq);
	return 0;
}

void
device_md_irq_mask(unsigned int irq)
{
	pic_irq_mask(irq);
}

void
device_md_irq_unmask(unsigned int irq)
{
	pic_irq_unmask(irq);
}

/*
 * ⚠️ i386_ioport_t, which is a name only this machine has.  Keeping the cast
 * here is the reason device_master.c no longer needs to know it exists.
 */
unsigned int
device_md_io_read(unsigned int port, unsigned int size)
{
	switch (size) {
	case 1:	return inb((i386_ioport_t)port);
	case 2:	return inw((i386_ioport_t)port);
	case 4:	return inl((i386_ioport_t)port);
	}
	return 0;
}

/*
 * What was on the line before the caller took it, so that giving it back is
 * possible without the caller knowing the shape of it.
 *
 * 🔑 These three arrays used to live in device/device_master.c, which is
 * machine-independent code holding an intr_t, a unit and an spl -- this
 * machine's idea of a handler, of its argument, and of a priority.  They are
 * here now because putting them back is this machine's business; see
 * <device/device_machdep.h> for why the register operation takes no priority.
 *
 * ⚠️ NINTR and not IRQ_FORWARD_MAX.  The caller's table is sixteen entries
 * because sixteen is what it forwards; this one is as wide as the machine has
 * lines, because it is indexed by whatever the caller was given.  Bounded
 * here as well as there: two files agreeing on a limit is the arrangement
 * that stops agreeing.
 */
static intr_t	irq_saved_handler[NINTR];
static int	irq_saved_unit[NINTR];
static int	irq_saved_spl[NINTR];

/*
 * SPL6 is the level every caller of this passed, every time.  It is spelt
 * here rather than taken as an argument because it is this machine's answer
 * to "the device level" and not a decision the caller was making -- see the
 * header.  On the i386 spl ladder it is where tty, net and imp already are.
 */
int
device_md_irq_register(unsigned int irq, device_md_intr_t handler)
{
	if (irq >= NINTR)
		return 0;

	/*
	 * reset_irq() first, and not merely for the saving: take_irq() on a
	 * line that already has a handler prints two lines and then spins
	 * forever, deliberately, because two devices on one line is a wiring
	 * fault it cannot resolve.  Clearing the line is what makes the claim
	 * below a claim rather than a coin toss.
	 */
	reset_irq((int)irq,
		  &irq_saved_unit[irq],
		  &irq_saved_spl[irq],
		  &irq_saved_handler[irq]);

	take_irq((int)irq, (int)irq, SPL6, (intr_t)handler);
	return 1;
}

/*
 * 🔴 GIVE THE LINE UP BEFORE PUTTING THE OLD HANDLER BACK.
 *
 * This is a FIX and not a move, and it is worth saying which.  device/
 * device_master.c did the restore with take_irq() alone:
 *
 *	take_irq(irq, irq_orig_unit[irq], irq_orig_spl[irq],
 *		 irq_orig_handler[irq]);
 *
 * take_irq() installs only when intpri[pic] is zero, and otherwise prints
 * "This device will clobber IRQ %d" and spins forever -- deliberately, since
 * two devices on one line is a wiring fault it cannot resolve.  But intpri[]
 * for this line is not zero: the matching register set it, to SPL6, a moment
 * ago.  So the restore took the else branch every time, and unregistering an
 * interrupt halted the machine.
 *
 * ⚠️ It reads as though it could not: take_irq() is guarded by
 *
 *	if (!mp_v1_1_take_irq(pic, unit, spl, intr))
 *
 * and mp_v1_1_take_irq() claims the line through the I/O APIC and answers
 * TRUE, which would skip the body entirely.  It does not here.  The build
 * compiles i386/AT386/mp/mp_stub.c and not mp_v1_1.c -- asked of the build
 * rather than assumed, because MP_V1_1 arrives as -DMP_V1_1=1 on the command
 * line and the generated header says 0 -- and the stub declines by contract,
 * exactly so that take_irq() runs its real body.
 *
 * The line has to be released first, which is what reset_irq() does, and only
 * then does take_irq() have a zero intpri to install into.
 */
void
device_md_irq_unregister(unsigned int irq)
{
	int	unit;
	int	spl;
	intr_t	intr;

	if (irq >= NINTR)
		return;

	reset_irq((int)irq, &unit, &spl, &intr);

	/*
	 * A line that had nothing on it saved a zero priority, and reset_irq()
	 * has already put it back to intnull with the pin masked.  Handing
	 * take_irq() a null handler at priority zero would install the null and
	 * then unmask the I/O APIC pin for it, which is a worse answer than
	 * doing nothing.
	 */
	if (irq_saved_spl[irq] == 0)
		return;

	take_irq((int)irq,
		 irq_saved_unit[irq],
		 irq_saved_spl[irq],
		 irq_saved_handler[irq]);
}

/*
 * The console break, and the park that has to come first (#382).
 *
 * ⚠️ The park is BEFORE anything slow on this processor, and that ordering is
 * the whole of why this is one operation and not a predicate.  A pre-park
 * serial printf alone takes milliseconds; in that window another processor
 * can start a TLB shootdown and wait for ever for an acknowledgement from
 * this one, which is about to stop.  The other processors then wedge inside
 * the 0xF1 handler before its EOI -- and an in-service 0xF1 pins the priority
 * register at 0xF0, so no device vector is ever delivered again.  The
 * debugger session looks like it works and the machine is already dead.
 *
 * ⚠️ And no printf before the park either: with db_active still zero, a
 * parked processor holding printf_lock would deadlock this one right here.
 * kdb_trap skips its own park when the flag is already up, and clears it on
 * the way out.
 */
extern int		ddb_kbd_break_enabled;	/* -K (model_dep.c) */
extern void		Debugger(const char *message);

int
device_md_debugger_break(void)
{
	if (!ddb_kbd_break_enabled)
		return 0;

#if	NCPUS > 1
	{
		extern volatile int	ddb_nmi_park;
		extern void		lapic_send_nmi_all_excluding_self(void);

		if (!ddb_nmi_park) {
			ddb_nmi_park = 1;
			lapic_send_nmi_all_excluding_self();
		}
	}
#endif	/* NCPUS > 1 */

	Debugger("console break");
	return 1;
}

void
device_md_io_write(unsigned int port, unsigned int size, unsigned int value)
{
	switch (size) {
	case 1:	outb((i386_ioport_t)port, (unsigned char)value); break;
	case 2:	outw((i386_ioport_t)port, (unsigned short)value); break;
	case 4:	outl((i386_ioport_t)port, (unsigned long)value); break;
	}
}

/*
 * 🔴 THIS MACHINE HAS NO MESSAGE-SIGNALLED INTERRUPTS, and says so.
 *
 * Not "not yet": this tree's i386 reaches interrupts through the 8259 and, on
 * more than one processor, an I/O APIC programmed from the MP tables.  Neither
 * is asked here.  What is missing is not code but a decision nobody has had to
 * make, because every device this target drives is on a wire.
 *
 * ⚠️ Answering zero rather than handing back an address.  An address is a
 * thing a caller programs a DEVICE with, and a plausible-looking one would be
 * written to by real hardware -- which is a store into whatever happens to be
 * at that physical address on a machine that never agreed to receive it.  The
 * one honest answer a machine without the mechanism can give is that it does
 * not have it.
 */
int
device_md_msi_register(unsigned int bus, unsigned int dev, unsigned int func,
		       unsigned int entry, device_md_intr_t handler,
		       unsigned int *slot_out)
{
	(void)bus;
	(void)dev;
	(void)func;
	(void)entry;
	(void)handler;
	(void)slot_out;
	return 0;
}

void
device_md_msi_unregister(unsigned int slot)
{
	(void)slot;
}

/*
 * 🔴 THIS MACHINE DOES NOT POLICE DMA, and says so rather than failing later.
 *
 * i386 here has no IOMMU and there is no plan to give it one: #432's argument
 * is that userspace drivers need remapping hardware to make their isolation a
 * security property rather than a convention, and the hardware that argument
 * is about is x86-64's.  So a driver on this target programs a device with a
 * physical address and the device reaches all of physical memory -- which is
 * what this tree has always done and is now, for the first time, a thing the
 * kernel can be ASKED about instead of a thing nobody mentions.
 *
 * ⚠️ Which is why the grant answers zero and does not pretend to succeed.  A
 * grant that returned success here would tell its caller a buffer is confined
 * when nothing confines it, and the caller would report isolation it does not
 * have -- a lie with a paper trail, which is worse than the honest gap.
 */
int
device_md_dma_isolates(void)
{
	return 0;
}

int
device_md_dma_grant(unsigned int bdf, unsigned long pa, unsigned long size,
		    int read, int write, unsigned long *dma_addr)
{
	(void)bdf;
	(void)pa;
	(void)size;
	(void)read;
	(void)write;
	(void)dma_addr;
	return 0;
}

int
device_md_dma_grant_pages(unsigned int bdf, const unsigned long *pa,
			  unsigned int n, int read, int write,
			  unsigned long *dma_addr)
{
	(void)bdf;
	(void)pa;
	(void)n;
	(void)read;
	(void)write;
	(void)dma_addr;
	return 0;
}

int
device_md_dma_revoke(unsigned int bdf, unsigned long pa, unsigned long size)
{
	(void)bdf;
	(void)pa;
	(void)size;
	return 0;
}

unsigned
device_md_dma_faults(unsigned int bdf, unsigned long *last)
{
	(void)bdf;
	(void)last;
	return 0;
}

int
device_md_dma_confined(unsigned int bdf)
{
	(void)bdf;
	return 0;
}

int
device_md_dma_identity(unsigned int bdf)
{
	(void)bdf;
	return 0;
}
