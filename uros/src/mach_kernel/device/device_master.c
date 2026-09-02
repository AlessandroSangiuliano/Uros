/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * device/device_master.c
 *
 * Kernel primitives for userspace drivers: PCI config access,
 * interrupt forwarding, and DMA buffer allocation.
 *
 * All RPCs require the master_device_port (privileged).
 */

#include <pci.h>

#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <mach/boolean.h>
#include <mach/vm_param.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>
#include <kern/ipc_mig.h>
#include <kern/ipc_tt.h>
#include <kern/task.h>
#include <ipc/ipc_port.h>
#include <vm/vm_kern.h>
#include <vm/vm_map.h>
#include <vm/vm_user.h>	/* vm_allocate, for the out-of-line list (#520) */
#include <vm/pmap.h>
#include <device/device_master.h>
#include <device/device_port.h>

/*
 * What this file needs from the machine, named once (#457).
 *
 * 🔑 It used to include six <i386/...> headers outside any conditional, which
 * is why compiling it for x86-64 pulled <mach/i386/vm_types.h> in behind them
 * -- where vm_offset_t is thirty-two bits -- and is why the whole subsystem
 * sat behind MACH_DEVICE_MASTER=0 on this target.  Six operations, one
 * contract, and each machine answers it in its own device_machdep.c.
 */
#include <device/device_machdep.h>
#include <machine/spl.h>

#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/kalloc.h>

/*
 * #448: the interface, so the compiler can compare it with what is below.
 *
 * These are MIG entry points.  The generated stub is compiled against
 * device_master_server.h in its own translation unit; without that header
 * here, this file's own idea of each signature is never checked against it,
 * and two self-consistent halves can disagree indefinitely -- which is
 * exactly how #448 put a thread_t where the interface said thread_act_t and
 * nothing noticed until a caller finally arrived through MIG.
 *
 * The fourteen signatures below agree today, and this include is what will
 * keep saying so.  It matters most on the way to x86-64: natural_t stays 32
 * bits while pointers do not, and this is the interface the userland drivers
 * reach the hardware through.
 */
#include <device/device_master_server.h>

/* ================================================================
 * Interrupt forwarding
 * ================================================================ */

struct irq_forward irq_forward_table[IRQ_FORWARD_MAX];

/*
 * ⚠️ What was on the line before is NOT saved here any more (#457).
 *
 * It used to be, in three arrays of intr_t, int and int -- i386's idea of a
 * handler, of the argument it is called with, and of a priority level.  This
 * file is machine-independent and held all three so that unregister could
 * hand them back; the machine holds them now, because the shape of what was
 * displaced is the machine's and unregister needs nothing but the line.
 *
 * Which also removed the last reason for <chips/busses.h> here.
 */

/*
 * IRQ delivery is split in two halves:
 *
 *   top-half (irq_forward_handler):
 *      Runs in interrupt context, at whatever level the machine gives
 *      device lines.  Just bumps a per-IRQ
 *      pending counter and wakes the bottom-half thread.
 *
 *   bottom-half (irq_forward_thread):
 *      A normal kernel thread.  Drains pending counters and calls
 *      mach_msg_send_from_kernel, which goes through the full IPC
 *      machinery (zone alloc, locks, possibly direct-thread-switch
 *      from #54).  None of that is safe from an IRQ handler — doing
 *      it inline corrupted curr_ipl and panicked intr_ret as soon
 *      as the keyboard fired its first scancode in #206.
 *
 * The userspace driver (e.g. ps2.so) drains the device on its own
 * inside the notification handler, so a single notification per IRQ
 * burst is enough — the counter is only there in case multiple IRQs
 * fire before the bottom-half runs.
 */

static volatile unsigned int	irq_pending[IRQ_FORWARD_MAX];
static int			irq_thread_started;
static int			irq_thread_wake_event;	/* address used as wait channel */

/*
 * #381: mask/unmask flow-control is only safe on lines where a masked
 * event is not lost.  The 8259 latches fronts in its IRR even while
 * masked, so with the legacy PIC masking is always safe.  The I/O APIC
 * does NOT latch a masked edge: a front arriving between the driver's
 * final drain and device_intr_enable's unmask is gone, and a device that
 * keeps INT asserted afterwards (16550 with a full RX FIFO) never fires
 * again — the intermittent, permanent serial-input freeze.  Level lines
 * re-fire on unmask, so for them the #222 storm protection stays.
 */
static boolean_t
irq_forward_mask_safe(int irq)
{
#if	NCPUS > 1
	return device_md_irq_is_level((unsigned int)irq);
#endif	/* NCPUS > 1 */
	return TRUE;
}

