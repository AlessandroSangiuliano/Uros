/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * kern/klog.h — in-kernel printf ring buffer (#200).
 *
 * Every char that goes out of the kernel via printf() is also
 * appended to a 128 KiB circular buffer.  Userspace drains it with
 * the host_get_log MIG RPC (see mach/mach_klog.defs) so that the
 * VGA path retired in #199 can be replaced by a userspace
 * log_forwarder pthread that mirrors kernel output to gpu_server.
 *
 * Cursor semantics: 32-bit unsigned, monotonic in modular
 * arithmetic.  Readers pass back the `next` they received last
 * time.  If the reader fell more than 128 KiB behind, the kernel
 * silently jumps the cursor to the oldest still-buffered byte
 * (data loss is preferable to blocking printf).
 */

#ifndef _KERN_KLOG_H_
#define _KERN_KLOG_H_

#include <mach/kern_return.h>
#include <mach/message.h>

/*
 * 128 KiB (#915): a boot says about 140 KB on the serial line, and since the
 * narration before klog_init() is kept too (klog_early_putc()) a reader that
 * starts with the first tasks -- bootlog, writing the log to a disk -- finds
 * some 60 KB already in the ring.  A power of two, as the cursor's modular
 * arithmetic needs.
 */
#define KLOG_BUF_SIZE	131072u

/* klog_data_t (MIG ctype) lives in <mach/host_info.h> alongside
 * kernel_boot_info_t since the same translation units need both. */

/* Append one character.  Safe from any context (printf, panic, kdb). */
extern void	klog_putc(char c);

/*
 * #915: a character of the boot's narration before klog_init(), kept for the
 * ring (kputs() on x86-64, which reaches the console without printf()).
 * Nothing once klog_init() has run: the narration after it stays the
 * console's, as it was, so that this is never asked a processor number.
 */
extern void	klog_early_putc(char c);

/* Initialize lock — call once from startup before SMP comes up. */
extern void	klog_init(void);

/*
 * Drain bytes written since `start`.  Copies up to *count bytes into
 * `buf`, returns the new cursor in `*next`, and *count is updated to
 * the number of bytes actually delivered (may be 0 if reader is
 * caught up).
 */
extern kern_return_t klog_read(natural_t start,
			       char *buf,
			       mach_msg_type_number_t *count,
			       natural_t *next);

/*
 * The write cursor right now: the `start' a reader passes to klog_read() to
 * receive everything appended from this instant on and nothing before it
 * (#497).  The console takes it when it hands its port to a driver, so the
 * driver's forwarder can carry every line the kernel says after that and
 * repeat none it said before.
 */
extern natural_t klog_cursor(void);

#endif /* _KERN_KLOG_H_ */
