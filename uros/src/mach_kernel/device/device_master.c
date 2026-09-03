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
#include <kern/cap.h>		/* #432: a capability for a device's kind */
#include <kern/ipc_mig.h>
/*
 * ⚠️ Declared here rather than found in a header: port_name_to_task is defined
 * in kern/ipc_mig.c and named in no header this file can reach.  Written out
 * so the day one appears, this line is the one that stops compiling instead of
 * the one that silently shadows it.
 */
extern task_t port_name_to_task(mach_port_t name);

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

/*
 * ── Which task drives which device (#432) ────────────────────────────
 *
 * 🔴 HOLDING THE MASTER PORT USED TO BE THE WHOLE ANSWER, and that is the gap
 * #432 names: "a device capability carries the right to map memory for that
 * device... rather than inventing an ad-hoc grant mechanism".  What existed
 * was the ad-hoc mechanism -- any holder of the master port could name any
 * bus/device/function and have memory mapped into ITS domain.  Which matters
 * precisely because of what the rest of this issue built: a task that can
 * grant for someone else's device can put a page it owns inside that device's
 * domain, and then it is the DEVICE that reaches the page, which is the one
 * kind of reach nothing else here checks.
 *
 * 🔑 This does not answer the capability question -- issuing a per-device
 * token is cap_server's to do and needs a policy about WHO is entitled to a
 * device, which is a decision and not an implementation.  What it does is
 * remove the part that needs no policy at all: a device has ONE driver.  The
 * first task to grant for a bus/device/function claims it, and a second task
 * naming it is refused.
 *
 * 🔴 THE `task' FIELD IS COMPARED AND NEVER DEREFERENCED, and that is what
 * decides how a dead driver has to be handled.  This comment used to say the
 * claim was deliberately never released, and that leaving a dead driver's
 * device claimed was "the safe direction".  It was the unsafe one (#513):
 *
 *	the pointer is stored WITHOUT a reference;
 *	nothing ever dereferences it -- `claim.task == me' is the only use;
 *	tasks come from a zone, so zfree()/zalloc() RECYCLE the address.
 *
 * ⇒ A later, unrelated task allocated into the dead driver's block compares
 * equal and INHERITS the claim -- the IOMMU domain included.  🔑 It is not a
 * use-after-free, and that is why it survived: nothing is dereferenced, so
 * there is no invalid access for a checker or a fault to catch.  A silent
 * false match reads exactly like a correct one.
 *
 * device_master_task_terminating() is the answer, and it is why the field can
 * stay reference-free: the entry does not outlive the task.
 */
#define	DEVICE_MAX_CLAIMS	16

static struct {
	natural_t	bdf;
	task_t		task;
	uint64_t	cap_id;		/* the token that established it */
} device_claim[DEVICE_MAX_CLAIMS];

static unsigned device_nclaims;

/*
 * Whether another task has claimed this device.  Asks and does not claim.
 */
static int
device_claimed_by_other(natural_t bdf)
{
	task_t me = current_task();
	unsigned i;

	for (i = 0; i < device_nclaims; i++)
		if (device_claim[i].bdf == bdf)
			return device_claim[i].task != me;

	return 0;
}

/*
 * May this task act on this device?
 *
 * 🔴 IT NO LONGER CLAIMS.  A device is claimed by device_claim(), which is
 * where a capability is presented; this only answers whether the claim already
 * belongs to the caller.  The two used to be one function, and the difference
 * is the whole of the policy: first-come-first-served is not an entitlement,
 * it is the absence of one.
 */