static void
irq_forward_handler(int irq)
{
	if (irq < 0 || irq >= IRQ_FORWARD_MAX)
		return;
	if (!irq_forward_table[irq].active ||
	    irq_forward_table[irq].notify_port == IP_NULL)
		return;

	/*
	 * Mask the IRQ at the PIC before EOI so a still-asserted
	 * level-triggered line (#222) cannot re-fire while the message
	 * is in flight.  Userspace driver calls device_intr_enable
	 * after clearing the device-side status register.
	 * #381: skip the mask on I/O APIC edge lines — see
	 * irq_forward_mask_safe().  Edge lines cannot storm: one front
	 * per event, and bursts coalesce in irq_pending[].
	 */
	if (irq_forward_mask_safe(irq))
		device_md_irq_mask(irq);

	irq_pending[irq]++;
	thread_wakeup((event_t)&irq_thread_wake_event);
}

static void
irq_forward_thread(void)
{
	for (;;) {
		int		irq;
		boolean_t	any;

		any = FALSE;
		for (irq = 0; irq < IRQ_FORWARD_MAX; irq++) {
			spl_t s;
			ipc_port_t notify;
			unsigned int pending;

			s = splhigh();
			pending = irq_pending[irq];
			if (pending == 0 || !irq_forward_table[irq].active) {
				splx(s);
				continue;
			}
			irq_pending[irq] = 0;
			notify = irq_forward_table[irq].notify_port;
			splx(s);

			if (notify == IP_NULL)
				continue;
			any = TRUE;

			/*
			 * One message per burst.  ps2.so / future modules
			 * loop on PS2_STAT_OBF themselves, so there's no
			 * value in firing N notifications for N IRQs.
			 */
			{
				mach_msg_header_t msg;

				msg.msgh_bits =
				    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
				msg.msgh_size = sizeof(msg);
				/*
				 * #442: the destination goes as an
				 * argument.  It used to be cast into
				 * msgh_remote_port, which is a NAME
				 * field: the same four bytes on i386,
				 * half a pointer on x86-64.
				 */
				msg.msgh_remote_port = MACH_PORT_NULL;
				msg.msgh_local_port  = MACH_PORT_NULL;
				msg.msgh_id = IRQ_NOTIFY_MSGH_BASE + irq;
				(void)mach_msg_send_from_kernel(notify,
							&msg, sizeof(msg),
							(ipc_port_t *) 0, 0);
			}
		}

		if (!any) {
			spl_t s = splhigh();
			boolean_t empty = TRUE;

			/*
			 * #381: register the wait BEFORE the re-check.
			 * splhigh() only quiesces THIS cpu — a top-half on
			 * another CPU can bump irq_pending[] and fire its
			 * thread_wakeup() between a bare re-check and
			 * assert_wait(); that wakeup finds no waiter and is
			 * lost, and we'd block forever with work pending.
			 * With the wait registered first, a concurrent
			 * wakeup marks it satisfied and thread_block()
			 * returns immediately; if we spot the work
			 * ourselves, cancel the wait and rescan.
			 */
			assert_wait((event_t)&irq_thread_wake_event,
				    FALSE);	/* uninterruptible */
			for (irq = 0; irq < IRQ_FORWARD_MAX; irq++) {
				if (irq_pending[irq] != 0) {
					empty = FALSE;
					break;
				}
			}
			if (empty) {
				splx(s);
				thread_block((void(*)(void))0);
			} else {
				clear_wait(current_thread(),
					   THREAD_AWAKENED, FALSE);
				splx(s);
			}
		}
	}
}

static void
irq_forward_thread_start(void)
{
	if (irq_thread_started)
		return;
	irq_thread_started = 1;
	(void)kernel_thread(kernel_task, irq_forward_thread, NULL);
}

void
device_master_init(void)
{
	int i;

	for (i = 0; i < IRQ_FORWARD_MAX; i++) {
		irq_forward_table[i].notify_port = IP_NULL;
		irq_forward_table[i].active = 0;
	}
}

/* ================================================================
 * MIG server routines (ds_master_ prefix from device_master.defs)
 * ================================================================ */

/*
 * Validate that the caller holds the master device port.
 */
static kern_return_t
check_master_port(ipc_port_t port)
{
	if (port == IP_NULL)
		return KERN_INVALID_ARGUMENT;
	if (port != master_device_port)
		return KERN_INVALID_ARGUMENT;
	return KERN_SUCCESS;
}

/* ---- PCI configuration space ---- */

kern_return_t
ds_master_device_pci_config_read(
	ipc_port_t		master_port,
	unsigned int		bus,
	unsigned int		slot,
	unsigned int		func,
	unsigned int		reg,
	unsigned int		*data)
{
#if NPCI > 0
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bus > 255 || slot > 31 || func > 7 || (reg & 3))
		return KERN_INVALID_ARGUMENT;

	*data = device_md_pci_read(bus, slot, func, reg);
	return KERN_SUCCESS;
#else
	return KERN_FAILURE;
#endif
}

