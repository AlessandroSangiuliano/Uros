/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * console_cdev.c — Minimal "console" character device.
 *
 * Replaces kd.c's kdopen/kdwrite/... cdev hooks after #208 retired
 * kd.c.  The "console" name is opened by userspace printf_init()
 * (libsa_mach) so every server can write to the boot/panic console
 * before / without a full graphics + tty stack.
 *
 * Bytes go straight to cnputc → com_putc (polled COM1).  No tty
 * line discipline, no echo, no input — input is owned by userspace
 * (char_server/ps2.so, char_server/uart.so).  This file is just a
 * shim so kernel printf and early userspace printf keep working.
 */

#include <mach/kern_return.h>
#include <kern/misc_protos.h>
#include <device/conf.h>
#include <device/io_req.h>
#include <device/device_types.h>
#include <i386/pio.h>

extern void com_putc(char c);
extern void cpu_shutdown(void);

/* ============================================================
 * Console primitives that used to live in kd.c.  Moved here so
 * cnputc/cninit/kdreboot have a home that doesn't pretend there's
 * a VGA console in the kernel.
 * ============================================================ */

#define K_CR	'\r'
#define K_LF	'\n'

void
cnputc(char c)
{
	/* CR/LF mapping kept for callers that print "\n" expecting
	 * the line to advance on a tty receiver — same behaviour the
	 * old kd_putc + serial mirror provided. */
	if (c == K_LF) {
		com_putc(K_CR);
		com_putc(K_LF);
	} else {
		com_putc(c);
	}
}

void
cninit(void)
{
	/* In-kernel VGA initialization removed in #199; in-kernel
	 * keyboard removed in #208.  Nothing for cninit to do — the
	 * COM1 path is brought up by com_cons_init() at boot, and
	 * userspace owns everything else. */
}

void
kdreboot(void)
{
	/* Pulse the 8042 controller's CPU reset line (0xFE on port
	 * 0x64).  If that fails — e.g. on a modern board without an
	 * 8042 — fall through to cpu_shutdown(), which clears IDTR
	 * and forces a triple-fault. */
	outb(0x64, 0xFE);
	cpu_shutdown();
	/*NOTREACHED*/
}


io_return_t
consoleopen(dev_t dev, dev_mode_t flag, io_req_t ior)
{
	(void)dev;
	(void)flag;
	(void)ior;
	return D_SUCCESS;
}

void
consoleclose(dev_t dev)
{
	(void)dev;
}

io_return_t
consolewrite(dev_t dev, io_req_t ior)
{
	char	*p;
	unsigned int n;

	(void)dev;
	if (ior == 0)
		return D_INVALID_OPERATION;

	p = (char *)ior->io_data;
	n = ior->io_count;
	while (n--)
		cnputc(*p++);

	ior->io_residual = 0;
	return D_SUCCESS;
}
