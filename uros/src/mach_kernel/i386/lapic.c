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
 * lapic.c — local APIC bring-up + IPI primitives (#302 increment 1).
 *
 * Public API documented in <i386/lapic.h>.  The MMIO window itself is
 * mapped by mp_table.c into `lapic_start` once the kernel VA system is
 * alive (mp_v1_1_init phase 2).  Each entry point here checks that
 * mapping before touching the device.
 *
 * Vector handlers + IDT wiring come in increment 2; AP-side enable
 * (slave_machine_init) + 8259 masking come in increment 3.
 */

#include <cpus.h>

#if	NCPUS > 1

#include <types.h>
#include <i386/lapic.h>
#include <i386/ioapic.h>		/* ioapic_unmask_irq (#322 replay) */
#include <kern/cpu_data.h>		/* current_cpu_id() */
#include <kern/misc_protos.h>		/* printf */
#include <i386/cpu_number.h>		/* cpu_number() */
#include <machine/AT386/mp/mp.h>	/* MP_AST/MP_CLOCK/MP_KDB/MP_TLB_FLUSH, cpu_int_word */
#include <i386/lock.h>			/* i_bit_clear */
#include <i386/eflags.h>		/* EFL_VM */
#include <i386/thread.h>		/* struct i386_interrupt_state */
#include <kern/ast.h>			/* ast_check */
#include <kern/time_out.h>		/* hertz_tick */
#include <i386/misc_protos.h>		/* slave_clock */
#include <machine/mach_param.h>		/* HZ */
#include <i386/spl.h>			/* splhi/splx */
#include <i386/pit.h>			/* PIT ports for 8254 calibration ref */
#include <i386/pio.h>			/* inb/outb */

extern unsigned char	mp_bsp_lapic_id_get(void);
extern unsigned char	mp_cpu_lapic_id_get(int slot);

/*
 * #311: last TPR value written per CPU, so set_spl/set_spl_noi (spl.S) can
 * skip the LAPIC MMIO write — a KVM VM-exit — when the spl transition does
 * not change the device-mask class.  This mirrors the curr_pic_mask compare
 * the 8259 path used.  Seeded to -1 by ioapic_init() so the first write on
 * each CPU always lands.
 */
int	lapic_tpr_cache[NCPUS];

/* Spin until the previous IPI has been accepted by its target(s).
 * Polling LAPIC_ICR_DS_PENDING in the ICR low word is the only way the
 * SDM blesses for tracking IPI delivery from the source side. */
static void
lapic_ipi_wait(void)
{
	while (LAPIC_REG32(LAPIC_ICR) & LAPIC_ICR_DS_PENDING)
		;
}

void
lapic_enable(void)
{
	unsigned int svr;

	if (lapic_start == 0) {
		printf("lapic_enable: LAPIC not mapped yet, skipping\n");
		return;
	}

	/*
	 * Accept every priority level — TPR=0 means no class-based masking,
	 * which is what the kernel wants while it is the only consumer.
	 */
	LAPIC_REG32(LAPIC_TPR) = 0;

	/*
	 * Spurious interrupt vector: set the enable bit and pin the vector
	 * to LAPIC_SPURIOUS_VECTOR.  Without the enable bit the LAPIC stays
	 * software-disabled and IPIs are simply dropped.
	 */
	svr  = LAPIC_REG32(LAPIC_SVR);
	svr &= ~LAPIC_SVR_MASK;
	svr |= LAPIC_SVR_ENABLE | (LAPIC_SPURIOUS_VECTOR & LAPIC_SVR_MASK);
	LAPIC_REG32(LAPIC_SVR) = svr;
}

void
lapic_eoi(void)
{
	if (lapic_start == 0)
		return;
	LAPIC_REG32(LAPIC_EOI) = 0;
}

/*
 * #312: per-CPU LAPIC timer.
 *
 * `lapic_timer_count` is the INITIAL_COUNT (at divide-by-16) that makes the
 * timer expire once per HZ tick.  We derive it by timing a free-running
 * one-shot LAPIC countdown against the 8254 PIT, which keeps counting in
 * hardware regardless of the interrupt mask -- so the measurement needs no
 * clock tick to advance and works with interrupts off.
 *
 * Calibrated ONCE on the BSP (start_other_cpus), before any AP runs, so the
 * BSP is the sole reader of the shared 8254 (no latch race) and there is no
 * dependency on a concurrent clock interrupt.  The LAPIC bus clock is shared
 * by every core on a socket, so APs reuse the value.
 *
 * Increment 1 only calibrates and prints; the periodic timer LVT, its IDT
 * vector and the switch off the cross-CPU MP_CLOCK IPI come next.
 */
unsigned int	lapic_timer_count;

/*
 * Set once on the BSP at the end of calibration.  From that point the APs own
 * their clock through the local LAPIC timer, so slave_clock() (mp.c) stops
 * forwarding the cross-CPU MP_CLOCK IPI -- the source of #317.  Read by mp.c.
 */
int		lapic_timer_enabled;

extern unsigned int	clknum;		/* rtclock.c: 8254 counts per second */
extern unsigned int	clks_per_int;	/* rtclock.c: 8254 counts per HZ tick */

#define	LAPIC_TIMER_DIV_16	0x03	/* SDM divide-configuration: bus/16 */

/* 64-bit / 32-bit -> 32-bit via a single i386 divl (no libgcc __udivdi3). */
static __inline__ unsigned int
lapic_div64_32(unsigned long long num, unsigned int den)
{
	unsigned int q, r;

	__asm__("divl %4"
		: "=a" (q), "=d" (r)
		: "a" ((unsigned int)num), "d" ((unsigned int)(num >> 32)),
		  "rm" (den));
	return q;
}

/* Latch + read 8254 counter 0 (the system tick).  Only valid with the clock
 * interrupt masked, so no one else latches the counter concurrently. */
#define	LAPIC_READ_8254(v)	{				\
	outb(PITCTL_PORT, PIT_C0);				\
	(v)  = inb(PITCTR0_PORT);				\
	(v) |= inb(PITCTR0_PORT) << 8; }

