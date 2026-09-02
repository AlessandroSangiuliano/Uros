/*
 * Copyright 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 *	device_machdep.h — what the device master needs from the machine (#457).
 *
 *	device_master.c is fourteen RPCs through which a user-space driver
 *	reaches hardware, and almost none of it is machine-dependent: the port
 *	checks, the task lookups, the mapping and the interrupt bookkeeping are
 *	the same everywhere.  What differs is nine operations, and they are
 *	named here so the RPC file can be read without knowing which machine it
 *	is on.
 *
 *	🔑 A contract and not a ladder of #if.  The file used to include
 *	<i386/pci/pci.h>, <i386/pci/pcibios.h>, <i386/ipl.h>, <i386/pio.h>,
 *	<i386/misc_protos.h> and <i386/ioapic.h> outside any conditional --
 *	which is why compiling it for x86-64 pulled in <mach/i386/vm_types.h>
 *	behind them, where vm_offset_t is thirty-two bits.  Six declarations in
 *	one place cost less than six conditionals in six, and the next machine
 *	inherits a list of what it has to answer rather than a diff to make.
 *
 *	⚠️ The two implementations are not the same shape underneath, and that
 *	is the point of naming the operation instead of the mechanism:
 *
 *	  configuration space   i386 reaches it through the BIOS-era tag;
 *	                        x86-64 through ECAM where the machine has one
 *	                        and the port pair where it does not
 *
 *	  masking an interrupt  i386 has an 8259 behind an I/O APIC that may or
 *	                        may not be in use; x86-64 has no 8259 in the
 *	                        picture at all and masks the pin
 *
 *	  claiming a line       i386 writes a handler, a unit and a priority
 *	                        into three parallel arrays the dispatch reads;
 *	                        x86-64 picks a vector, claims it in the IDT
 *	                        dispatch table and points an I/O APIC pin at it
 *
 *	  entering the          i386 must stop the other processors before it
 *	  debugger              does; x86-64's debugger stops them itself
 */

#ifndef	_DEVICE_DEVICE_MACHDEP_H_
#define	_DEVICE_DEVICE_MACHDEP_H_

#include <mach/machine/vm_types.h>

/*
 * One aligned 32-bit configuration register.
 *
 * ⚠️ Addressed by bus/device/function and not by a tag: a tag is a thing one
 * of the two mechanisms happens to have, and the RPC does not have one to
 * give.  Segment zero is implied -- the interface above has no field for a
 * segment either, which is #427's other half and is written down there.
 *
 * A read of a function that is not there answers 0xFFFFFFFF, because that is
 * what the bus returns.  A caller enumerating has to know that number.
 */
extern unsigned int	device_md_pci_read(unsigned int bus, unsigned int slot,
					   unsigned int func, unsigned int reg);
extern void		device_md_pci_write(unsigned int bus, unsigned int slot,
					    unsigned int func, unsigned int reg,
					    unsigned int value);

/*
 * Is this interrupt level-triggered?
 *
 * Asked because a level-triggered line has to be masked until the driver
 * says it has been serviced, and an edge-triggered one does not.  A machine
 * that cannot tell answers 0: treating a level line as edge loses interrupts,
 * treating an edge line as level does not, so the safe direction is the one
 * that keeps the mask in place.
 */
extern int		device_md_irq_is_level(unsigned int irq);

/* Stop and resume delivery of one interrupt. */
extern void		device_md_irq_mask(unsigned int irq);
extern void		device_md_irq_unmask(unsigned int irq);

/*
 * One I/O port access, `size' bytes wide (1, 2 or 4).
 *
 * 🔑 Here rather than behind a per-machine <io.h>, because the operation is
 * what the RPC has and the instruction is what the machine has.  i386 spells
 * the port type i386_ioport_t and x86-64 has no such name; naming the access
 * means neither spelling reaches this file.
 *
 * ⚠️ A size that is not 1, 2 or 4 reads as zero and writes nothing.  The RPC
 * above validates it, and this is the second half of that check rather than a
 * repetition of it: a machine asked for a width it does not have must not
 * pick the nearest one.
 */
extern unsigned int	device_md_io_read(unsigned int port, unsigned int size);
extern void		device_md_io_write(unsigned int port, unsigned int size,
					   unsigned int value);

/*
 * What runs when the line fires.  Called with the interrupt number, in
 * interrupt context, on whichever processor took it.
 */
typedef void	(*device_md_intr_t)(int irq);

