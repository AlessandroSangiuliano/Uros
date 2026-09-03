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
 * device/device_master.h
 *
 * Kernel primitives for userspace drivers.
 */

#ifndef _DEVICE_DEVICE_MASTER_H_
#define _DEVICE_DEVICE_MASTER_H_

#include <mach/kern_return.h>
#include <mach/mach_types.h>
#include <ipc/ipc_port.h>
#include <stdint.h>

/*
 * Interrupt notification message sent to userspace drivers.
 * msgh_id encodes the IRQ number (IRQ_NOTIFY_MSGH_BASE + irq).
 */
#define IRQ_NOTIFY_MSGH_BASE	3000

/*
 * How many notification slots there are.
 *
 * 🔑 SLOTS, and not IRQ numbers, as of #457.  The low sixteen are interrupt
 * lines and a driver names them; the rest are message-signalled interrupts,
 * which have no line number to be named by -- the kernel allocates one and
 * hands it back.  Whether a slot arrives on a pin or in a store by the device
 * is exactly what <device/device_machdep.h> exists to keep out of this file,
 * so the table below does not distinguish them and neither does the thread
 * that drains it.
 *
 * ⚠️ The number a driver may PASS is still bounded by what the machine has
 * lines for, and that bound lives in the machine's own file.  This one bounds
 * the table.
 */
#define IRQ_FORWARD_LINES	16
#define IRQ_FORWARD_MAX		32

/*
 * Per-IRQ forwarding state, stored in irq_forward_table[].
 */
struct irq_forward {
	ipc_port_t	notify_port;	/* send-right to userspace */
	int		active;		/* nonzero if registered */

	/*
	 * 🔴 WHICH KIND OF SLOT THIS IS, because giving one back is not the
	 * same operation for the two (#520).
	 *
	 * A line is released by masking a pin: the machine can do that knowing
	 * nothing but the number.  A message-signalled slot lives in the
	 * DEVICE's own table, so releasing it has to reach the device and put
	 * the mask back there -- which is what device_md_msi_unregister() is
	 * for, and it says so in <device/device_machdep.h>:
	 *
	 *	A device left armed at a vector whose handler is gone raises an
	 *	interrupt nobody claims.
	 *
	 * 🔑 That function was written, was right, and was CALLED BY NOBODY on
	 * the release path -- unregister went to device_md_irq_unregister(),
	 * which returns without doing anything for a slot above the lines.  So
	 * "giving them back answered success" was true and the devices stayed
	 * armed.  Recorded here rather than derived from the slot number,
	 * because the numbering is the machine's and this file is not.
	 */
	int		msi;		/* nonzero if a message, not a line */
};

extern struct irq_forward irq_forward_table[];

/*
 * Initialize the interrupt forwarding subsystem.
 * Called from device_init().
 */
extern void	device_master_init(void);

/*
 * ── What the rest of the kernel has to tell this file ────────────────
 *
 * Both of these take authority away, and neither can be an IPC notification:
 * what they withdraw is MATERIALISED -- an IOMMU domain is a page table the
 * device walks without consulting anything -- so a teardown that waited for a
 * message would leave a window in which the authority is void and the device
 * is still reaching.
 *
 * ⚠️ DECLARED HERE, not `extern' at each call site.  They were, and a
 * declaration written beside the caller is one the compiler never compares
 * against the definition: the two halves agree by inspection until the day
 * somebody changes one of them.
 */

/* A revoked capability takes the mappings it bought with it (#432). */
extern void	device_master_cap_revoked(uint64_t cap_id);

/*
 * A dying task gives back its devices and its DMA buffers (#513).
 *
 * 🔴 Called from task_terminate() and NOT from task_free().  This file holds a
 * reference on the task for every region it recorded, so the reference count
 * cannot reach zero until this has run -- a hook on the free path would be
 * waiting for an event only the hook itself can cause.
 */
extern void	device_master_task_terminating(task_t task);

#endif /* _DEVICE_DEVICE_MASTER_H_ */