void
lapic_timer_calibrate(void)
{
	unsigned int	c0, c1, end_count, lapic_ticks, pit_counts, window;
	unsigned int	guard;
	spl_t		s;

	if (lapic_start == 0 || lapic_timer_count != 0)
		return;			/* once per boot; APs reuse the value */

	/* /16 divisor; one-shot (no PERIODIC bit), masked during measurement. */
	LAPIC_REG32(LAPIC_TIMER_DIVIDE_CONFIG) = LAPIC_TIMER_DIV_16;
	LAPIC_REG32(LAPIC_LVT_TIMER) = LAPIC_LVT_MASKED;

	window = clknum / 125;		/* ~8 ms of 8254 counts, < one tick */

	s = splhi();			/* mask the clock IRQ: sole 8254 reader */

	/* Start near the top of an 8254 period so the window cannot wrap. */
	guard = 100000000u;
	do { LAPIC_READ_8254(c0); }
	while (c0 < clks_per_int - clks_per_int / 32 && --guard);

	LAPIC_REG32(LAPIC_INITIAL_COUNT_TIMER) = 0xFFFFFFFFu;	/* start LAPIC */
	guard = 100000000u;
	do { LAPIC_READ_8254(c1); }
	while (c1 <= c0 && (c0 - c1) < window && --guard);

	end_count = LAPIC_REG32(LAPIC_CURRENT_COUNT_TIMER);
	LAPIC_REG32(LAPIC_INITIAL_COUNT_TIMER) = 0;		/* stop LAPIC */
	splx(s);

	pit_counts  = (c1 <= c0) ? (c0 - c1) : 1;	/* 8254 counts elapsed */
	lapic_ticks = 0xFFFFFFFFu - end_count;		/* LAPIC counts elapsed */
	if (pit_counts == 0)
		pit_counts = 1;

	/*
	 * counts/tick = lapic_freq16 / HZ
	 *             = (lapic_ticks * clknum / pit_counts) / HZ
	 *             =  lapic_ticks * clks_per_int / pit_counts   (clks_per_int = clknum/HZ)
	 */
	lapic_timer_count =
		lapic_div64_32((unsigned long long)lapic_ticks * clks_per_int,
			       pit_counts);

	printf("lapic timer: %u lapic / %u pit (~8ms, /16) -> %u counts/tick @ %dHz\n",
	       lapic_ticks, pit_counts, lapic_timer_count, HZ);

	/*
	 * From here on the APs drive their own clock locally (lapic_timer_start
	 * in slave_machine_init): tell slave_clock() to stop forwarding the
	 * cross-CPU MP_CLOCK IPI.  Set last, after lapic_timer_count is valid,
	 * and on the BSP before any AP arms -- the AP isn't scheduling yet, so
	 * the brief gap with neither source is harmless.
	 */
	if (lapic_timer_count != 0)
		lapic_timer_enabled = 1;
}