/*
 * Make `handler' the thing that runs for `irq', and put back whatever was
 * there before.
 *
 * 🔴 THERE IS NO PRIORITY ARGUMENT, and its absence is the design.
 *
 * i386 takes one -- take_irq(pic, unit, spl, intr) -- because there the level
 * is a number kept beside the handler in intpri[], consulted by the dispatch
 * in interrupt.S.  x86-64 cannot take one, because there a vector's priority
 * class IS its top four bits (<cpu/spl.h>): choosing the vector is choosing
 * the priority, and there is no second table to put a level in.  A machine
 * handed SPL6 there would have to either ignore it -- an argument that is not
 * an argument -- or route the line to a vector picked to match, which is
 * choosing hardware from a software constant.
 *
 * So the operation says which line and what runs, and the machine answers at
 * the level it has for devices.  That is what both of them meant anyway: the
 * only value this file's caller ever passed was SPL6, once, for every line.
 *
 * 🔑 And the machine remembers what it displaced.  It used to be the caller
 * that did, in three arrays of intr_t, unit and spl -- which is i386's idea of
 * a handler, its idea of an argument, and a priority this machine does not
 * have, all held in machine-independent code so that unregister could put
 * them back.  Saved here, the shape of what was there is the machine's
 * business and unregister needs no arguments beyond the line.
 *
 * Answers zero if the line cannot be claimed -- there is no vector for it, or
 * no controller to route it through -- and in that case nothing was displaced
 * and unregister must not be called.
 *
 * ⚠️ One handler per line.  Sharing an interrupt is a thing PCI does and this
 * cannot express; the caller above enforces one registration per line, and
 * the day that changes it changes here first.
 */
extern int		device_md_irq_register(unsigned int irq,
					       device_md_intr_t handler);
extern void		device_md_irq_unregister(unsigned int irq);

/*
 * The console driver saw the break key: stop here, if a debugger was asked
 * for.  Answers whether it did -- zero means none is armed and the driver
 * should deliver the byte as ordinary input.
 *
 * Returns when the operator continues, in the driver's own RPC context (#382).
 *
 * 🔑 The whole operation and not just "is the debugger armed", because what
 * has to happen around the entry is not the same on the two machines.  i386
 * stops the other processors HERE, before anything slow: with an NMI park
 * done later, one of them can start a TLB shootdown in the window and wait
 * for an acknowledgement from a processor that is about to stop.  x86-64's
 * debugger stops them itself on the way in, so a second park here would be a
 * park racing a park.  A predicate would have left that difference at the
 * call site, in machine-independent code, spelt as an #if.
 *
 * ⚠️ THIS ONE WAS NOT FOUND BY READING THE INCLUDES.  The other eight
 * announced themselves as <i386/...> at the top of device_master.c; this one
 * declared `ddb_kbd_break_enabled', `ddb_nmi_park' and
 * `lapic_send_nmi_all_excluding_self' as externs beside the code that used
 * them, where an enumeration of the file's includes cannot see them.  It
 * surfaced as three undefined symbols at link time, which is the check that
 * did work -- and the reason the header says eight anywhere is a mistake this
 * one corrected.
 */
extern int		device_md_debugger_break(void);

/*
 * ── An interrupt that is not a wire ──────────────────────────────────
 *
 * Claim a message-signalled interrupt: answer the address and the value a
 * device must be programmed to WRITE in order to raise it, and make
 * `handler' the thing that runs when it does.
 *
 * 🔑 Nothing is routed, because there is nothing to route.  A pin is a
 * property of the machine's wiring that the firmware describes and the
 * interrupt controller has to be told about; a message is an ordinary store
 * to an ordinary address, and what makes it an interrupt is only where that
 * address points.  Which is why this operation takes no device, no bus and no
 * line: it hands out an address and a value, and who writes them is the
 * caller's business.
 *
 * ⚠️ AND WHY IT IS THE KERNEL THAT PROGRAMS THE DEVICE.  The address is
 * ordinary as far as the device is concerned, so a driver that could write
 * its own MSI-X table could aim the device at any address in the machine --
 * the same hole #432 and #511 exist to close.  This operation exists so that
 * the caller above can be the kernel rather than the driver.
 *
 * 🔴 AND THE CALLER NEVER SEES THE ADDRESS.  It names a DEVICE and one of
 * that device's table entries; the machine finds the capability, allocates a
 * vector, decides the address and the value, and writes them into the
 * device's table itself.  An interface that answered an address instead would
 * be an interface whose caller could choose one -- and the caller above this
 * is an RPC a user-space driver reaches.
 *
 * `slot' comes back as an interrupt number in the caller's own numbering,
 * continuing the lines rather than starting a second space: from the
 * forwarding table's point of view a notification slot is a notification
 * slot, and whether it arrives on a pin or in a store is exactly what this
 * file exists to keep out of that table.  So device_intr_enable() and the
 * unregister below take it without knowing which kind it is.
 *
 * Answers zero if the machine has no message-signalled interrupts, if that
 * device has none, or if there is no slot left.  ⚠️ i386 answers zero always
 * and means it: this tree's i386 has no MSI at all, and a machine that cannot
 * must say so rather than accept a request it will not honour.
 */
