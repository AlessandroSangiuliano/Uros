/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * kern/klog.h — in-kernel printf ring buffer (#200).
 *
 * Every char that goes out of the kernel via printf() is also
 * appended to a 64 KiB circular buffer.  Userspace drains it with
 * the host_get_log MIG RPC (see mach/mach_klog.defs) so that the
 * VGA path retired in #199 can be replaced by a userspace
 * log_forwarder pthread that mirrors kernel output to gpu_server.
 *
 * Cursor semantics: 32-bit unsigned, monotonic in modular
 * arithmetic.  Readers pass back the `next` they received last
 * time.  If the reader fell more than 64 KiB behind, the kernel
 * silently jumps the cursor to the oldest still-buffered byte
 * (data loss is preferable to blocking printf).
 */

#ifndef _KERN_KLOG_H_
#define _KERN_KLOG_H_

#include <mach/kern_return.h>
#include <mach/message.h>

#define KLOG_BUF_SIZE	65536u

/* klog_data_t (MIG ctype) lives in <mach/host_info.h> alongside
 * kernel_boot_info_t since the same translation units need both. */

/* Append one character.  Safe from any context (printf, panic, kdb). */
extern void	klog_putc(char c);

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

#endif /* _KERN_KLOG_H_ */
