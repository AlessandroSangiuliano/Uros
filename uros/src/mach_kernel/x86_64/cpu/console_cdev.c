/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The console as a Mach device: a printf sink, not a driver (#426).
 *
 * ── Why this exists on a target that has decided it has no drivers ────
 *
 * conf.c next door says, at length, that in-kernel device drivers are not
 * coming back here: network, block and character devices belong to servers in
 * user space reached through the device master port (#427, #432).  That still
 * holds, and this does not weaken it.
 *
 * What this is, is the other thing i386's console cdev is -- its own comment
 * calls it "the kernel/early-userspace printf sink".  Every server's first act
 * is printf_init(), which opens "console" and writes to it; libmach's printf
 * has no other way out, and panic() goes through printf.  With no such device
 * the first line of every server on this target was
 *
 *     libmach: device_open("console") failed: ... (0x000009c6)
 *
 * D_NO_SUCH_DEVICE, correctly, from a table that has none.  So userland could
 * not say anything at all -- including why it had failed.
 *
 * ⚠️ It moves real bytes to the real console, which is the line conf.c draws:
 * "inventing a stub console here would be a device that reports success and
 * moves no bytes".  cnputc() is the same entry the kernel's own printf uses,
 * and below it x86_64/ddb/cons.c knows the serial port -- and only that, the
 * framebuffer this used to name being one no code on this target touches
 * (#497).  Nothing here touches hardware, claims an interrupt, or owns a
 * resource a user-space driver will want.
 *
 * ⚠️ WRITE-ONLY, and the read handler must be NO_READ rather than NULL_READ.
 * That is not symmetry, it is a bug i386 already paid for: nulldev reports
 * D_SUCCESS, which leaves io_data NULL and io_residual 0, so ds_read_done()
 * computes size_read == io_count and marshals that many bytes out of a null
 * pointer.  D_INVALID_OPERATION is the honest answer -- console input belongs
 * to whoever owns the keyboard, and here that is nobody yet.
 */

#include <device/conf.h>
#include <device/device_types.h>
#include <device/io_req.h>
#include <device/console_cdev.h>
#include <kern/lock.h>
#include <kern/misc_protos.h>

extern void cnputc(char);

decl_simple_lock_data(extern, printf_lock)

/* The most a hold may carry when the caller's bytes have no newline (#551). */
#define CONSOLE_CHUNK	256

io_return_t
consoleopen(dev_t dev, dev_mode_t flag, io_req_t ior)
{
	(void) dev;
	(void) flag;
	(void) ior;
	return D_SUCCESS;
}

void
consoleclose(dev_t dev)
{
	(void) dev;
}

io_return_t
consolewrite(dev_t dev, io_req_t ior)
{
	char *p;
	unsigned int n;

	(void) dev;
	if (ior == 0)
		return D_INVALID_OPERATION;

	p = (char *) ior->io_data;
	n = ior->io_count;

	/*
	 * One LINE under the lock the kernel's own printf takes, not the whole
	 * buffer (#551).
	 *
	 * The lock is what keeps a server's line and a kernel line from
	 * interleaving byte by byte on the wire, and a line is the unit that
	 * needs it.  The whole buffer used to be under it, and the buffer's
	 * length is the CALLER's choice: on this target the hold masks
	 * interrupts (#528) and a byte costs what the wire costs -- measured,
	 * cons_cost_report() prints it every boot -- so a 4 KB write was a
	 * tenth of a second of one processor with interrupts off, sized from
	 * userland.  Now a hold is one line, or CONSOLE_CHUNK bytes of a line
	 * that has no newline in it, and what a writer chooses is how many
	 * such holds it takes in a row, between any two of which the
	 * scheduler may run somebody else.  cnputc is polled and bounded
	 * (#551), so a hold ends without a voluntary context switch and
	 * without waiting on a transmitter that will not empty.
	 */
	while (n > 0) {
		unsigned int k;

		for (k = 0; k < n && k < CONSOLE_CHUNK; k++)
			if (p[k] == '\n') {
				k++;
				break;
			}
		n -= k;
		simple_lock(&printf_lock);
		while (k--)
			cnputc(*p++);
		simple_unlock(&printf_lock);
	}

	ior->io_residual = 0;
	return D_SUCCESS;
}
