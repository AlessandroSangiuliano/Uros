/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * clock_watch.c — a line when processor 0's tick stops (#599).
 *
 * On i386 only processor 0 takes the 8254, and its tick is what advances the
 * clock (rtclock_intr), counts the timeout ticks (timeout_tick) and advances
 * RCU.  When it stops, every timed wait in the system stops returning while
 * everything else keeps running.  Nothing said so: the only signs were a
 * benchmark printing "0 ns" and a boot that never finished, killed at the
 * harness's cap with nobody looking.
 *
 * The other processors tick from their own local APIC timer, so each of them
 * can watch processor 0's tick count, nmi_cpu_tick[0], which hardclock bumps
 * first thing.  The ruler is the watcher's own TSC and not its own ticks:
 * those are deferred at a raised level, coalesced, and can come in a burst
 * after the host stalls the VM.  A stop is declared only when the count has
 * not moved for CW_STOP_MS of this processor's TSC AND for CW_STOP_TICKS of its
 * own ticks, so neither a host stall (the TSC jumps, few ticks) nor a burst of
 * catch-up ticks (many ticks, little TSC) is enough on its own.
 *
 * On a stop, the first processor to see it prints a line without taking a
 * lock and repeats it every CW_REPEAT_MS while the stop lasts: the console is
 * shared, and another processor's output can cut a line (#544).  Once per boot
 * it also sends processor 0 an NMI, and processor 0's NMI handler
 * (nmi_watchdog.c) calls clock_watch_nmi(), which prints where processor 0
 * was: eip, eflags, backtrace, its spl level and pending bits, and its local
 * APIC's priority, in-service and request registers.  Those are what tell
 * #599's hypotheses apart.
 *
 * ⚠️ Nothing here reads the I/O APIC.  Its select and window registers are a
 * pair, and a read from here would be one more accessor racing the others --
 * the very thing #599's first hypothesis is about.
 *
 * Cost when nothing is wrong: one load, one rdtsc and two compares per tick of
 * each application processor.
 */

#include <cpus.h>

#if	NCPUS > 1

#include <mach/vm_param.h>
#include <mach/i386/thread_status.h>	/* struct i386_saved_state */
#include <kern/cpu_data.h>		/* cpu_data[].active_thread */
#include <kern/cpu_number.h>		/* cpu_number, master_cpu */
#include <kern/misc_protos.h>		/* printf */
#include <i386/eflags.h>		/* EFL_IF, EFL_VM */
#include <i386/lapic.h>
#include <i386/apic.h>
#include <i386/clock_watch.h>

extern void			cnputc(char);
extern int			db_active;		/* a processor is in DDB */
extern volatile unsigned int	nmi_cpu_tick[];		/* nmi_watchdog.c */
extern unsigned int		mp_tsc_per_us;		/* rtclock.c */
extern unsigned long long	rtclock_tsc_at_tick;	/* rtclock.c */
extern int			curr_ipl[];		/* spl.S */
extern unsigned int		timeout_ticks;		/* kern/mach_clock.c */

#define	CW_STOP_MS	2000	/* no tick from processor 0 for this long ... */
#define	CW_STOP_TICKS	200	/* ... and for this many of the watcher's own */
#define	CW_REPEAT_MS	10000	/* the line again, while the stop lasts */

/*
 * One per watcher.  Each is written only by its own processor, from its own
 * tick, so it needs no lock.
 */
static struct {
	int			armed;
	unsigned int		seen;	/* nmi_cpu_tick[0] when it last moved */
	unsigned long long	since;	/* this processor's TSC then */
	unsigned int		ticks;	/* this processor's ticks since then */
	unsigned long long	next;	/* when the reporter repeats the line */
} cw[NCPUS];

static volatile int	cw_reporter = -1;	/* the processor that said so */
static volatile int	cw_nmi_sent;		/* the NMI goes once per boot */
static volatile int	cw_dump_wanted;		/* read by processor 0's NMI */

static __inline__ unsigned long long
cw_rdtsc(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
	return ((unsigned long long)hi << 32) | lo;
}

/*
 * Cycles to milliseconds.  divl faults when the quotient does not fit in 32
 * bits, so a delta that large is clamped rather than divided.
 */
static unsigned int
cw_ms(unsigned long long cycles)
{
	unsigned int	us, r;

	if (mp_tsc_per_us == 0)
		return 0;
	if ((unsigned int)(cycles >> 32) >= mp_tsc_per_us)
		return 0xFFFFFFFFu / 1000;
	__asm__("divl %4"
		: "=a" (us), "=d" (r)
		: "a" ((unsigned int)cycles), "d" ((unsigned int)(cycles >> 32)),
		  "rm" (mp_tsc_per_us));
	return us / 1000;
}

/* Console output that takes no lock: the stopped processor may hold one. */
static void
cw_puts(const char *s)
{
	while (*s)
		cnputc(*s++);
}

static void
cw_hex(unsigned int v)
{
	int	i;

	cw_puts("0x");
	for (i = 28; i >= 0; i -= 4)
		cnputc("0123456789abcdef"[(v >> i) & 0xF]);
}