kern_return_t
ds_master_device_pci_config_write(
	ipc_port_t		master_port,
	unsigned int		bus,
	unsigned int		slot,
	unsigned int		func,
	unsigned int		reg,
	unsigned int		data)
{
#if NPCI > 0
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bus > 255 || slot > 31 || func > 7 || (reg & 3))
		return KERN_INVALID_ARGUMENT;

	device_md_pci_write(bus, slot, func, reg, data);
	return KERN_SUCCESS;
#else
	return KERN_FAILURE;
#endif
}

/* ---- Interrupt forwarding ---- */

kern_return_t
ds_master_device_intr_register(
	ipc_port_t		master_port,
	unsigned int		irq,
	ipc_port_t		notify_port)
{
	kern_return_t kr;
	spl_t s;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * ⚠️ LINES and not MAX.  The table is thirty-two slots as of #457, and
	 * the upper half is message-signalled interrupts -- which have no line
	 * number, are never asked for by one, and are allocated by
	 * ds_master_device_msi_register() rather than named here.  Bounding
	 * this by the table's size would let a driver claim a slot the kernel
	 * hands out, and the machine would then refuse it for a different
	 * reason and with a different code.
	 */
	if (irq >= IRQ_FORWARD_LINES)
		return KERN_INVALID_ARGUMENT;

	if (notify_port == IP_NULL)
		return KERN_INVALID_ARGUMENT;

	if (irq_forward_table[irq].active)
		return KERN_RESOURCE_SHORTAGE;	/* already registered */

	/* Spawn the bottom-half kthread on first registration. */
	irq_forward_thread_start();

	s = splhigh();

	/*
	 * The table entry before the claim, because the claim is what makes
	 * the handler reachable: on a machine that routes the line to another
	 * processor, the first interrupt can arrive before this one returns,
	 * and a top half that found active == 0 would drop it.
	 */
	irq_forward_table[irq].notify_port = notify_port;
	irq_forward_table[irq].active = 1;

	if (!device_md_irq_register(irq, irq_forward_handler)) {
		/*
		 * The machine has no vector or no controller for this line.
		 * Put the entry back rather than leaving a registration that
		 * nothing can ever deliver to -- and do not call unregister,
		 * which has nothing to undo.
		 */
		irq_forward_table[irq].notify_port = IP_NULL;
		irq_forward_table[irq].active = 0;
		splx(s);
		return KERN_FAILURE;
	}

	splx(s);

	return KERN_SUCCESS;
}

/*
 * A device's message-signalled interrupt (#457).
 *
 * 🔑 The same table, the same thread, the same notification.  What differs is
 * only that this side did not choose the slot: the caller has no line number
 * to give, because there is no line -- so the machine allocates one and hands
 * it back, and everything after this point treats it as any other slot.
 *
 * ⚠️ Which is why the entry is filled in BEFORE the machine is asked.  The
 * device is armed by device_md_msi_register(), and on a machine with more than
 * one processor the first interrupt can arrive before that call returns; a top
 * half that found active == 0 would drop it.  But the slot is not known until
 * afterwards -- so the entry is written against the slot the machine answers
 * with, and the window is closed by the forwarding table being checked under
 * splhigh() rather than by an ordering this file could arrange.
 */