/*
 * lapic_timer_start() — arm this CPU's LAPIC timer to fire LAPIC_TIMER_VECTOR
 * periodically at the HZ rate, using the count calibrated above.  Called by
 * each AP from slave_machine_init() once its local APIC is enabled.  The
 * timer keeps counting in hardware even while masked by the TPR; the LAPIC
 * holds at most one tick pending in the IRR and delivers it when this CPU
 * drops back to spllo, so a CPU that spends a stretch at high spl coalesces
 * the missed ticks into one -- the same behaviour the masked device clock has.
 */
void
lapic_timer_start(void)
{
	if (lapic_start == 0 || lapic_timer_count == 0)
		return;

	LAPIC_REG32(LAPIC_TIMER_DIVIDE_CONFIG) = LAPIC_TIMER_DIV_16;
	LAPIC_REG32(LAPIC_LVT_TIMER) = LAPIC_TIMER_VECTOR | LAPIC_LVT_PERIODIC;
	LAPIC_REG32(LAPIC_INITIAL_COUNT_TIMER) = lapic_timer_count;
}

/*
 * lapic_timer_handler() — per-CPU clock tick (#312), reached from the ipi.S
 * stub on LAPIC_TIMER_VECTOR.  Replaces the MP_CLOCK IPI for application
 * processors: each AP does its own scheduler accounting via hertz_tick().
 *
 * rtclock_intr() (time-of-day keeping) is deliberately NOT called here -- that
 * stays master-only on the BSP's device clock; an AP only needs the per-CPU
 * quantum/usage accounting hertz_tick() does.
 *
 * Because LAPIC_TIMER_VECTOR sits in TPR class 3, the LAPIC delivered this
 * only because the CPU is at spllo; we therefore CANNOT be nested inside a
 * scheduler lock holder (those raise spl to splsched -> TPR 0x40, which masks
 * class 3).  So thread_quantum_update() may take thread_lock the ordinary way
 * -- none of the #317 cross-CPU trylock hazard applies.
 *
 * Preemption is held off across hertz_tick for the same reason as
 * ipi_mp_handler (#316): hertz_tick's internal balanced enable_preemption
 * would otherwise reach kernel_preempt_check() and context-switch from inside
 * this interrupt (IF=0, frame on the interrupt stack).  We close with
 * mp_enable_preemption_no_check(); the asm stub returns through ipi_ast_return
 * (locore.S), which takes any AST_QUANTUM raised here through a proper trap
 * frame.  `regs` is the saved i386_interrupt_state, read only for usermode/pc.
 */
void
lapic_timer_handler(struct i386_interrupt_state *regs)
{
	boolean_t	usermode;
	extern volatile unsigned int nmi_heartbeat;	/* #344 watchdog */
	extern volatile unsigned int nmi_cpu_tick[];	/* #355 per-cpu watchdog */

	mp_disable_preemption();

	/* #344: bump the NMI-watchdog heartbeat from the per-CPU tick too. */
	nmi_heartbeat++;
	/* #355: also bump THIS cpu's own tick, so the NMI can detect a partial
	 * (single-cpu) wedge instead of only a total clock-stop. */
	nmi_cpu_tick[cpu_number()]++;

	usermode = (regs->efl & EFL_VM) || ((regs->cs & 0x03) != 0);
	hertz_tick(usermode, (vm_offset_t)regs->eip);

	lapic_eoi();
	mp_enable_preemption_no_check();
}

