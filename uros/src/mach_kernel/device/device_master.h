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
 * Maximum IRQ number we support for forwarding.
 */
#define IRQ_FORWARD_MAX		16

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
