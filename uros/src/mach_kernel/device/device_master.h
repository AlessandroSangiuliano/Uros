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
};

extern struct irq_forward irq_forward_table[];

/*
 * Initialize the interrupt forwarding subsystem.
 * Called from device_init().
 */
extern void	device_master_init(void);

#endif /* _DEVICE_DEVICE_MASTER_H_ */
