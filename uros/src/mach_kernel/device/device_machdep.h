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
 * `slot' comes back as an interrupt number in the caller's own numbering,
 * continuing the lines rather than starting a second space: from the
 * forwarding table's point of view a notification slot is a notification
 * slot, and whether it arrives on a pin or in a store is exactly what this
 * file exists to keep out of that table.
 *
 * Answers zero if the machine has no message-signalled interrupts, or none
 * left.  ⚠️ i386 answers zero always and means it: this tree's i386 has no
 * MSI at all, and a machine that cannot must say so rather than hand back an
 * address that is not one.
 */
extern int		device_md_msi_register(device_md_intr_t handler,
					       unsigned int *slot_out,
					       unsigned long long *addr_out,
					       unsigned int *data_out);
extern void		device_md_msi_unregister(unsigned int slot);

#endif	/* _DEVICE_DEVICE_MACHDEP_H_ */
