/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * kern/klog.c — kernel printf ring buffer (#200).  See klog.h.
 *
 * Design: single 64 KiB byte-circular buffer protected by
 * klog_lock.  `head` counts total bytes appended (mod 2^32);
 * `tail` is `head - KLOG_BUF_SIZE` once the buffer fills, otherwise
 * 0.  Both are natural_t — the natural type for the MIG cursor.
 *
 * Locking: simple_lock is enough.  klog_putc is called from inside
 * printf() which already holds printf_lock, but klog_putc must also
 * be safe from contexts that bypass printf() (panic prefix, kdb
 * single-step prints).  Holding a separate lock costs nothing on UP
 * and is correct on MP.
 *
 * 🔴 Except against the processor holding it (#599).  printf() runs in
 * interrupt handlers, and on i386 the spin lock leaves interrupts as the
 * caller had them: a handler whose printf() reached klog_putc while its own
 * interrupted thread held klog_lock -- in klog_putc, or across klog_read()'s
 * copy -- spun on it for ever before its EOI, and nothing in the interrupt's
 * priority class, the tick included, was delivered again.  So the holder
 * writes its number in klog_lock_holder, and a klog_putc that finds its own
 * number is nested inside the holder and leaves the ring alone: that byte
 * still reaches the console through klog_cnputc(), and the log misses it.
 * Masking interrupts for the hold instead is not what splhigh()/splx() do
 * here: splx() turns them back on, whatever the caller had.
 */

#include <kern/lock.h>
#include <kern/cpu_number.h>	/* klog_lock_holder, #599 */
#include <kern/klog.h>
#include <kern/host.h>
#include <string.h>
#include <mach/mach_klog_server.h>	/* #448: interface against implementation */

static char		klog_buf[KLOG_BUF_SIZE];
static natural_t		klog_head;
static natural_t		klog_tail;
static int		klog_ready;

/*
 * #915: what the boot says before klog_init(), kept for the ring.  A log that
 * began at klog_init() had none of the early self-tests -- some 32 KB, 281
 * lines, on a boot of the log bundle -- and on a machine whose memory is
 * cleared at reset that log is the only one.  One processor runs then, with
 * interrupts off: no lock.  klog_init() puts it in front of everything else
 * and says how much did not fit.
 */
#define KLOG_EARLY_SIZE	(48u * 1024u)
static char		klog_early[KLOG_EARLY_SIZE];
static unsigned int	klog_early_len;
static unsigned int	klog_early_lost;
decl_simple_lock_data(static, klog_lock)
static volatile int	klog_lock_holder = -1;	/* #599, see above */

static void
klog_lock_take(void)
{
	simple_lock(&klog_lock);
	klog_lock_holder = cpu_number();
}

static void
klog_lock_drop(void)
{
	klog_lock_holder = -1;
	simple_unlock(&klog_lock);
}

static void
klog_early_keep(char c)
{
	if (klog_early_len < KLOG_EARLY_SIZE)
		klog_early[klog_early_len++] = c;
	else
		klog_early_lost++;
}

void
klog_early_putc(char c)
{
	if (!klog_ready)
		klog_early_keep(c);
}

/* Before klog_ready, into the ring with no lock: klog_init() alone. */
static void
klog_init_put(const char *s)
{
	for (; *s; s++)
		klog_buf[klog_head++ % KLOG_BUF_SIZE] = *s;
}

/*
 * ⚠️ No printf() here: this runs inside printf_init().  So the note on what
 * did not fit is written into the ring by hand, the number with it.
 */
void
klog_init(void)
{
	char		num[12];
	unsigned int	i, n;

	simple_lock_init(&klog_lock, ETAP_MISC_PRINTF);
	for (i = 0; i < klog_early_len; i++)
		klog_buf[i % KLOG_BUF_SIZE] = klog_early[i];
	klog_head = klog_early_len;
	if (klog_early_lost != 0) {
		i = sizeof(num) - 1;
		num[i] = '\0';
		n = klog_early_lost;
		do {
			num[--i] = (char)('0' + n % 10);
			n /= 10;
		} while (n != 0 && i > 0);
		klog_init_put("[klog: ");
		klog_init_put(&num[i]);
		klog_init_put(" bytes said before klog_init() did not fit in "
			      "the ones kept (#915)]\n");
	}
	klog_tail = klog_head > KLOG_BUF_SIZE ? klog_head - KLOG_BUF_SIZE : 0;
	klog_ready = 1;
}

void
klog_putc(char c)
{
	if (!klog_ready) {
		/* #915: kept for klog_init(), no longer dropped. */
		klog_early_keep(c);
		return;
	}
	if (klog_lock_holder == cpu_number())
		return;		/* nested inside the holder: see above */
	klog_lock_take();
	klog_buf[klog_head % KLOG_BUF_SIZE] = c;
	klog_head++;
	if ((natural_t)(klog_head - klog_tail) > KLOG_BUF_SIZE)
		klog_tail = klog_head - KLOG_BUF_SIZE;
	klog_lock_drop();
}

kern_return_t
klog_read(natural_t start,
	  char *buf,
	  mach_msg_type_number_t *count,
	  natural_t *next)
{
	natural_t avail;
	natural_t want;
	natural_t i;
	natural_t copy_start;
	natural_t cap = *count;

	if (!klog_ready) {
		*count = 0;
		*next = 0;
		return KERN_SUCCESS;
	}

	klog_lock_take();

	/* If reader's cursor is outside [tail, head] (more than
	 * KLOG_BUF_SIZE bytes behind, or ahead of head), snap it to
	 * tail.  We computed in unsigned 32-bit modular space so an
	 * "ahead" cursor (>head) wraps into a huge `behind`. */
	avail = klog_head - start;
	if (avail > KLOG_BUF_SIZE) {
		start = klog_tail;
		avail = klog_head - start;
	}

	want = avail < cap ? avail : cap;
	copy_start = start;
	for (i = 0; i < want; i++)
		buf[i] = klog_buf[(copy_start + i) % KLOG_BUF_SIZE];

	*count = want;
	*next = start + want;

	klog_lock_drop();
	return KERN_SUCCESS;
}

natural_t
klog_cursor(void)
{
	natural_t now;

	if (!klog_ready)
		return 0;
	klog_lock_take();
	now = klog_head;
	klog_lock_drop();
	return now;
}

/*
 * MIG entry point (mach_klog_server.c): drop the host arg, defer to
 * klog_read.  Any task with a host_t name can drain — printf already
 * goes out on the serial console where anyone with the right device
 * could see it, so we don't gate on host_priv.
 */
kern_return_t
host_get_log(host_t host,
	     natural_t start,
	     klog_data_t buf,		/* #448: the interface's type, which
					 * carries the bound this writes within;
					 * `char *` decays the same but throws
					 * that away */
	     mach_msg_type_number_t *count,
	     natural_t *next)
{
	kern_return_t	kr;
	unsigned int	n;

	if (host == HOST_NULL)
		return KERN_INVALID_HOST;
	kr = klog_read(start, buf, count, next);

	/*
	 * #851: the stub rounds the array up to a word and puts `next' after
	 * it, and the bytes between are the reply's, from kalloc().
	 */
	if (kr == KERN_SUCCESS)
		for (n = *count; n & 3; n++)
			buf[n] = '\0';
	return kr;
}
