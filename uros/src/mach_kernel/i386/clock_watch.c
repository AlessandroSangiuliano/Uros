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
 * A processor spinning with interrupts off takes no tick, so the waits on
 * another processor run the same watch from inside the spin
 * (clock_watch_spin).
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
#include <i386/ioapic.h>		/* ioapic_mask_irq, for an ablation */
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
#define	CW_CONFIRM_MS	100	/* a spin's second look, see clock_watch_spin */
#define	CW_ABLATE_AT	3000	/* processor 0's ticks, for the #599 ablations */

/*
 * One per watcher.  Each is written only by its own processor, from its own
 * tick, so it needs no lock.
 */
static struct {
	int			armed;
	unsigned int		seen;	/* nmi_cpu_tick[0] when it last moved */
	unsigned long long	since;	/* this processor's TSC then */
	unsigned int		ticks;	/* this processor's ticks since then */
	unsigned long long	confirm; /* a spin's second look, 0 = none yet */
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
cw_say_stopped(int cpu, unsigned long long now, const char *where)
{
	unsigned long long	anchor = rtclock_tsc_at_tick;

	cw_puts("\nclock: processor 0's tick has not run for ");
	cw_dec(cw_ms(now - cw[cpu].since));
	cw_puts(" ms (watched by processor ");
	cw_dec((unsigned int)cpu);
	if (where != 0) {
		cw_puts(", spinning in ");
		cw_puts(where);
	}
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
#ifdef ABLATE_599_MASK_PIT
	printf("clock: #599 ablation -- at tick %u processor 0 masks the 8254's "
	       "pin and leaves nothing pending\n", CW_ABLATE_AT);
#endif
#ifdef ABLATE_599_CLI_SPIN
	printf("clock: #599 ablation -- at tick %u processor 0 spins with "
	       "interrupts off\n", CW_ABLATE_AT);
#endif
}

#if defined(ABLATE_599_MASK_PIT) || defined(ABLATE_599_CLI_SPIN)
/*
 * Stop processor 0's tick on purpose, from its own tick, CW_ABLATE_AT ticks
 * into the boot -- about half-way through the full bundle's tests.  Each is
 * the shape of one of #599's hypotheses, so a boot with it shows that the
 * watch fires on that shape and what the NMI's dump then says:
 *
 *   MASK_PIT	pin 2 masked with no pending bit to unmask it again, which is
 *		what a lost read-modify-write of the I/O APIC leaves;
 *   CLI_SPIN	processor 0 spinning with interrupts off, never sending its
 *		EOI.
 */
void
clock_watch_ablate(int cpu)
{
	static int	done;

	if (cpu != master_cpu || done || nmi_cpu_tick[cpu] < CW_ABLATE_AT)
		return;
	done = 1;
#ifdef ABLATE_599_MASK_PIT
	{
		unsigned int	flags;

		/*
		 * With interrupts off, as every other caller does it: hardclock
		 * runs with them on, and a device interrupt deferred here would
		 * do its own read-modify-write inside this one.
		 */
		cw_puts("\nclock: #599 ablation -- processor 0 masks the 8254's "
			"pin now\n");
		__asm__ volatile("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
		ioapic_mask_irq(0);
		__asm__ volatile("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
	}
#endif
#ifdef ABLATE_599_CLI_SPIN
	cw_puts("\nclock: #599 ablation -- processor 0 spins with interrupts off "
		"now\n");
	__asm__ volatile("cli");
	for (;;)
		__asm__ volatile("pause");
#endif
}
#endif

/*
 * The watch itself.  `where' is 0 from the watcher's own tick, and names the
 * wait it was spinning in otherwise; the two differ only in what stands in
 * for the watcher's own ticks, which a spinning processor does not take.
 */
static void
cw_watch(int cpu, const char *where)
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
		cw[cpu].confirm = 0;
		return;
	}

	if (where == 0 && ++cw[cpu].ticks < CW_STOP_TICKS)
		return;
	stop = (unsigned long long)mp_tsc_per_us * (CW_STOP_MS * 1000u);
	if (now - cw[cpu].since < stop)
		return;
	if (where != 0) {
		if (cw[cpu].confirm == 0)
			cw[cpu].confirm = now +
			    (unsigned long long)mp_tsc_per_us *
			    (CW_CONFIRM_MS * 1000u);
		if (now < cw[cpu].confirm)
			return;
	}

	if (cw_reporter != cpu) {
		if (!__sync_bool_compare_and_swap(&cw_reporter, -1, cpu))
			return;
		cw[cpu].next = now;
	}
	if (now < cw[cpu].next)
		return;
	cw[cpu].next = now +
	    (unsigned long long)mp_tsc_per_us * (CW_REPEAT_MS * 1000u);
	cw_say_stopped(cpu, now, where);

	if (!cw_nmi_sent) {
		cw_nmi_sent = 1;
		cw_dump_wanted = 1;
		lapic_send_nmi(master_cpu);
	}
}

void
clock_watch_tick(int cpu)
{
	cw_watch(cpu, 0);
}

/*
 * The same watch from inside a wait on another processor, found blind by the
 * CLI_SPIN ablation: when processor 0 stopped with interrupts off, every other
 * processor was in a TLB shootdown's wait for processor 0's ack within ten of
 * its own ticks, spinning with interrupts off too, and none of them ticked
 * again to say so.  The waits call this every 1024 turns
 * (MACHINE_SPIN_WATCH, <i386/lock.h>): a simple lock's spin and a TLB
 * shootdown's wait for its acks.  The assembly spins of i386_lock.S do not.
 *
 * With no ticks of its own to count, a spin declares a stop after CW_STOP_MS of
 * its TSC and a second look CW_CONFIRM_MS later with processor 0's count still
 * where it was: a host stall jumps the TSC once, and processor 0 ticks within
 * one tick of the machine running again.  Interrupts go off for the look, so
 * the watcher's own tick cannot cut into the state it shares with it.
 */
void
clock_watch_spin(const char *where)
{
	unsigned int	flags;

	__asm__ volatile("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	cw_watch(cpu_number(), where);
	__asm__ volatile("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
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
