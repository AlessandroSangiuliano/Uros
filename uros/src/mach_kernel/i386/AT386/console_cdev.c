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
#include <kern/lock.h>
#include <device/conf.h>
#include <device/io_req.h>
#include <device/device_types.h>
#include <i386/pio.h>
#include <i386/AT386/fbcons.h>

extern void com_putc(char c);
extern void cpu_shutdown(void);

/*
 * Shared with kern/printf.c: the single console serialization lock.  Kernel
 * printf() already holds it for the duration of one message; taking the same
 * lock here makes each userspace console write() atomic too, so concurrent
 * writers (servers, the shell, benchmarks) on different CPUs cannot interleave
 * their bytes character-by-character on the COM1 line.  On a uniprocessor
 * simple_lock is a no-op and an in-kernel write runs to completion without a
 * voluntary switch, so no interleaving is possible there anyway.
 */
decl_simple_lock_data(extern, printf_lock)

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

	/* Mirror to the emergency framebuffer console.  No-op until
	 * fbcons_init() has mapped a multiboot2 framebuffer (#342); on a
	 * pure-UEFI box this is the only path that actually shows output,
	 * since com_putc above writes to a COM1 that isn't there. */
	fbcons_putc(c);
}

void
cninit(void)
{
	/* In-kernel VGA initialization removed in #199; in-kernel
	 * keyboard removed in #208.  The COM1 path is brought up by
	 * com_cons_init() at boot, and userspace owns everything else.
	 *
	 * #342: bring up the emergency framebuffer console if GRUB handed
	 * us a multiboot2 framebuffer.  This runs right after i386_init(),
	 * so the pmap is up and io_map() can map the LFB.  It stays a no-op
	 * on legacy/QEMU boots that have a real serial console. */
	fbcons_init();
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


#include <device/console_cdev.h>

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

	/*
	 * Serialize the whole buffer against kernel printf and any other
	 * console writer (shared printf_lock) so SMP writers don't interleave
	 * byte-by-byte on the UART.  cnputc -> com_putc is polled and never
	 * blocks, so the section completes without a voluntary context switch.
	 */
	simple_lock(&printf_lock);
	while (n--)
		cnputc(*p++);
	simple_unlock(&printf_lock);

	ior->io_residual = 0;
	return D_SUCCESS;
}