static kern_return_t
check_claim(natural_t bdf)
{
	task_t me = current_task();
	unsigned i;

	if (bdf == DEVICE_DMA_NO_BDF)
		return KERN_SUCCESS;

	for (i = 0; i < device_nclaims; i++)
		if (device_claim[i].bdf == bdf)
			return device_claim[i].task == me
			       ? KERN_SUCCESS : KERN_NO_ACCESS;

	/*
	 * ⚠️ An UNCLAIMED device is refused too, and that is not the same
	 * answer wearing the same code by accident: a driver that has not
	 * presented a capability for this device's kind has exactly as much
	 * right to map memory for it as one that presented somebody else's.
	 */
	return KERN_NO_ACCESS;
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
 * ── What the kernel handed out for DMA, and to whom (#432) ───────────
 *
 * 🔴 THE HOLE THIS CLOSES IS THE ONE #432 CALLED `DEVICE_DMA_NO_BDF'.
 * ext_server allocates scatter-gather pages that the BLOCK SERVER's disk will
 * read, and names no device because it owns none.  Once that disk is confined
 * the pages are in nobody's domain, so the first transfer into them is
 * refused -- correct, diagnosable, and useless: the filesystem stops working.
 *
 * The shape this issue asks for is that the server which OWNS the device does
 * the mapping.  It cannot do that with raw physical addresses unless the
 * kernel can tell a page it handed out for DMA from any other page in the
 * machine -- otherwise "map this address for my device" is a request to put
 * ARBITRARY physical memory inside a device's reach, which is the whole thing
 * being prevented.  So the kernel remembers what it allocated.
 *
 * 🔑 A REGION AND NOT A PAGE.  A four-megabyte page cache is one region of a
 * thousand frames, mapped for a device ONCE and given a thousand consecutive
 * addresses -- so the record is one entry, the invalidation is one flush, and
 * the driver's per-page question after the first is arithmetic.  A per-page
 * record would have been a thousand entries per buffer in a table sized for
 * sixteen.
 *
 * ⚠️ It narrows and does not close.  Any holder of the master port can map any
 * page the kernel allocated for DMA -- not any page in the machine, which is
 * where this started, but not only the ones somebody handed it either.  The
 * difference between those two is a capability naming the BUFFER, and that is
 * the same design cap_server owes this issue for devices.
 */
/*
 * The biggest contiguous buffer this will record, in pages.  ⚠️ A CEILING and
 * not a guess: the page list is built on the stack here, and a contiguous
 * allocation big enough to overflow it is refused rather than left
 * unrecorded.  Scatter-gather has no such limit -- its list is already in
 * memory the kernel allocated for it.
 */
#define	DEVICE_CONTIG_MAX_PAGES	64

#define	DEVICE_MAX_DMA_REGIONS	16
#define	DEVICE_MAX_REGION_USERS	4

struct dma_region {
	vm_offset_t	kva;		/* zero when the slot is free    */
	vm_size_t	size;
	unsigned int	npages;
	vm_offset_t	*pa;		/* npages entries, kalloc'd      */

	/*
	 * ── What makes this buffer nameable, and whose it is (#432) ──
	 *
	 * 🔴 A REGION NEEDS AN IDENTITY BEFORE ANYONE CAN BE GIVEN A
	 * CAPABILITY FOR IT.  device_dma_map_foreign takes a physical address
	 * and checks only that the kernel allocated it for DMA -- so any
	 * holder of the master port can put anybody's DMA buffer inside its
	 * own device's reach.  That is narrower than "all of physical memory",
	 * which is where this started, and it is not "only what somebody
	 * handed me".  Closing the difference means the buffer has a NAME a
	 * capability can carry, and an OWNER whose consent that capability
	 * represents.
	 *
	 * 🔑 The id never repeats, and that is the whole of its safety: a
	 * region freed and its slot reused would otherwise inherit the
	 * capabilities issued against the buffer that used to be there.  Sixty
	 * -four bits at one allocation per microsecond is six hundred thousand
	 * years, so the counter is not a thing that needs a policy.
	 */
	uint64_t	id;
	task_t		owner;		/* who allocated it; holds a ref */

	/*
	 * 🔥 WHERE ELSE THIS MEMORY IS MAPPED, AND A PANIC A USER TASK COULD
	 * REACH.  device_dma_alloc_sg enters its pages into the caller's map
	 * with pmap_enter(..., wired), and device_dma_free frees the KERNEL
	 * side -- so freeing an sg buffer while the task still holds it hands
	 * the pages back to the VM with a wired mapping outstanding, and the
	 * next teardown finds it:
	 *
	 *	panic(cpu 0): pmap_remove_all removing a wired page
	 *
	 * ⚠️ Nothing hit it because nothing ever freed a scatter-gather
	 * buffer: the drivers allocate theirs once and keep them.  It took a
	 * test that did, and it panicked i386 from a userland RPC.
	 *
	 * So the region remembers the task it was mapped into, holding a
	 * reference, and the free takes that mapping down first.
	 */
	task_t		task;		/* null for a contiguous buffer  */
	vm_offset_t	uva;

	/* Which devices have been given this region, and where. */
	unsigned int	nusers;
	struct {
		natural_t	bdf;
		vm_offset_t	dma;	/* address of page zero          */
	} user[DEVICE_MAX_REGION_USERS];
};

static struct dma_region dma_region[DEVICE_MAX_DMA_REGIONS];

/*
 * ⚠️ Starts at one.  Zero is the answer dma_region_owner() gives for a region
 * that does not exist, and an id that could also be zero would make "no such
 * buffer" and "the first buffer" the same reply.
 */
static uint64_t dma_region_next_id = 1;

/*
 * Remember one allocation.  Answers zero when there is no room, and the caller
 * must then fail the allocation: a region that is not recorded is one no
 * device can ever be given, and one whose pages nothing will revoke.
 */
static int
dma_region_add(vm_offset_t kva, vm_size_t size, const vm_offset_t *pa,
	       unsigned int npages, task_t task, vm_offset_t uva,
	       uint64_t *id_out)
{
	struct dma_region *r = 0;
	unsigned int i;

	for (i = 0; i < DEVICE_MAX_DMA_REGIONS; i++)
		if (dma_region[i].kva == 0) {
			r = &dma_region[i];
			break;
		}

	if (r == 0)
		return 0;

	r->pa = (vm_offset_t *) kalloc(npages * sizeof(vm_offset_t));
	if (r->pa == 0)
		return 0;

	for (i = 0; i < npages; i++)
		r->pa[i] = pa[i];

	r->kva = kva;
	r->size = size;
	r->npages = npages;
	r->nusers = 0;
	r->task = task;
	r->uva = uva;
	r->id = dma_region_next_id++;

	/*
	 * ⚠️ The OWNER and the mapped-into task are recorded separately even
	 * though they are the same today.  One is "who may say who else may
	 * read this" and the other is "whose address space has to be taken
	 * down before the pages go back": a contiguous buffer has the second
	 * and not the first, and conflating them would make a free path decide
	 * a question about authority.
	 */
	r->owner = current_task();
	task_reference(r->owner);

	if (task != TASK_NULL)
		task_reference(task);

	if (id_out != 0)
		*id_out = r->id;

	return 1;
}

/* The region holding this physical page, and which page of it, or null. */
static struct dma_region *
dma_region_of(vm_offset_t pa, unsigned int *index)
{
	unsigned int i, p;

	for (i = 0; i < DEVICE_MAX_DMA_REGIONS; i++) {
		if (dma_region[i].kva == 0)
			continue;

		for (p = 0; p < dma_region[i].npages; p++)
			if (dma_region[i].pa[p] == (pa & ~(vm_offset_t)PAGE_MASK)) {
				*index = p;
				return &dma_region[i];
			}
	}

	return 0;
}

/*
 * Forget an allocation, taking it back from every device it was given to.
 *
 * 🔴 THE FOREIGN MAPPINGS GO FIRST, and they are the reason this exists rather
 * than the free path simply revoking its own grant.  A region freed while
 * another device's domain still maps it is memory the VM can hand to a task's
 * stack while a disk can still write it -- which is the exact reach #432
 * exists to remove, reached by freeing in the wrong order.
 */
static void
dma_region_drop(vm_offset_t kva)
{
	unsigned int i, u;

	for (i = 0; i < DEVICE_MAX_DMA_REGIONS; i++) {
		struct dma_region *r = &dma_region[i];

		if (r->kva != kva)
			continue;

		for (u = 0; u < r->nusers; u++)
			(void) device_md_dma_revoke(r->user[u].bdf,
						    (unsigned long)r->pa[0],
						    (unsigned long)r->size);

		/*
		 * 🔴 THE TASK'S MAPPING BEFORE THE PAGES.  See the note on
		 * `task' above: the other order hands wired pages back to the
		 * VM and panics the kernel from a userland RPC.
		 */
		if (r->task != TASK_NULL) {
			(void) vm_map_remove(r->task->map, r->uva,
					     r->uva + r->size,
					     VM_MAP_NO_FLAGS);
			task_deallocate(r->task);
			r->task = TASK_NULL;
		}

		if (r->owner != TASK_NULL) {
			task_deallocate(r->owner);
			r->owner = TASK_NULL;
		}

		kfree((vm_offset_t)r->pa, r->npages * sizeof(vm_offset_t));
		r->kva = 0;
		r->id = 0;
		r->pa = 0;
		r->npages = 0;
		r->nusers = 0;
		return;
	}
}


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
 * 🔴🔴 AND THE ADDRESS RETURNED IS NO LONGER THE PHYSICAL ONE (#432 stage 3e).
 * It is the address the DEVICE must be programmed with: an IOVA, meaningless
 * outside that device's domain -- another device given it reaches nothing, and
 * the processor cannot use it at all.  #457 closed with the clause "a
 * userspace driver server does DMA to a buffer it does not know the physical
 * address of", and until this it knew by construction.
 *
 * 🔑 That closes the isolation in the second direction.  Confining a device to
 * what it was granted stops it reaching elsewhere BY ACCIDENT; not telling it
 * where its memory is stops it doing so ON PURPOSE, because the number it
 * holds means nothing to anything else.
 *
 * ⚠️ On a machine that polices nothing the same field carries the physical
 * address, exactly as before.  One interface, two machines, and no driver has
 * to know which it is on -- which is what makes this a change of MEANING and
 * not of protocol: the parameter is "the address to program", and it always
 * was, it just used to have only one possible value.
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
	vm_address_t		*dma_out,
	uint64_t		*region_id_out)
{
	kern_return_t kr;
	vm_offset_t kva;
	vm_offset_t pa;
	vm_address_t dma;
	uint64_t region_id = 0;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	kr = check_claim(bdf);
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
	 * The physical address is what a machine with nothing to police DMA
	 * hands out, and it is the FALLBACK here rather than the answer.
	 */
	dma = (vm_address_t)pa;

	/*
	 * ⚠️ The grant is a hard failure and not a warning.  A buffer the
	 * device cannot reach is not a slower buffer -- it is one every
	 * transfer into it faults on, at an address the driver believes it
	 * owns, and the driver's own error path has no way to tell that from
	 * the disk being broken.  Failing the allocation says it once, here.
	 */
	/*
	 * ⚠️ RECORDED BEFORE IT IS GRANTED, and failing to record fails the
	 * allocation.  A region the kernel does not remember is one no other
	 * server can ever be given and one whose foreign mappings nothing will
	 * revoke -- so an unrecorded buffer is worse than no buffer.
	 */
	{
		vm_offset_t	pages[DEVICE_CONTIG_MAX_PAGES];
		unsigned int	n = (unsigned int)(size / PAGE_SIZE), i;

		if (n == 0 || n > DEVICE_CONTIG_MAX_PAGES) {
			kmem_free(kernel_map, kva, size);
			return KERN_RESOURCE_SHORTAGE;
		}

		for (i = 0; i < n; i++)
			pages[i] = pa + (vm_offset_t)i * PAGE_SIZE;

		if (!dma_region_add(kva, size, pages, n, TASK_NULL, 0,
				    &region_id)) {
			kmem_free(kernel_map, kva, size);
			return KERN_RESOURCE_SHORTAGE;
		}
	}

	if (bdf != DEVICE_DMA_NO_BDF && device_md_dma_isolates()) {
		unsigned long iova = 0;

		if (!device_md_dma_grant(bdf, (unsigned long)pa,
					 (unsigned long)size, TRUE, TRUE,
					 &iova)) {
			dma_region_drop(kva);
			kmem_free(kernel_map, kva, size);
			return KERN_FAILURE;
		}

		dma = (vm_address_t)iova;
	}

	*vaddr_out = kva;		/* #427: no narrowing cast */
	*dma_out = dma;
	*region_id_out = region_id;
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

	kr = check_claim(bdf);
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

	/*
	 * 🔴 AND EVERY DEVICE THIS REGION WAS LENT TO, before the memory goes
	 * back.  The owner's own grant is above; this is the block server's
	 * disk still reaching a filesystem's page cache.  Freed with either
	 * one still mapped, the page returns to the VM and can become a task's
	 * stack while a device can still write it.
	 */
	dma_region_drop((vm_offset_t)vaddr);

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
 * `dma_addrs' is a vm_map_copy_t handed to MIG, not a caller-supplied buffer, and
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
	vm_address_t		**dma_addrs,
	mach_msg_type_number_t	*dma_addrs_count,
	uint64_t		*region_id_out)
{
	uint64_t	region_id = 0;
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

	kr = check_claim(bdf);
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
		((vm_address_t *)list)[i] = pa;
	}

	/*
	 * 🔴 ONE GRANT OVER THE WHOLE LIST, AFTER IT IS COLLECTED, and not one
	 * per page inside the loop above.  These pages are contiguous in the
	 * kernel's address space and in the task's and NOT in physical memory,
	 * so a range granted from the first address would map whatever lies
	 * between them -- and n separate grants would give the driver n
	 * unrelated addresses and fill the kernel's record of what this device
	 * holds a thousand times over.  Granted together they get consecutive
	 * addresses: the scattering stays a fact about the machine.
	 *
	 * ⚠️ Which is why the list is REWRITTEN afterwards.  Up to here it
	 * holds physical addresses, because that is what pmap_extract answers
	 * and what a machine with no remapping hardware hands out; on one that
	 * confines this device, every entry becomes the address the DEVICE
	 * must be programmed with, and the physical addresses leave with the
	 * loop.
	 *
	 * ⚠️ The failure path is the one above, whole: the range is removed
	 * and the pages freed.  A partially granted scatter-gather list is the
	 * worst of both -- the driver gets an address list it believes, and
	 * the transfer faults on whichever entry was not reached.
	 */
	/*
	 * 🔴 RECORDED BEFORE THE LIST IS REWRITTEN, and this is the whole
	 * reason the recording is here and not beside the allocation.  Below
	 * this point `list' holds ADDRESSES THE DEVICE USES; the kernel's
	 * record has to hold the physical frames, because that is what another
	 * server will name when it asks to map this buffer for its own device.
	 * A recording taken afterwards would remember one domain's IOVAs as if
	 * they were physical memory.
	 */
	if (!dma_region_add(kva, size, (const vm_offset_t *)list, n_pages,
			    task, uva, &region_id)) {
		(void) vm_map_remove(map, uva, uva + size, VM_MAP_NO_FLAGS);
		(void) vm_map_unwire(ipc_kernel_map, list, list + list_size,
				     FALSE);
		kmem_free(ipc_kernel_map, list, list_size);
		kmem_free(kernel_map, kva, size);
		task_deallocate(task);
		return KERN_RESOURCE_SHORTAGE;
	}

	if (bdf != DEVICE_DMA_NO_BDF && device_md_dma_isolates()) {
		unsigned long iova = 0;

		if (!device_md_dma_grant_pages(bdf,
					       (const unsigned long *)list,
					       n_pages, TRUE, TRUE, &iova)) {
			dma_region_drop(kva);
			(void) vm_map_remove(map, uva, uva + size,
					     VM_MAP_NO_FLAGS);
			(void) vm_map_unwire(ipc_kernel_map, list,
					     list + list_size, FALSE);
			kmem_free(ipc_kernel_map, list, list_size);
			kmem_free(kernel_map, kva, size);
			task_deallocate(task);
			return KERN_RESOURCE_SHORTAGE;
		}

		for (i = 0; i < n_pages; i++)
			((vm_address_t *)list)[i] =
				(vm_address_t)(iova + (unsigned long)i
					       * PAGE_SIZE);
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
	*dma_addrs = (vm_address_t *) list_copy;
	*dma_addrs_count = n_pages;
	*region_id_out = region_id;
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

/*
 * ── A device that must be programmed with physical addresses (#432) ──
 *
 * 🔴 NOT AN OPT-OUT OF ISOLATION, and the distinction is the whole of why this
 * exists rather than a flag that says "leave me alone".  The device is still
 * confined: its domain contains what it has been granted and nothing else, and
 * every other address in the machine faults for it.  What it gives up is stage
 * 3e -- not knowing where its memory is.
 *
 * 🔑 AND IT IS A CONSTRAINT AND NOT A PREFERENCE.  A legacy virtio device is
 * SPECIFIED to take physical addresses: there is no VIRTIO_F_ACCESS_PLATFORM
 * to negotiate on the legacy interface, so there is no exchange in which it
 * could be told an address means something else.  Handed an IOVA it programs
 * it as a physical address, the transfer lands on unmapped memory, and the
 * only symptom is a request that never completes.  This tree's virtio driver
 * is that interface; the fix is virtio 1.0, and that is a driver rewrite.
 *
 * ⚠️ A driver could ask for this in order to learn its own physical addresses,
 * and nothing here prevents it.  So the kernel SAYS SO, once, on the console:
 * a weakening that is announced is one somebody can notice, and this interface
 * has exactly one legitimate caller today.
 */
kern_return_t
ds_master_device_dma_identity(
	ipc_port_t		master_port,
	natural_t		bdf)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bdf == DEVICE_DMA_NO_BDF)
		return KERN_INVALID_ARGUMENT;

	kr = check_claim(bdf);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * ⚠️ Success on a machine that polices nothing, and it means what it
	 * says: physical addresses are what such a machine hands out anyway,
	 * so the caller has been given exactly what it asked for.  Failing
	 * here would make every driver carry a branch for the ordinary case.
	 */
	if (!device_md_dma_isolates())
		return KERN_SUCCESS;

	if (!device_md_dma_identity(bdf))
		return KERN_FAILURE;

	printf("iommu: %02x:%02x.%u asked to be programmed with PHYSICAL "
	       "addresses — it is still confined to what it is granted, and "
	       "it knows where that is\n",
	       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
	       (unsigned)(bdf & 7));

	return KERN_SUCCESS;
}

kern_return_t
ds_master_device_dma_owned(
	ipc_port_t		master_port,
	natural_t		bdf,
	natural_t		*by_other)
{
	kern_return_t kr;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	*by_other = (natural_t)device_claimed_by_other(bdf);
	return KERN_SUCCESS;
}

/*
 * ── The server that owns the device maps somebody else's buffer ──────
 *
 * See the note on device_dma_map_foreign in <device/device_master.defs> for
 * why this exists.  In one line: the block server's disk has to read the
 * filesystem's page cache, and the filesystem owns no device to name.
 */
kern_return_t
ds_master_device_dma_map_foreign(
	ipc_port_t		master_port,
	natural_t		bdf,
	vm_address_t		paddr,
	cap_token_t		token,
	mach_msg_type_number_t	tokenCnt,
	vm_address_t		*dma_addr)
{
	kern_return_t		kr;
	struct dma_region	*r;
	struct uros_cap		cap;
	unsigned int		page, u;
	unsigned long		base = 0;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bdf == DEVICE_DMA_NO_BDF)
		return KERN_INVALID_ARGUMENT;

	/* A device has one driver, and only that driver may map for it. */
	kr = check_claim(bdf);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * 🔴 THE PAGE MUST BE ONE THIS KERNEL HANDED OUT FOR DMA.  Without
	 * this the call is "put any physical address inside my device's
	 * reach", which is the property the whole issue exists to create.
	 */
	r = dma_region_of((vm_offset_t)paddr, &page);
	if (r == 0)
		return KERN_INVALID_ADDRESS;

	/*
	 * 🔴 AND A CAPABILITY FOR THAT REGION, WHICH IS THE DELEGATION.
	 * Knowing the address is not the same as having been given the buffer:
	 * before this, a driver could put another server's page cache inside
	 * its device's reach on the strength of an address it happened to see.
	 * The token says the region's OWNER handed it over.
	 */
	if (tokenCnt != sizeof(struct uros_cap))
		return KERN_INVALID_ARGUMENT;

	memcpy(&cap, token, sizeof(cap));

	kr = cap_check_in_kernel(&cap, (uint32_t)CAP_OP_DMA_DEVICE_WRITE,
				 r->id);
	if (kr != KERN_SUCCESS) {
		printf("device: %02x:%02x.%u showed no capability for DMA "
		       "region %lu (kr=%d) — knowing an address is not being "
		       "given the buffer\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7), (unsigned long)r->id, (int)kr);
		return KERN_NO_ACCESS;
	}

	/*
	 * ⚠️ On a machine that polices nothing, the physical address IS the
	 * answer and no mapping happens.  Reported as success because that is
	 * what it is: the caller asked for an address its device can use, and
	 * on such a machine every address is one.
	 */
	if (!device_md_dma_isolates()) {
		*dma_addr = paddr;
		return KERN_SUCCESS;
	}

	for (u = 0; u < r->nusers; u++)
		if (r->user[u].bdf == bdf) {
			base = (unsigned long)r->user[u].dma;
			break;
		}

	if (u == r->nusers) {
		if (r->nusers >= DEVICE_MAX_REGION_USERS)
			return KERN_RESOURCE_SHORTAGE;

		/*
		 * 🔑 THE WHOLE REGION AT ONCE, at consecutive addresses.  The
		 * caller asks page by page and pays for it once: everything
		 * after this is the arithmetic below.
		 */
		if (!device_md_dma_grant_pages(bdf,
					       (const unsigned long *)r->pa,
					       r->npages, TRUE, TRUE, &base))
			return KERN_FAILURE;

		r->user[r->nusers].bdf = bdf;
		r->user[r->nusers].dma = (vm_offset_t)base;
		r->nusers++;

		printf("device: %02x:%02x.%u may now read a %u-page buffer it "
		       "did not allocate, at 0x%lx — mapped by the server that "
		       "owns the device, not by the one that owns the memory\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7), r->npages, base);
	}

	*dma_addr = (vm_address_t)(base + (unsigned long)page * PAGE_SIZE);
	return KERN_SUCCESS;
}

/*
 * ── Claiming a device, by showing a capability for its kind (#432) ───
 *
 * See the note on device_claim in <device/device_master.defs>.  In one line:
 * the token says what KIND of hardware this driver is for, and the claim says
 * which instance of it this task got — and neither is a policy alone.
 */
kern_return_t
ds_master_device_claim(
	ipc_port_t		master_port,
	natural_t		bdf,
	cap_token_t		token,
	mach_msg_type_number_t	tokenCnt)
{
	kern_return_t		kr;
	struct uros_cap		cap;
	unsigned int		class_word;
	uint64_t		class_id;
	task_t			me = current_task();
	unsigned		i;

	kr = check_master_port(master_port);
	if (kr != KERN_SUCCESS)
		return kr;

	if (bdf == DEVICE_DMA_NO_BDF || bdf > 0xFFFFu)
		return KERN_INVALID_ARGUMENT;

	/*
	 * ⚠️ The blob's length is checked against the struct rather than
	 * trusted: MIG bounds it at CAP_TOKEN_MAX, which is the WIRE cap and
	 * deliberately larger than the token so the stubs stay frozen while
	 * the struct evolves.  A short blob copied into a struct would be a
	 * token whose tail is whatever was on the stack.
	 */
	if (tokenCnt != sizeof(struct uros_cap))
		return KERN_INVALID_ARGUMENT;

	memcpy(&cap, token, sizeof(cap));

	/*
	 * 🔑 THE CLASS COMES FROM THE HARDWARE, NOT FROM THE CALLER.  The
	 * token is a signed statement about the task; this is the device
	 * answering for itself, out of its own configuration space.  A caller
	 * that named its own class would be presenting a capability for
	 * whatever it had one for.
	 *
	 * Register 0x08 is revision in 7:0 and class code in 31:8 — base
	 * class, sub-class and programming interface.
	 */
	class_word = device_md_pci_read((unsigned)(bdf >> 8),
					(unsigned)((bdf >> 3) & 0x1F),
					(unsigned)(bdf & 7), 0x08);

	if (class_word == 0xFFFFFFFFu)
		return KERN_INVALID_ARGUMENT;	/* nothing is there */

	class_id = (uint64_t)(class_word >> 8);

	kr = cap_check_in_kernel(&cap, (uint32_t)CAP_OP_PCI_DMA_MAP, class_id);
	if (kr != KERN_SUCCESS) {
		printf("device: %02x:%02x.%u REFUSED to task 0x%lx — its "
		       "capability does not cover class 0x%06lx (kr=%d)\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7), (unsigned long)me,
		       (unsigned long)class_id, (int)kr);
		return KERN_NO_ACCESS;
	}

	for (i = 0; i < device_nclaims; i++)
		if (device_claim[i].bdf == bdf)
			return device_claim[i].task == me
			       ? KERN_SUCCESS : KERN_NO_ACCESS;

	/*
	 * ⚠️ Full is a REFUSAL and not a free-for-all.  A table that stopped
	 * recording would leave every device past the sixteenth claimable by
	 * anybody -- the check turning itself off exactly when there are
	 * enough devices for it to matter.
	 */
	if (device_nclaims >= DEVICE_MAX_CLAIMS)
		return KERN_RESOURCE_SHORTAGE;

	device_claim[device_nclaims].bdf = bdf;
	device_claim[device_nclaims].task = me;
	device_claim[device_nclaims].cap_id = cap.cap_id;
	device_nclaims++;

	printf("device: %02x:%02x.%u (class 0x%06lx) is now driven by task "
	       "0x%lx, which showed a capability for that class\n",
	       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
	       (unsigned)(bdf & 7), (unsigned long)class_id,
	       (unsigned long)me);
	return KERN_SUCCESS;
}

/*
 * ── A revoked capability takes the device with it (#432) ─────────────
 *
 * 🔴 THIS IS WHAT A MATERIALISED CAPABILITY OWES.  A capability that is
 * checked on every use is revoked by refusing the next one.  A capability that
 * is turned into a MAPPING -- and an IOMMU domain is a page table, which is
 * what a mapping has always been -- keeps working after its token is revoked,
 * because nothing consults the token again.  The only way to withdraw it is to
 * tear the mapping down, and that is this.
 *
 * Without it the whole arrangement is capability-SHAPED rather than a
 * capability system: authority that cannot be withdrawn is not authority that
 * was granted, it is authority that was released.
 *
 * 🔑 CALLED FROM THE KERNEL'S OWN REVOKE, NOT FROM AN IPC NOTIFICATION.
 * urmach_cap_revoke is a trap that only cap_server may make, so the kernel
 * learns of a revocation BEFORE anybody it would have to tell -- and a
 * teardown that waited for a message would leave a window in which the token
 * is void and the device still reaching.  cap_server's cap_revoke_notify fan-
 * out still happens, for the servers that hold handles; this is the half no
 * message could do in time.
 *
 * ⚠️ The device is left BLOCKED and not passing through.  See
 * iommu_domain_release(): a revocation that restored the entry the device
 * started in would have widened its reach, which is worse than not revoking
 * because somebody trusted it.
 */
void
device_master_cap_revoked(uint64_t cap_id)
{
	unsigned i;

	for (i = 0; i < device_nclaims; i++) {
		natural_t bdf;

		if (device_claim[i].cap_id != cap_id)
			continue;

		bdf = device_claim[i].bdf;

		printf("device: the capability behind %02x:%02x.%u was revoked "
		       "— the claim is dropped and the device is left reaching "
		       "nothing\n",
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7));

		(void) device_md_dma_release(bdf);

		/*
		 * ⚠️ Compacted, and the loop index is NOT advanced: the entry
		 * moved into this slot has not been looked at.  One token can
		 * stand behind more than one device -- a driver acquires a
		 * capability for a CLASS and claims every instance of it with
		 * the same one -- so a revocation that stopped at the first
		 * match would leave the rest reaching.
		 */
		device_claim[i] = device_claim[device_nclaims - 1];
		device_nclaims--;
		i--;
	}
}

/*
 * ── A driver that dies gives back what it was holding (#513) ─────────
 *
 * 🔴 THE OTHER WAY A MATERIALISED CAPABILITY ENDS.  device_master_cap_revoked
 * above handles the deliberate withdrawal; this one handles the driver simply
 * ceasing to exist, which is the commoner event and had no path at all.  What
 * a dead driver left behind was all three of:
 *
 *	its CLAIM, keyed on a task_t that the zone allocator will hand to
 *	somebody else -- see the note on device_claim[] for why a pointer that
 *	is only ever compared is the dangerous kind;
 *
 *	its DOMAIN, a page table its device keeps walking, since nothing
 *	consults a token to do that;
 *
 *	its BUFFERS, wired pages plus a task reference this file holds -- so
 *	the task struct itself could never be freed either.
 *
 * 🔑 THE BUFFERS BEFORE THE CLAIMS, and it is the same order device_dma_free
 * has for the same reason.  Dropping a region revokes the ranges lent to OTHER
 * devices, whose domains belong to servers that are still running and must be
 * reached one range at a time.  The dying task's own domain is not unmapped
 * range by range: releasing the claim detaches the device, which takes every
 * grant it held at once.
 *
 * ⚠️ The device is left detached and NOT restored to pass-through, for the
 * reason iommu_domain_release() gives: a teardown that put the device back
 * where it started would have widened its reach at the moment its driver died.
 * A new driver that claims it builds a fresh domain.
 */
void
device_master_task_terminating(task_t task)
{
	unsigned int i;

	if (task == TASK_NULL)
		return;

	for (i = 0; i < DEVICE_MAX_DMA_REGIONS; i++) {
		struct dma_region	*r = &dma_region[i];
		vm_offset_t		kva;
		vm_size_t		size;

		if (r->kva == 0)
			continue;
		if (r->owner != task && r->task != task)
			continue;

		kva = r->kva;
		size = r->size;

		printf("device: task 0x%lx died holding DMA region %llu (%lu "
		       "bytes, lent to %u device%s) — taking it back\n",
		       (unsigned long)task, (unsigned long long)r->id,
		       (unsigned long)size, r->nusers,
		       r->nusers == 1 ? "" : "s");

		/*
		 * ⚠️ The mapping in the dying task's own address space goes
		 * down in here, before the pages do.  Its map is still alive:
		 * this file's reference is one of the ones keeping it so, and
		 * that is exactly why the hook cannot live on the free path.
		 */
		dma_region_drop(kva);
		kmem_free(kernel_map, kva, size);
	}

	for (i = 0; i < device_nclaims; i++) {
		natural_t	bdf;

		if (device_claim[i].task != task)
			continue;

		bdf = device_claim[i].bdf;

		printf("device: task 0x%lx died driving %02x:%02x.%u — the "
		       "claim is dropped and the device is left reaching "
		       "nothing\n", (unsigned long)task,
		       (unsigned)(bdf >> 8), (unsigned)((bdf >> 3) & 0x1F),
		       (unsigned)(bdf & 7));

		(void) device_md_dma_release(bdf);

		/* Compacted; see device_master_cap_revoked() on the index. */
		device_claim[i] = device_claim[device_nclaims - 1];
		device_nclaims--;
		i--;
	}
}

/*
 * ── Does this task own that DMA region? (#432) ───────────────────────
 *
 * 🔴 THE HALF cap_server CANNOT KNOW.  Issuing a capability for a buffer means
 * deciding two things: may this task hold buffer capabilities at all, which is
 * its manifest and cap_server's business; and is THIS buffer that task's to
 * give away, which is a fact about an allocation the kernel made and nobody
 * else has ever been told.
 *
 * 🔑 Same shape as the device half.  There the manifest names a class and the
 * kernel reads the class off the hardware; here the manifest permits the KIND
 * of authority and the kernel supplies the instance.  Neither side is asked to
 * know something it has no way of knowing, which is why neither has to be
 * trusted about it.
 *
 * ⚠️ `task' is a name in the CALLER's space -- cap_server's -- and not a
 * global anything.  It is the send right cap_provision_task was handed and
 * that interface has carried since #216 as "informational for now (audit /
 * future cross-reference)".  This is that cross-reference.
 */
kern_return_t
urmach_dma_region_owner(
	uint64_t		region_id,
	mach_port_name_t	task)
{
	task_t		t;
	unsigned int	i;
	kern_return_t	kr = KERN_INVALID_ARGUMENT;

	if (region_id == 0)
		return KERN_INVALID_ARGUMENT;

	t = port_name_to_task(task);
	if (t == TASK_NULL)
		return KERN_INVALID_ARGUMENT;

	for (i = 0; i < DEVICE_MAX_DMA_REGIONS; i++)
		if (dma_region[i].kva != 0 && dma_region[i].id == region_id) {
			kr = dma_region[i].owner == t
			     ? KERN_SUCCESS : KERN_NO_ACCESS;
			break;
		}

	task_deallocate(t);
	return kr;
}