kern_return_t
ds_master_device_msi_register(
	ipc_port_t		master_port,
	unsigned int		bus,
	unsigned int		dev,
	unsigned int		func,
	unsigned int		entry,
	ipc_port_t		notify_port,
	unsigned int		*slot_out)
{
	kern_return_t	kr;
	spl_t		s;
	unsigned int	slot = 0;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (notify_port == IP_NULL || slot_out == 0)
		return KERN_INVALID_ARGUMENT;

	irq_forward_thread_start();

	s = splhigh();

	if (!device_md_msi_register(bus, dev, func, entry,
				    irq_forward_handler, &slot)) {
		splx(s);
		return KERN_FAILURE;
	}

	/*
	 * ⚠️ Bounded here as well as in the machine.  The slot is a number this
	 * file indexes a table with, and "the machine would not answer a bad
	 * one" is a claim about another file -- which is the arrangement that
	 * stops holding the day a third machine answers.
	 */
	if (slot >= IRQ_FORWARD_MAX) {
		device_md_msi_unregister(slot);
		splx(s);
		return KERN_FAILURE;
	}

	irq_forward_table[slot].notify_port = notify_port;
	irq_forward_table[slot].active = 1;
	irq_forward_table[slot].msi = 1;

	splx(s);

	*slot_out = slot;
	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_intr_unregister(
	ipc_port_t		master_port,
	unsigned int		irq)
{
	kern_return_t kr;
	spl_t s;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (irq >= IRQ_FORWARD_MAX)
		return KERN_INVALID_ARGUMENT;

	if (!irq_forward_table[irq].active)
		return KERN_INVALID_ARGUMENT;

	s = splhigh();

	/*
	 * The line first, the entry second -- the mirror of register, and for
	 * the mirror reason: until the line is given up an interrupt can still
	 * arrive, and it must find an entry that still says where to send it.
	 */
	/*
	 * 🔴 The right one of the two (#520).  A message-signalled slot has to
	 * be disarmed in the DEVICE; a line is masked at the pin.  This used to
	 * call the line's release for both, and the line's release returns
	 * without acting on anything above the sixteen lines -- so every MSI
	 * slot ever given back was left armed at a vector whose handler had
	 * just been released.
	 */
	if (irq_forward_table[irq].msi)
		device_md_msi_unregister(irq);
	else
		device_md_irq_unregister(irq);

	irq_forward_table[irq].notify_port = IP_NULL;
	irq_forward_table[irq].active = 0;
	irq_forward_table[irq].msi = 0;

	/*
	 * 🔴 AND THE COUNT, or the bottom half spins for ever (#457).
	 *
	 * A front can arrive between the top half's increment and this
	 * unregister, leaving irq_pending[irq] non-zero on a line that is no
	 * longer active.  irq_forward_thread() then cannot make progress and
	 * cannot stop: its drain loop skips the entry -- `pending == 0 ||
	 * !active' -- without clearing it, so nothing is sent and `any' stays
	 * FALSE; and the block below that loop refuses to sleep precisely
	 * because some irq_pending[] is non-zero.  It cancels its own wait and
	 * rescans, at full speed, with nothing left that could ever clear the
	 * count.
	 *
	 * 🔑 Cleared HERE rather than tolerated in the loop, because a line
	 * nobody is registered for has no pending notifications by definition:
	 * this is where the state stops being true, so this is where it is not
	 * allowed to become false.  Teaching the drain loop to skip it more
	 * gracefully would have left the stranded count in existence and made
	 * the spin depend on the shape of a `continue'.
	 */
	irq_pending[irq] = 0;

	splx(s);

	return KERN_SUCCESS;
}

/*
 * Re-enable the IRQ at the PIC after the userspace driver has processed
 * a forwarded notification (#222).  Drivers call this uniformly
 * regardless of trigger mode; #381 makes it the exact mirror of the
 * top-half — only lines the top-half actually masked (level, or legacy
 * 8259) are unmasked, so it cannot spuriously re-enable a line someone
 * else masked on purpose.
 */
kern_return_t
ds_master_device_intr_enable(
	ipc_port_t		master_port,
	unsigned int		irq)
{
	kern_return_t kr;
	spl_t s;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (irq >= IRQ_FORWARD_MAX)
		return KERN_INVALID_ARGUMENT;

	s = splhigh();
	if (irq_forward_mask_safe((int)irq))
		device_md_irq_unmask(irq);
	splx(s);

	return KERN_SUCCESS;
}

/* ---- DMA buffer allocation ---- */


/*
 * ── What a DMA buffer's `bdf' says (#432) ────────────────────────────
 *
 * Which device is going to read the buffer.  That is the fact the kernel
 * needs before it can put the pages in that device's IOMMU domain and hand
 * back an address that means nothing outside it -- and until #457 closed,
 * nothing anywhere carried it: device_dma_alloc took a size and gave back a
 * physical address, so the isolation the driver model claims was enforced by
 * the driver being well behaved.
 *
 * 🔴 AND IT IS NOW ACTED ON, ON A MACHINE THAT CAN (#432 stage 3d).  Every
 * page allocated for a named device is put in that device's IOMMU domain, and
 * the first such page is what takes the device off pass-through -- after which
 * it reaches what it has been granted and nothing else in the machine.
 *
 * 🔑 THE ADDRESS RETURNED DOES NOT CHANGE.  A grant maps the buffer at its own
 * physical address, so every driver goes on programming what it always did --
 * what changed is that every OTHER address now faults for that device.  The
 * step that makes the returned number meaningless outside the domain is a
 * separate one, because it is a protocol change and this is not.
 *
 * ⚠️ A machine with no remapping hardware, or booted without -I, answers no to
 * device_md_dma_isolates() and nothing below happens.  That is i386 always,
 * and x86-64 by default: the isolation is opt-in until #432 closes, so a
 * kernel that cannot survive it can still be booted to find out why.
 *
 * 🔴 DEVICE_DMA_NO_BDF IS A HOLE, AND IT IS NOW A VISIBLE ONE.  ext_server
 * allocates pages that the BLOCK SERVER's disk will read, and names no device
 * because it owns none -- so those pages are granted to nobody, and the first
 * time a disk in a real domain is told to read into one, the transfer is
 * REFUSED and the fault names the address.  That is the correct behaviour and
 * an honest report of a protocol that is wrong: the server that owns the
 * device has to be the one that maps.  Before this the same mistake was made
 * silently and worked.
 */
kern_return_t
ds_master_device_dma_alloc(
	ipc_port_t		master_port,
	natural_t		bdf,
	vm_size_t		size,
	vm_address_t		*vaddr_out,
	vm_address_t		*paddr_out)
{
	kern_return_t kr;
	vm_offset_t kva;
	vm_offset_t pa;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (size == 0)
		return KERN_INVALID_ARGUMENT;

	size = round_page(size);

	/*
	 * Allocate wired, physically contiguous memory.
	 * kmem_alloc_contig guarantees contiguity for DMA.
	 */
	kr = kmem_alloc_contig(kernel_map, &kva, size,
			       PAGE_MASK, 0);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * Extract the physical address.
	 */
	pa = pmap_extract(pmap_kernel(), kva);
	if (pa == 0) {
		kmem_free(kernel_map, kva, size);
		return KERN_FAILURE;
	}

	/*
	 * ⚠️ The grant is a hard failure and not a warning.  A buffer the
	 * device cannot reach is not a slower buffer -- it is one every
	 * transfer into it faults on, at an address the driver believes it
	 * owns, and the driver's own error path has no way to tell that from
	 * the disk being broken.  Failing the allocation says it once, here.
	 */
	if (bdf != DEVICE_DMA_NO_BDF && device_md_dma_isolates()
	    && !device_md_dma_grant(bdf, (unsigned long)pa,
				    (unsigned long)size, TRUE, TRUE)) {
		kmem_free(kernel_map, kva, size);
		return KERN_FAILURE;
	}

	*vaddr_out = kva;		/* #427: no narrowing cast */
	*paddr_out = pa;
	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_dma_free(
	ipc_port_t		master_port,
	natural_t		bdf,
	vm_address_t		vaddr,
	vm_size_t		size)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (size == 0)
		return KERN_INVALID_ARGUMENT;

	size = round_page(size);

	/*
	 * 🔴 REVOKED BEFORE THE MEMORY IS GIVEN BACK, and the order is the
	 * whole of it.  Freed first, the page returns to the VM and can be
	 * handed to anything -- a task's stack, another driver's buffer --
	 * while the device is still able to write it, and for as long as
	 * nobody happens to revoke.  That is precisely the reach this issue
	 * exists to remove, arrived at by tidying up in the wrong order.
	 *
	 * ⚠️ The physical address is extracted while the mapping still exists,
	 * for the same reason.  After kmem_free there is nothing to ask.
	 */
	if (bdf != DEVICE_DMA_NO_BDF && device_md_dma_isolates()) {
		vm_offset_t pa = pmap_extract(pmap_kernel(),
					      (vm_offset_t)vaddr);

		if (pa != 0)
			(void) device_md_dma_revoke(bdf, (unsigned long)pa,
						    (unsigned long)size);
	}

	kmem_free(kernel_map, (vm_offset_t)vaddr, size);
	return KERN_SUCCESS;
}

/* ---- Scatter-gather DMA allocation ---- */

/*
 * device_dma_alloc_sg — allocate n_pages individually wired pages,
 * map them contiguously into a user task, and return per-page PAs.
 *
 * Unlike device_dma_alloc (kmem_alloc_contig), pages need NOT be
 * physically contiguous.  kmem_alloc_wired gives contiguous kernel VA
 * with individually allocated physical pages — perfect for building
 * multi-entry PRDTs (scatter-gather DMA).
 */
/*
 * ── The address list travels out of line (#520) ──────────────────────
 *
 * `paddrs' is a vm_map_copy_t handed to MIG, not a caller-supplied buffer, and
 * every element is a full vm_address_t.  See the note on dma_sg_addr_t in
 * <device/device_master.defs> for why the shape changed with the width.
 *
 * ⚠️ The ceiling on n_pages is gone with the inline array that imposed it.
 * What bounds this now is the allocation itself: n_pages pages of wired kernel
 * memory, which fails when there are not that many.  A number that was a
 * message-layout limit pretending to be a policy is worse than no number.
 */
kern_return_t
ds_master_device_dma_alloc_sg(
	ipc_port_t		master_port,
	natural_t		bdf,
	unsigned int		n_pages,
	ipc_port_t		task_port,
	vm_address_t		*kva_out,
	vm_address_t		*uva_out,
	vm_address_t		**paddrs,
	mach_msg_type_number_t	*paddrs_count)
{
	kern_return_t	kr;
	task_t		task;
	vm_offset_t	kva;
	vm_size_t	size;
	vm_map_t	map;
	vm_offset_t	uva;
	unsigned int	i;
	vm_offset_t	list;
	vm_size_t	list_bytes, list_size;
	vm_map_copy_t	list_copy;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (n_pages == 0)
		return KERN_INVALID_ARGUMENT;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	size = (vm_size_t)n_pages * PAGE_SIZE;

	/*
	 * The list first, because it is the cheapest thing to fail on and
	 * failing after the mapping would mean unmapping it again.
	 */
	list_bytes = (vm_size_t)n_pages * sizeof(vm_address_t);
	list_size = round_page(list_bytes);

	kr = vm_allocate(ipc_kernel_map, &list, list_size, TRUE);
	if (kr != KERN_SUCCESS) {
		task_deallocate(task);
		return kr;
	}

	/*
	 * Wired while it is filled, and unwired before it is copied out --
	 * the shape mach_port_names() uses, and for its reason: vm_map_copyin
	 * on a wired range is not the same operation.
	 */
	kr = vm_map_wire(ipc_kernel_map, list, list + list_size,
			 VM_PROT_READ | VM_PROT_WRITE, FALSE);
	if (kr != KERN_SUCCESS) {
		kmem_free(ipc_kernel_map, list, list_size);
		task_deallocate(task);
		return kr;
	}

	/*
	 * Allocate wired kernel pages.  kmem_alloc_wired gives us
	 * contiguous kernel VA but physical pages are allocated
	 * individually — no contiguity requirement.
	 */
	kr = kmem_alloc_wired(kernel_map, &kva, size);
	if (kr != KERN_SUCCESS) {
		(void) vm_map_unwire(ipc_kernel_map, list, list + list_size,
				     FALSE);
		kmem_free(ipc_kernel_map, list, list_size);
		task_deallocate(task);
		return kr;
	}

	/*
	 * Map pages contiguously into the user task.
	 * Reserve a VA range, then wire in each physical page.
	 */
	map = task->map;
	uva = 0;
	kr = vm_map_enter(map, &uva, size, 0, TRUE,
			  VM_OBJECT_NULL, (vm_offset_t)0, FALSE,
			  VM_PROT_READ | VM_PROT_WRITE,
			  VM_PROT_READ | VM_PROT_WRITE,
			  VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS) {
		(void) vm_map_unwire(ipc_kernel_map, list, list + list_size,
				     FALSE);
		kmem_free(ipc_kernel_map, list, list_size);
		kmem_free(kernel_map, kva, size);
		task_deallocate(task);
		return kr;
	}

	for (i = 0; i < n_pages; i++) {
		vm_offset_t pa = pmap_extract(pmap_kernel(),
					      kva + i * PAGE_SIZE);
		/*
		 * #407: the answer was being dropped three lines below this
		 * routine's own error handling.  A mapping that does not
		 * happen and is not reported hands the task an address it
		 * will fault on, with KERN_SUCCESS to say all is well.
		 */
		if (pmap_enter(map->pmap, uva + i * PAGE_SIZE, pa,
			       VM_PROT_READ | VM_PROT_WRITE, TRUE) != 0) {
			/*
			 * vm_map_enter above already took the range, and the
			 * pages entered before this one are in it.  Releasing
			 * the range drops both; leaving it would trade a
			 * silent bad mapping for a silent leak.
			 */
			(void) vm_map_remove(map, uva, uva + size, VM_MAP_NO_FLAGS);
			(void) vm_map_unwire(ipc_kernel_map, list,
					     list + list_size, FALSE);
			kmem_free(ipc_kernel_map, list, list_size);
			kmem_free(kernel_map, kva, size);
			task_deallocate(task);
			return KERN_RESOURCE_SHORTAGE;
		}

		/*
		 * #520: whole, where this used to write `(unsigned int)pa'.
		 * pmap_extract answers a vm_offset_t and the list now holds
		 * one, so a page above four gigabytes is named rather than
		 * silently folded into the low half of memory.
		 */
		/*
		 * 🔴 EVERY PAGE, ONE AT A TIME, because that is what a
		 * scatter-gather buffer IS.  These pages are contiguous in the
		 * kernel's address space and in the task's, and not in
		 * physical memory -- granting the range from the first address
		 * would map whatever lies between them, which is the ordinary
		 * case here and could be anything.
		 *
		 * ⚠️ The failure path is the one above, whole: the range is
		 * removed and the pages freed.  A partially granted
		 * scatter-gather list is the worst of both -- the driver gets
		 * an address list it believes, and the transfer faults on
		 * whichever entry was not reached.
		 */
		if (bdf != DEVICE_DMA_NO_BDF && device_md_dma_isolates()
		    && !device_md_dma_grant(bdf, (unsigned long)pa,
					    (unsigned long)PAGE_SIZE,
					    TRUE, TRUE)) {
			(void) vm_map_remove(map, uva, uva + size,
					     VM_MAP_NO_FLAGS);
			(void) vm_map_unwire(ipc_kernel_map, list,
					     list + list_size, FALSE);
			kmem_free(ipc_kernel_map, list, list_size);
			kmem_free(kernel_map, kva, size);
			task_deallocate(task);
			return KERN_RESOURCE_SHORTAGE;
		}

		((vm_address_t *)list)[i] = pa;
	}

	task_deallocate(task);

	kr = vm_map_unwire(ipc_kernel_map, list, list + list_size, FALSE);
	if (kr != KERN_SUCCESS) {
		kmem_free(ipc_kernel_map, list, list_size);
		(void) vm_map_remove(map, uva, uva + size, VM_MAP_NO_FLAGS);
		kmem_free(kernel_map, kva, size);
		return kr;
	}

	/*
	 * ⚠️ list_bytes and not list_size: the caller is handed exactly the
	 * addresses it asked for, not the page the allocator rounded up to.
	 * Copying the rounding out would hand a driver whatever the tail of
	 * that page happened to contain, with a count that says it is data.
	 */
	kr = vm_map_copyin(ipc_kernel_map, list, list_bytes, TRUE, &list_copy);
	if (kr != KERN_SUCCESS) {
		kmem_free(ipc_kernel_map, list, list_size);
		(void) vm_map_remove(map, uva, uva + size, VM_MAP_NO_FLAGS);
		kmem_free(kernel_map, kva, size);
		return kr;
	}

	*kva_out = kva;			/* #427: no narrowing cast */
	*uva_out = uva;
	*paddrs = (vm_address_t *) list_copy;
	*paddrs_count = n_pages;
	return KERN_SUCCESS;
}

/* ---- User-space DMA and MMIO mapping ---- */

/*
 * Helper: reserve a VA range in a task's map and wire in physical pages.
 */
static kern_return_t
map_phys_into_task(task_t		task,
		   vm_offset_t		phys_base,	/* page-aligned */
		   vm_size_t		size,		/* page-aligned */
		   vm_offset_t		*uva_out)
{
	vm_map_t	map = task->map;
	vm_offset_t	uva = 0;
	kern_return_t	kr;
	vm_offset_t	i;

	kr = vm_map_enter(map, &uva, size, 0, TRUE,
			  VM_OBJECT_NULL, (vm_offset_t)0, FALSE,
			  VM_PROT_READ | VM_PROT_WRITE,
			  VM_PROT_READ | VM_PROT_WRITE,
			  VM_INHERIT_NONE);
	if (kr != KERN_SUCCESS)
		return kr;

	/* #407: same as above -- report rather than hand back a hole, and give
	 * back the range vm_map_enter just took rather than abandon it. */
	for (i = 0; i < size; i += PAGE_SIZE)
		if (pmap_enter(map->pmap, uva + i, phys_base + i,
			       VM_PROT_READ | VM_PROT_WRITE, TRUE) != 0) {
			(void) vm_map_remove(map, uva, uva + size, VM_MAP_NO_FLAGS);
			return KERN_RESOURCE_SHORTAGE;
		}

	*uva_out = uva;
	return KERN_SUCCESS;
}

/*
 * device_dma_map_user — map a kernel DMA buffer into a user task.
 * kva: kernel VA returned by device_dma_alloc (physically contiguous).
 * Returns user VA accessible by the driver process.
 */
kern_return_t
ds_master_device_dma_map_user(
	ipc_port_t		master_port,
	vm_address_t		kva,
	vm_size_t		size,
	ipc_port_t		task_port,
	vm_address_t		*uva_out)
{
	task_t		task;
	vm_offset_t	uva;
	vm_offset_t	pa;
	kern_return_t	kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (kva == 0 || size == 0)
		return KERN_INVALID_ARGUMENT;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	pa = pmap_extract(pmap_kernel(), (vm_offset_t)kva);
	if (pa == 0) {
		task_deallocate(task);
		return KERN_FAILURE;
	}

	kr = map_phys_into_task(task, pa, round_page(size), &uva);
	task_deallocate(task);
	if (kr != KERN_SUCCESS)
		return kr;

	*uva_out = uva;			/* #427: no narrowing cast */
	return KERN_SUCCESS;
}

/*
 * device_mmio_map — map a physical MMIO range (PCI BAR) into a user task.
 * phys_addr need not be page-aligned; the returned uva has the same
 * page offset as phys_addr.
 */
kern_return_t
ds_master_device_mmio_map(
	ipc_port_t		master_port,
	vm_address_t		phys_addr,
	vm_size_t		size,
	ipc_port_t		task_port,
	vm_address_t		*uva_out)
{
	task_t		task;
	vm_offset_t	phys_base;
	vm_offset_t	page_offset;
	vm_offset_t	uva;
	vm_size_t	round_sz;
	kern_return_t	kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (phys_addr == 0 || size == 0)
		return KERN_INVALID_ARGUMENT;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	phys_base   = trunc_page((vm_offset_t)phys_addr);
	page_offset = (vm_offset_t)phys_addr - phys_base;
	round_sz    = round_page(page_offset + size);

	kr = map_phys_into_task(task, phys_base, round_sz, &uva);
	task_deallocate(task);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * #427: no cast.  This used to be `(unsigned int)(uva + page_offset)'
	 * -- an EXPLICIT narrowing of a 64-bit vm_offset_t, so the compiler
	 * said nothing, and the address a task was told to use was the low
	 * half of the one it had been given.  It would have worked anyway for
	 * a long time: vm_map_enter() searches from the map's min_offset, so
	 * the first hole in a sparse task is low.  Which is the worst shape a
	 * defect can have -- right until the low four gigabytes are busy.
	 */
	*uva_out = uva + page_offset;
	return KERN_SUCCESS;
}

/*
 * device_mmio_unmap — remove an MMIO mapping from a user task.
 */
kern_return_t
ds_master_device_mmio_unmap(
	ipc_port_t		master_port,
	vm_address_t		uva,
	vm_size_t		size,
	ipc_port_t		task_port)
{
	task_t		task;
	kern_return_t	kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	task = convert_port_to_task(task_port);
	if (task == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	kr = vm_map_remove(task->map,
			   trunc_page((vm_offset_t)uva),
			   round_page((vm_offset_t)uva + size),
			   FALSE);
	task_deallocate(task);
	return kr;
}

/* ---- I/O port access ---- */

kern_return_t
ds_master_device_io_port_read(
	ipc_port_t		master_port,
	unsigned int		port,
	unsigned int		size,
	unsigned int		*data_out)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (size != 1 && size != 2 && size != 4)
		return KERN_INVALID_ARGUMENT;

	*data_out = device_md_io_read(port, size);
	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_io_port_write(
	ipc_port_t		master_port,
	unsigned int		port,
	unsigned int		size,
	unsigned int		data)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (size != 1 && size != 2 && size != 4)
		return KERN_INVALID_ARGUMENT;

	device_md_io_write(port, size, data);
	return KERN_SUCCESS;
}

/*
 * #382: console-break entry into DDB from a userspace console driver.
 *
 * The serial (uart.so) and keyboard (ps2.so) drivers own their devices'
 * RX paths, so the kernel-side break checks (com.c check_debugger,
 * ddb_kbd) never see the break key once char_server is up.  The driver
 * calls this when it spots Ctrl+D; we enter DDB right here, in the
 * driver's RPC context, and return when the operator continues — the
 * calling server thread simply blocks for the debug session.
 *
 * Gated by the debugger's own boot flag: when not armed we return
 * KERN_FAILURE and the driver delivers the byte as ordinary input instead.
 * Which flag that is belongs to the machine -- `-K' on i386, `-r' on x86-64,
 * two debuggers with two ways of being asked for.
 *
 * ⚠️ #457: the arming flag, the pre-park and the entry are ONE machine-
 * dependent operation, not a predicate this file acts on.  What has to happen
 * around the entry differs between the machines -- see
 * <device/device_machdep.h> -- and it used to be here as three externs
 * declared beside the code, which is why enumerating this file's includes did
 * not find it and the link did.
 */
kern_return_t
ds_master_device_ddb_break(
	ipc_port_t		master_port)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (!device_md_debugger_break())
		return KERN_FAILURE;

	printf("ddb: console break (Ctrl+D) session ended, resuming\n");
	return KERN_SUCCESS;
}