void
lapic_send_ipi(int slot, unsigned int vector)
{
	unsigned char lapic_dest;

	if (lapic_start == 0)
		return;

	lapic_dest = mp_cpu_lapic_id_get(slot);
	if (lapic_dest == 0xFF)
		return;		/* slot unknown — silent, callers check ncpus */

	lapic_ipi_wait();
	LAPIC_REG32(LAPIC_ICRD) =
	    ((unsigned int)lapic_dest & 0xFFu) << LAPIC_ICRD_DEST_SHIFT;
	LAPIC_REG32(LAPIC_ICR)  =
	    LAPIC_ICR_DM_FIXED
	    | LAPIC_ICR_LEVEL_ASSERT
	    | LAPIC_ICR_DSS_DEST
	    | (vector & LAPIC_ICR_VECTOR_MASK);
	lapic_ipi_wait();
}

void
lapic_send_ipi_all_excluding_self(unsigned int vector)
{
	if (lapic_start == 0)
		return;

	lapic_ipi_wait();
	LAPIC_REG32(LAPIC_ICR) =
	    LAPIC_ICR_DM_FIXED
	    | LAPIC_ICR_LEVEL_ASSERT
	    | LAPIC_ICR_DSS_OTHERS
	    | (vector & LAPIC_ICR_VECTOR_MASK);
	lapic_ipi_wait();
}

/*
 * #382: broadcast an NMI to every CPU but this one.  Used by kdb_trap to
 * park the other CPUs for the DDB session: NMI delivery ignores IF and
 * the TPR, so it reaches a CPU wherever it is — including the middle of
 * an IPI handler — without touching its ISR/EOI state (the parked CPU
 * just resumes where it was on release).
 */
void
lapic_send_nmi_all_excluding_self(void)
{
	if (lapic_start == 0)
		return;

	lapic_ipi_wait();
	LAPIC_REG32(LAPIC_ICR) =
	    LAPIC_ICR_DM_NMI
	    | LAPIC_ICR_LEVEL_ASSERT
	    | LAPIC_ICR_DSS_OTHERS;
	lapic_ipi_wait();
}

/*
 * #322 soft-spl (deferred interrupt masking).
 *
 * #311 raised the LAPIC task-priority register to class 4 on every spl above
 * spllo so device IRQs (0x40-0x4f) and the LAPIC timer (0x3f) were masked in
 * hardware while IPIs (class F) stayed deliverable.  Under KVM without APICv
 * each of those TPR MMIO writes is a VM-exit that qemu's vapic TPR-access path
 * turns into a full vCPU register+MSR sync (~18us); an inter-task RPC crosses
 * spl ~5 times and pays ~90us of pure virtualization tax (#322).
 *
 * But the TPR is a pure function of curr_ipl, which the kernel already tracks
 * in software.  So leave the TPR at 0 (the LAPIC masks nothing) and enforce
 * the spl level in software: a maskable vector delivered while curr_ipl>0 is
 * recorded in softspl_pending[cpu] by the dispatch stub (interrupt.S / ipi.S),
 * which returns without running the handler; set_spl/set_spl_noi (spl.S) call
 * softspl_replay() when curr_ipl drops back to spllo, re-injecting each pending
 * source via a self-IPI so its normal IDT entry runs the handler at the safe
 * level.  spl transitions become plain memory writes -- no MMIO, no VM-exit --
 * and the only cost is paid when an interrupt genuinely fires while soft-masked
 * (never on the us-scale RPC hot path).  IPIs are class F: never soft-masked.
 *
 * softspl_enabled gates the scheme: 0 restores the exact #311 hardware-TPR
 * path, for A/B measurement and as a safety fallback.
 */
int		softspl_enabled = 1;
unsigned int	softspl_pending[NCPUS];

/*
 * Re-inject a vector on the current CPU.  DSS_SELF is the SDM self-IPI
 * shorthand; with TPR=0 the vector is latched in the IRR and delivered the
 * instant IF is restored, re-entering its normal IDT stub.
 */
void
lapic_send_self_ipi(unsigned int vector)
{
	if (lapic_start == 0)
		return;

	lapic_ipi_wait();
	LAPIC_REG32(LAPIC_ICR) =
	    LAPIC_ICR_DM_FIXED
	    | LAPIC_ICR_LEVEL_ASSERT
	    | LAPIC_ICR_DSS_SELF
	    | (vector & LAPIC_ICR_VECTOR_MASK);
	lapic_ipi_wait();
}