static void
cw_dec(unsigned int v)
{
	char	buf[11];
	int	i = sizeof (buf);

	buf[--i] = '\0';
	do {
		buf[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	cw_puts(&buf[i]);
}

/*
 * The watcher's line.  The anchor's age is read without the lock that
 * rtclock_intr writes it under, so its two halves can tear; it is printed as
 * a hint next to the count, which is the measurement.
 */
static void
cw_say_stopped(int cpu, unsigned long long now)
{
	unsigned long long	anchor = rtclock_tsc_at_tick;

	cw_puts("\nclock: processor 0's tick has not run for ");
	cw_dec(cw_ms(now - cw[cpu].since));
	cw_puts(" ms (watched by processor ");
	cw_dec((unsigned int)cpu);
	cw_puts("): its count ");
	cw_dec(cw[cpu].seen);
	cw_puts(", timeout ticks ");
	cw_dec(timeout_ticks);
	cw_puts(", spl ");
	cw_dec((unsigned int)curr_ipl[master_cpu]);
	cw_puts(", pending ");
	cw_hex(softspl_pending[master_cpu]);
	cw_puts(", the clock's TSC anchor ");
	cw_dec(anchor <= now ? cw_ms(now - anchor) : 0);
	cw_puts(" ms old\n");
}

void
clock_watch_init(void)
{
	printf("clock: the other processors watch processor 0's tick, and say "
	       "so when it has not run for %u ms (#599)\n", CW_STOP_MS);
}


void
clock_watch_tick(int cpu)
{
	unsigned int		count;
	unsigned long long	now, stop;

	if (cpu == master_cpu || mp_tsc_per_us == 0)
		return;

	/*
	 * A DDB session stops every clock on purpose.  Start again from
	 * scratch when it ends rather than report it as a stop.
	 */
	if (db_active) {
		cw[cpu].armed = 0;
		return;
	}

	count = nmi_cpu_tick[master_cpu];
	now = cw_rdtsc();

	if (!cw[cpu].armed || count != cw[cpu].seen) {
		if (cw_reporter == cpu) {
			cw_puts("\nclock: processor 0's tick ran again after ");
			cw_dec(cw_ms(now - cw[cpu].since));
			cw_puts(" ms\n");
			cw_reporter = -1;
		}
		cw[cpu].armed = 1;
		cw[cpu].seen = count;
		cw[cpu].since = now;
		cw[cpu].ticks = 0;
		return;
	}

	if (++cw[cpu].ticks < CW_STOP_TICKS)
		return;
	stop = (unsigned long long)mp_tsc_per_us * (CW_STOP_MS * 1000u);
	if (now - cw[cpu].since < stop)
		return;

	if (cw_reporter != cpu) {
		if (!__sync_bool_compare_and_swap(&cw_reporter, -1, cpu))
			return;
		cw[cpu].next = now;
	}
	if (now < cw[cpu].next)
		return;
	cw[cpu].next = now +
	    (unsigned long long)mp_tsc_per_us * (CW_REPEAT_MS * 1000u);
	cw_say_stopped(cpu, now);

	if (!cw_nmi_sent) {
		cw_nmi_sent = 1;
		cw_dump_wanted = 1;
		lapic_send_nmi(master_cpu);
	}
}

/*
 * Processor 0, in its NMI handler.  Returns 1 when the NMI was the watch's,
 * so nmi_watchdog() does not also take it for its own or for a debugger break.
 */
int
clock_watch_nmi(struct i386_saved_state *regs)
{
	unsigned int	esp, ebp;
	int		i, user;

	if (cpu_number() != master_cpu || !cw_dump_wanted)
		return 0;
	cw_dump_wanted = 0;

	/*
	 * From kernel mode the processor pushed no esp: the interrupted stack
	 * pointer is where the frame it did push ends.
	 */
	user = (regs->efl & EFL_VM) || (regs->cs & 3) != 0;
	esp = user ? regs->uesp : (unsigned int)&regs->uesp;

	cw_puts("\nclock: processor 0 at the NMI: eip ");
	cw_hex(regs->eip);
	cw_puts(user ? " (user)" : " (kernel)");
	cw_puts(", eflags ");
	cw_hex(regs->efl);
	cw_puts((regs->efl & EFL_IF) ? " (interrupts on)" : " (interrupts off)");
	cw_puts(", esp ");
	cw_hex(esp);
	cw_puts(", ebp ");
	cw_hex(regs->ebp);

	cw_puts("\nclock: processor 0's backtrace:");
	ebp = regs->ebp;
	for (i = 0; !user && i < 24 && ebp >= VM_MIN_KERNEL_ADDRESS; i++) {
		unsigned int	*fr = (unsigned int *)ebp;

		cnputc(' ');
		cw_hex(fr[1]);
		if (fr[0] <= ebp)	/* frames ascend; anything else ends it */
			break;
		ebp = fr[0];
	}

	cw_puts("\nclock: processor 0's spl ");
	cw_dec((unsigned int)curr_ipl[master_cpu]);
	cw_puts(", pending ");
	cw_hex(softspl_pending[master_cpu]);
	cw_puts(", active thread ");
	cw_hex((unsigned int)cpu_data[master_cpu].active_thread);

	cw_puts("\nclock: processor 0's local APIC: TPR ");
	cw_hex(LAPIC_REG32(LAPIC_TPR));
	cw_puts(", PPR ");
	cw_hex(LAPIC_REG32(LAPIC_PPR));
	cw_puts(", in service (vectors 255..0)");
	for (i = 7; i >= 0; i--) {
		cnputc(' ');
		cw_hex(LAPIC_REG32(LAPIC_ISR_BASE + 0x10 * i));
	}
	cw_puts(", requested");
	for (i = 7; i >= 0; i--) {
		cnputc(' ');
		cw_hex(LAPIC_REG32(LAPIC_IRR_BASE + 0x10 * i));
	}
	cw_puts("\n");
	return 1;
}

#endif	/* NCPUS > 1 */