/*
 * ── Was it the IOMMU? (#432 stage 3d) ────────────────────────────────
 *
 * 🔑 A DRIVER'S FAILED TRANSFER HAS TWO ORDINARY EXPLANATIONS and the device
 * reports neither.  Either the hardware stopped answering, or the driver
 * programmed an address it was never granted -- and once #432 is doing its
 * job, the second one produces exactly the symptoms of the first: no
 * completion, no error bit, a command that simply never finishes.  The
 * difference is written down in the engine's fault records, inside the kernel,
 * where no driver can see it.
 *
 * 🔴 AND `confined' IS A SEPARATE FACT, not one that can be read off the
 * count.  Zero refusals is the answer on a machine with no remapping hardware
 * AND on a device that is behaving, and a self-test written to demonstrate the
 * isolation must tell those apart -- otherwise "the DMA was allowed" reads as
 * a pass on exactly the machine where it is the failure.
 */
kern_return_t
ds_master_device_dma_faults(
	ipc_port_t		master_port,
	natural_t		bdf,
	natural_t		*confined,
	natural_t		*count,
	vm_address_t		*address)
{
	kern_return_t kr;
	unsigned long last = 0;
	unsigned n;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	n = device_md_dma_faults(bdf, &last);

	*confined = (natural_t)device_md_dma_confined(bdf);
	*count = (natural_t)n;
	*address = (vm_address_t)last;
	return KERN_SUCCESS;
}