/*
 * Replay every interrupt deferred on this CPU while it was soft-masked.
 * Called from set_spl/set_spl_noi (IF off) right after curr_ipl drops to
 * spllo.  A deferred device IRQ had its RTE masked at defer time to stop a
 * level-triggered line from storming; unmask it and self-IPI so the line is
 * re-armed and the (possibly edge) interrupt is re-delivered.  The clock is
 * re-injected the same way so timekeeping does not lose the tick.  Bits
 * 0..15 are device IRQs (vector 0x40+irq); SOFTSPL_CLOCK_BIT is the timer.
 */
void
softspl_replay(int cpu)
{
	unsigned int p = softspl_pending[cpu];
	int irq;

	softspl_pending[cpu] = 0;

	for (irq = 0; irq < 16; irq++) {
		if (p & (1u << irq)) {
			ioapic_unmask_irq(irq);
			lapic_send_self_ipi(0x40 + irq);
		}
	}

	if (p & SOFTSPL_CLOCK_BIT)
		lapic_send_self_ipi(LAPIC_TIMER_VECTOR);
}

/*
 * The active cross-CPU path is ipi_mp_handler() below (IPI_VECTOR_MP,
 * sent by cpu_interrupt()).  The RESCHED and CALL_FUNC vectors are not on
 * any current send path — they survive only for the #302 bring-up
 * self-test (start_other_cpus sends a CALL_FUNC to the BSP) and as room
 * for future directed sends; their handlers stay minimal.
 *
 * All IPI handlers run with interrupts disabled (K_INTR_GATE in the IDT),
 * %gs = CPU_DATA, on the stack the LAPIC interrupted — no kmsg pool / no
 * sleeping, exactly like any other hardware interrupt handler.
 */
void
ipi_resched_handler(void)
{
	printf("IPI: resched on cpu %d\n", current_cpu_id());
	lapic_eoi();
}

/*
 * ipi_mp_handler() — #316: the unified cross-CPU IPI handler.
 *
 * cpu_interrupt() (mp_stub.c) sends a single coalescing IPI on
 * IPI_VECTOR_MP after a producer in mp.c sets its reason bit in
 * cpu_int_word[this_cpu].  Here we drain every pending bit and dispatch
 * it.  Previously this routine (then ipi_tlb_shoot_handler) only ran the
 * TLB shootdown, so MP_AST / MP_CLOCK / MP_KDB were silently dropped and
 * cross-CPU reschedule / clock distribution never happened.
 *
 * Entered with IF=0 (K_INTR_GATE), %gs = CPU_DATA.  The ipi.S stub runs us
 * on this CPU's interrupt stack (NOT the interrupted thread's %esp, which
 * is &pcb->iss when the IPI hit user mode — running here would corrupt the
 * saved state).  EOI is our responsibility.  On return the stub re-enters
 * the shared all_intrs AST/preempt epilogue (ipi_ast_return, locore.S) so
 * the AST raised below is actually taken.
 *
 * The whole dispatch runs with preemption DISABLED, exactly like the
 * historical mp_intr().  ast_check() / hertz_tick() internally do balanced
 * mp_disable/enable_preemption(); were our outer level 0, the closing
 * enable would drop to 0 with need_ast already set and fire
 * kernel_preempt_check() — a context switch from INSIDE this interrupt
 * handler (IF=0, no rebuilt trap frame), corrupting the preempted user
 * thread (its EIP comes back garbage -> #GP).  So we hold preemption off,
 * close with mp_enable_preemption_no_check(), and let ipi_ast_return take
 * the deferred AST through the proper trap-frame path.
 *
 * `regs` points at the saved i386_interrupt_state (layout-identical to an
 * all_intrs frame) and is read only for the MP_CLOCK usermode/pc.
 *
 * #304/#310: the TLB shootdown is serviced UNCONDITIONALLY, exactly as the
 * old handler did.  Its ack is tracked by cpu_update_needed[] independently
 * of cpu_int_word, so gating on MP_TLB_FLUSH could skip a shootdown whose
 * bit a prior invocation already cleared, hanging the initiator's spin.  An
 * unconditional local flush is cheap and cannot regress the proven path.
 * We never call the older pmap_update_interrupt(): its process_pmap_updates()
 * reloads CR3 for dead pmaps mid-interrupt, corrupting an AP that took the
 * IPI while running a user thread.
 */
