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
#include <kern/spl.h>			/* splsched, for an ablation */
#include <i386/cn_nolock.h>		/* the stopped processor may hold the console lock */

#ifdef ABLATE_599_CLI_SPIN
#include <kern/lock.h>
decl_simple_lock_data(extern, timer_lock)	/* kern/mach_clock.c */
#endif

extern int			db_active;		/* a processor is in DDB */
extern volatile unsigned int	nmi_cpu_tick[];		/* nmi_watchdog.c */
extern unsigned int		mp_tsc_per_us;		/* rtclock.c */
extern unsigned long long	rtclock_tsc_at_tick;	/* rtclock.c */
extern int			curr_ipl[];		/* spl.S */
extern unsigned int		timeout_ticks;		/* kern/mach_clock.c */
extern volatile unsigned int	ioapic_overlaps;	/* ioapic.c */

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

/*
 * The watcher's line.  The anchor's age is read without the lock that
 * rtclock_intr writes it under, so its two halves can tear; it is printed as
 * a hint next to the count, which is the measurement.
 */
static void
cw_say_stopped(int cpu, unsigned long long now, const char *where)
{
	unsigned long long	anchor = rtclock_tsc_at_tick;

	cn_puts("\nclock: processor 0's tick has not run for ");
	cn_dec(cw_ms(now - cw[cpu].since));
	cn_puts(" ms (watched by processor ");
	cn_dec((unsigned int)cpu);
	if (where != 0) {
		cn_puts(", spinning in ");
		cn_puts(where);
	}
	cn_puts("): its count ");
	cn_dec(cw[cpu].seen);
	cn_puts(", timeout ticks ");
	cn_dec(timeout_ticks);
	cn_puts(", spl ");
	cn_dec((unsigned int)curr_ipl[master_cpu]);
	cn_puts(", pending ");
	cn_hex(softspl_pending[master_cpu]);
	cn_puts(", the clock's TSC anchor ");
	cn_dec(anchor <= now ? cw_ms(now - anchor) : 0);
	cn_puts(" ms old, I/O APIC overlaps ");
	cn_dec(ioapic_overlaps);
	cn_puts("\n");
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
	printf("clock: #599 ablation -- at tick %u processor 0 takes the timeout "
	       "list's lock and spins with interrupts off\n", CW_ABLATE_AT);
#endif
#ifdef ABLATE_599_SPIN_ONLY
	printf("clock: #599 ablation -- the watch does not run from the tick, "
	       "only from inside spins\n");
#endif
#ifdef ABLATE_599_SPL_LEAK
	printf("clock: #599 ablation -- after tick %u a thread_switch on "
	       "processor 0 returns to user mode at splsched, once\n",
	       CW_ABLATE_AT);
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
 *		EOI.  It takes the timeout list's lock first and keeps it, so
 *		that any other processor arming a timeout ends up spinning on
 *		it within a tick -- which is what makes SPIN_ONLY's answer
 *		certain rather than a matter of what the others were doing;
 *   SPIN_ONLY	the watch does not run from the tick, so that with CLI_SPIN
 *		only a spin (clock_watch_spin) can say so.
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
		cn_puts("\nclock: #599 ablation -- processor 0 masks the 8254's "
			"pin now\n");
		__asm__ volatile("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
		ioapic_mask_irq(0);
		__asm__ volatile("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
	}
#endif
#ifdef ABLATE_599_CLI_SPIN
	cn_puts("\nclock: #599 ablation -- processor 0 takes the timeout list's "
		"lock and spins with interrupts off now\n");
	simple_lock(&timer_lock);
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
			cn_puts("\nclock: processor 0's tick ran again after ");
			cn_dec(cw_ms(now - cw[cpu].since));
			cn_puts(" ms\n");
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

#ifdef ABLATE_599_SPL_LEAK
/*
 * #599's second hypothesis, on purpose: processor 0 goes back to user mode
 * with its level raised, the way kern/subsystem.c's last release and
 * kern/eventcount.c's evc_wait would.  A boot with it shows that
 * return_to_user's check (spl_to_user_seen, trap.c) says so, and whether a
 * raised level on processor 0 is enough to stop the tick, and for how long:
 * the next switch to another thread may lower it.
 */
void
clock_watch_ablate_spl_leak(void)
{
	static int	done;

	if (cpu_number() != master_cpu || done ||
	    nmi_cpu_tick[master_cpu] < CW_ABLATE_AT)
		return;
	done = 1;
	cn_puts("\nclock: #599 ablation -- this thread_switch returns to user "
		"mode at splsched now\n");
	(void) splsched();
}
#endif

void
clock_watch_tick(int cpu)
{
#ifndef ABLATE_599_SPIN_ONLY
	cw_watch(cpu, 0);
#endif
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

	cn_puts("\nclock: processor 0 at the NMI: eip ");
	cn_hex(regs->eip);
	cn_puts(user ? " (user)" : " (kernel)");
	cn_puts(", eflags ");
	cn_hex(regs->efl);
	cn_puts((regs->efl & EFL_IF) ? " (interrupts on)" : " (interrupts off)");
	cn_puts(", esp ");
	cn_hex(esp);
	cn_puts(", ebp ");
	cn_hex(regs->ebp);

	cn_puts("\nclock: processor 0's backtrace:");
	ebp = regs->ebp;
	for (i = 0; !user && i < 24 && ebp >= VM_MIN_KERNEL_ADDRESS; i++) {
		unsigned int	*fr = (unsigned int *)ebp;

		cnputc(' ');
		cn_hex(fr[1]);
		if (fr[0] <= ebp)	/* frames ascend; anything else ends it */
			break;
		ebp = fr[0];
	}

	cn_puts("\nclock: processor 0's spl ");
	cn_dec((unsigned int)curr_ipl[master_cpu]);
	cn_puts(", pending ");
	cn_hex(softspl_pending[master_cpu]);
	cn_puts(", active thread ");
	cn_hex((unsigned int)cpu_data[master_cpu].active_thread);

	cn_puts("\nclock: processor 0's local APIC: TPR ");
	cn_hex(LAPIC_REG32(LAPIC_TPR));
	cn_puts(", PPR ");
	cn_hex(LAPIC_REG32(LAPIC_PPR));
	cn_puts(", in service (vectors 255..0)");
	for (i = 7; i >= 0; i--) {
		cnputc(' ');
		cn_hex(LAPIC_REG32(LAPIC_ISR_BASE + 0x10 * i));
	}
	cn_puts(", requested");
	for (i = 7; i >= 0; i--) {
		cnputc(' ');
		cn_hex(LAPIC_REG32(LAPIC_IRR_BASE + 0x10 * i));
	}
	cn_puts("\n");
	return 1;
}

#endif	/* NCPUS > 1 */
