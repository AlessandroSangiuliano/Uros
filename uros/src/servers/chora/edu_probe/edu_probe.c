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
 * edu_probe -- a test module of chora, for QEMU's educational device: it maps
 * a region and takes one interrupt by message, and drives nothing (#977).
 *
 * edu (vendor 1234h, device 11e8h, docs/specs/edu.rst in QEMU 11.1.2) has MSI
 * with one message and no MSI-X table, which is why it is here: a device of
 * the kind that took its INTx line before #977, and on q35 could not even do
 * that, since libdriver takes no line above 15.  Written to its register 60h,
 * it raises an interrupt and keeps the value in its status, 24h, until the
 * value is written to 64h -- which the handler does, as edu.rst says a driver
 * using MSI must.
 *
 * So: the device's first region mapped; the interrupt asked of libdriver,
 * which must be a message; 977h raised; and up to a second for the handler.
 * chora said the claim, and this says the rest.
 *
 * ⚠️ A test module: what it was asked to see and did not is a WRONG.
 */

#include "libdriver.h"
#include "chora_module.h"

#include <mach.h>
#include <mach/thread_switch.h>

#define EDU_INTR_STATUS		0x24u
#define EDU_INTR_RAISE		0x60u
#define EDU_INTR_ACK		0x64u
#define EDU_INTR_VALUE		0x977u

#define PROBE_WAIT_STEPS	100		/* of 10 ms: a second */

static volatile uint32_t	*regs;
static volatile int		rang;
static volatile uint32_t	rang_status;

/* On the interrupt thread: what was raised, read and acknowledged. */
static void
edu_interrupt(struct driver *d, void *arg)
{
	(void)d;
	(void)arg;
	rang_status = regs[EDU_INTR_STATUS / 4];
	regs[EDU_INTR_ACK / 4] = rang_status;
	rang++;
}

static int
edu_attach(struct driver *d)
{
	volatile void	*va = NULL;
	vm_size_t	size = 0;
	kern_return_t	kr;
	int		i;

	kr = driver_map_region(d, 0, &va, &size);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- region 0, the registers, would not map: "
			   "0x%x", kr);
		return -1;
	}
	regs = (volatile uint32_t *)va;
	driver_log(d, "mapped region 0, %lu bytes at 0x%lx", (unsigned long)size,
		   (unsigned long)(vm_address_t)va);

	kr = driver_interrupt(d, edu_interrupt, NULL);
	if (kr != KERN_SUCCESS) {
		driver_log(d, "WRONG -- no interrupt to take: 0x%x", kr);
		return -1;
	}
	if (!d->irq_message) {
		driver_log(d, "WRONG -- given INTx line %u, where its MSI was a "
			   "message to take", (unsigned)d->irq_slot);
		return -1;
	}

	regs[EDU_INTR_RAISE / 4] = EDU_INTR_VALUE;
	for (i = 0; i < PROBE_WAIT_STEPS && rang == 0; i++)
		(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 10);

	if (rang == 0) {
		driver_log(d, "WRONG -- told to raise 0x%x, the device sent "
			   "nothing in a second to message slot %u",
			   EDU_INTR_VALUE, (unsigned)d->irq_slot);
		return -1;
	}
	driver_log(d, "took an interrupt, message slot %u, status 0x%x",
		   (unsigned)d->irq_slot, (unsigned)rang_status);
	return 0;
}

const struct chora_module_ops edu_probe_chora_ops = {
	.name	= "edu_probe",
	.attach	= edu_attach,
};