extern void pmap_tlb_shootdown_handler(void);

void
ipi_mp_handler(struct i386_interrupt_state *regs)
{
	int		mycpu;
	int		word;
	boolean_t	sched_safe;

	/*
	 * Capture the INTERRUPTED preemption level before we disable it below.
	 * thread_lock() and every other scheduler simple_lock disable
	 * preemption while held, so a nonzero level here means the code we cut
	 * into already holds one.  ast_check() and
	 * hertz_tick()->thread_quantum_update() re-take exactly those locks;
	 * running them now would self-deadlock — the IPI vector (0xF1) sits
	 * above splsched's TPR class, so unlike the spl-gated mp_intr() of old
	 * it is NOT masked while a CPU holds a scheduler lock.  When it is
	 * unsafe we leave the MP_AST / MP_CLOCK bits pending and let the next
	 * clock IPI retry them: the per-CPU accounting is approximate and the
	 * cross-CPU reschedule ends up at most one tick late.  The TLB
	 * shootdown is lock-free and always safe.
	 */
	sched_safe = (get_preemption_level() == 0);

	mp_disable_preemption();
	mycpu = cpu_number();

	/* MP_TLB_FLUSH: lock-free, safe at any spl — always service. */
	pmap_tlb_shootdown_handler();
	i_bit_clear(MP_TLB_FLUSH, &cpu_int_word[mycpu]);

	/*
	 * Service the coalesced event bits — SINGLE pass (#382).  This used
	 * to be a drain loop ("repeat while we made progress"), but every
	 * producer pairs its i_bit_set with a cpu_interrupt(): a bit that
	 * lands while we are in-service leaves its IPI pending in the LAPIC
	 * IRR and we re-enter right after the EOI, so nothing is ever lost
	 * by leaving it for the next invocation.  The loop, on the other
	 * hand, was a livelock: with several CPUs inside this handler the
	 * MP_CLOCK round-robin (slave_clock passes the tick to the next
	 * running CPU) circulates among them in microseconds, each pass
	 * re-arming `did` — every CPU stays trapped here pre-EOI forever
	 * (in-service 0xF1 pins PPR at 0xF0: no device vector delivers
	 * again), while a TLB-shootdown initiator spins on their acks.
	 * Observed live wedging the whole box out of a DDB session.
	 */
	word = cpu_int_word[mycpu];

	/*
	 * MP_AST: cross-CPU reschedule request (cause_ast_check).
	 * Evaluate this CPU's run state and raise need_ast[] if a switch
	 * is due; ipi_ast_return consumes it on the way out.
	 */
	if (sched_safe && (word & (1 << MP_AST))) {
		i_bit_clear(MP_AST, &cpu_int_word[mycpu]);
		ast_check();
	}

	/*
	 * MP_CLOCK: round-robin clock distribution (slave_clock chain off
	 * the BSP's hardclock).  Do this CPU's per-CPU scheduler
	 * accounting — hertz_tick() decrements the quantum and raises
	 * AST_QUANTUM when it expires — then pass the tick to the next
	 * running CPU.
	 */
	if (sched_safe && (word & (1 << MP_CLOCK))) {
		boolean_t usermode;

		i_bit_clear(MP_CLOCK, &cpu_int_word[mycpu]);
		usermode = (regs->efl & EFL_VM) ||
			   ((regs->cs & 0x03) != 0);
		hertz_tick(usermode, (natural_t)regs->eip);
		slave_clock();
	}

	/*
	 * MP_KDB: remote_kdb() asks this CPU to enter the debugger as a
	 * slave.  Interactive DDB across CPUs needs console arbitration
	 * (kdb_console is still a stub) and is deferred to the post-SMP
	 * DDB rework; just clear the bit.  No regression: it was dropped
	 * before.
	 */
	if (word & (1 << MP_KDB))
		i_bit_clear(MP_KDB, &cpu_int_word[mycpu]);

	lapic_eoi();
	mp_enable_preemption_no_check();
}

void
ipi_call_func_handler(void)
{
	printf("IPI: call-func on cpu %d\n", current_cpu_id());
	lapic_eoi();
}

#endif	/* NCPUS > 1 */