extern int		device_md_msi_register(unsigned int bus,
					       unsigned int dev,
					       unsigned int func,
					       unsigned int entry,
					       device_md_intr_t handler,
					       unsigned int *slot_out);

/*
 * Give a message-signalled slot back.
 *
 * 🔴 Unlike a line, this has to reach the DEVICE.  Masking a pin stops
 * delivery whatever the device does; here the address and the value are in
 * the device's own table, so the machine must put the mask back there -- and
 * it can, because it is the one that wrote them.  A device left armed at a
 * vector whose handler is gone raises an interrupt nobody claims.
 */
extern void		device_md_msi_unregister(unsigned int slot);

/*
 * ── What polices a device's DMA, if anything (#432) ──────────────────
 *
 * Whether this machine can confine a device to the memory it has been given.
 *
 * 🔴 A QUESTION AND NOT AN ASSUMPTION, because both answers are ordinary.  A
 * machine with no remapping hardware, or with translation left off, is a
 * machine on which a userspace driver reaches all of physical memory -- which
 * is what this tree did everywhere until now and still does on i386.  The
 * caller's job is to say so, not to pretend either way.
 */
extern int		device_md_dma_isolates(void);

/*
 * Make `size' bytes of physical memory at `pa' reachable by the device at
 * `bdf', and by nothing else new.  Answers non-zero when it is done.
 *
 * 🔑 The device is named as a packed bus/device/function because that is the
 * name the HARDWARE knows it by: an IOMMU indexes its tables by the requester
 * id on the bus, not by any handle this kernel invented.  DEVICE_DMA_NO_BDF is
 * a caller with no device of its own and must not reach here -- there is
 * nothing to grant to.
 *
 * 🔴 AND `*dma_addr' IS NOT THE PHYSICAL ADDRESS when the machine confines
 * this device.  It is the address the DEVICE must be programmed with, which is
 * what the caller wanted all along -- and on a machine that polices nothing it
 * is the physical address, so one interface answers both and no caller has to
 * know which kind of machine it is on.
 *
 * ⚠️ The first grant for a device is what takes it OFF pass-through, so a
 * failure here can leave it in a domain missing part of what was asked for.
 * The caller must treat that as a failed allocation: a half-granted buffer is
 * one a device will fault on at an address its driver believes it owns.
 *
 * i386 answers zero, and device_md_dma_isolates() answering zero is what says
 * that is a property of the machine rather than a failure.
 */
extern int		device_md_dma_grant(unsigned int bdf,
					    unsigned long pa,
					    unsigned long size,
					    int read, int write,
					    unsigned long *dma_addr);

/*
 * The same for `n' frames that are not physically contiguous: they are made
 * reachable at CONSECUTIVE addresses starting from the one answered.
 *
 * 🔑 One call and one window.  A scatter-gather buffer is scattered in
 * physical memory and there is no reason for it to be scattered in the
 * device's -- so a driver receives one address and a length, and the
 * scattering stays a fact about the machine.
 */
extern int		device_md_dma_grant_pages(unsigned int bdf,
						  const unsigned long *pa,
						  unsigned int n,
						  int read, int write,
						  unsigned long *dma_addr);

/*
 * Take a granted range back.  Answers non-zero when the device can no longer
 * reach it.
 *
 * ⚠️ The device stays in its domain.  A device whose last buffer is freed is a
 * device between transfers, and putting it back on pass-through would make
 * every free a window in which it reaches everything again.
 */
extern int		device_md_dma_revoke(unsigned int bdf,
					     unsigned long pa,
					     unsigned long size);

/*
 * How many of this device's DMA requests the machine has refused, and the last
 * address it refused.  Zero when it has refused none, and zero on a machine
 * that refuses nothing because it polices nothing.
 *
 * ⚠️ `*last' is left alone when the answer is zero, rather than being cleared.
 * A caller that ignored the count and read the address would then see whatever
 * it had put there itself -- which is a wrong answer it wrote, and far easier
 * to trace than a zero that looks like an address.
 */
extern unsigned		device_md_dma_faults(unsigned int bdf,
					     unsigned long *last);

/*
 * Whether this device is confined to what it has been granted right now.
 *
 * 🔑 Not the same question as device_md_dma_isolates(), and the difference is
 * the one that matters: that one says the MACHINE could confine a device, this
 * one says THIS device is confined.  A device that has never asked for a DMA
 * buffer is still passing through on a machine that polices perfectly.
 */
extern int		device_md_dma_confined(unsigned int bdf);

/*
 * Confine this device, but map its grants where the memory really is.
 *
 * ⚠️ Before its first grant only.  A domain that already holds translated
 * buffers cannot become an identity one without moving them, and moving them
 * means changing the addresses a device is reading through while it reads.
 */
extern int		device_md_dma_identity(unsigned int bdf);

#endif	/* _DEVICE_DEVICE_MACHDEP_H_ */
