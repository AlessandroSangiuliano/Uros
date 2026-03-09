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
