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
 * nmi_watchdog.c — perf-counter NMI hard-lockup detector (#344).
 *
 * The omen bare-metal wedge is a silent freeze with interrupts disabled
 * (IF=0): the LAPIC-timer / device-clock IRQ is masked, so nothing advances
 * and an IRQ-based debugger entry (Ctrl+D) can never break in.  An NMI is the
 * only thing that still fires at IF=0.
 *
 * We program a general-purpose performance counter (IA32_PMC0, unhalted core
 * cycles) to overflow roughly every ~100 ms and route the LAPIC performance-
 * counter LVT to NMI delivery.  Each NMI checks a heartbeat that the clock
 * tick bumps at IF=1: if it has not advanced for several NMIs the CPU is
 * wedged, and we dump the interrupted EIP + registers + a kernel-stack
 * backtrace LOCK-FREE (cnputc, which never takes printf_lock) so the output
 * reaches fbcons even if the wedged CPU holds the console lock.
 *
 * Caveat: unhalted-cycles counts only while the CPU is executing, so this
 * catches a spin/deadlock at IF=0.  A CPU genuinely HLT'd at IF=0 stops the
 * counter and would not be caught — but that case prints nothing either way,
 * and the absence of a watchdog report is itself the distinguishing signal.
 *
 * Opt-in via the "-W" boot argument; off by default so normal runs and KVM
 * are unperturbed.  Diagnostic infrastructure for #344; keep.
 */

#include <cpus.h>
#include <kern/misc_protos.h>
#include <kern/cpu_number.h>
#include <mach/vm_param.h>
#include <i386/proc_reg.h>
#include <mach/i386/thread_status.h>	/* struct i386_saved_state */

#if	NCPUS > 1
#include <i386/lapic.h>
#include <i386/apic.h>
#endif	/* NCPUS > 1 */

extern void cnputc(char);
extern int  db_active;		/* nonzero while a CPU is stopped in DDB */

/*
 * heartbeat — bumped by the clock tick (hardclock / lapic_timer_handler),
 * which only runs at IF=1.  Stalls the instant the system wedges at IF=0.
 * Defined unconditionally so the tick paths can bump it without #if soup.
 */
volatile unsigned int nmi_heartbeat = 0;

/*
 * #355: PER-CPU tick counter, bumped by each CPU's OWN timer tick (which only
 * runs at IF=1).  The global nmi_heartbeat above stays alive as long as ANY
 * cpu ticks, so it only catches a total clock-stop; a PARTIAL wedge (one cpu
 * spinning at IF=0 while the rest idle-tick) never trips it.  Indexing this
 * per-cpu lets the NMI detect exactly the wedged core.
 */
volatile unsigned int nmi_cpu_tick[NCPUS] = { 0 };

/*
 * Per-CPU dedicated stacks for the NMI handler (#344).  A user-mode NMI makes
 * the CPU load ktss.esp0, which on this kernel points into the interrupted
 * thread's INLINE-pcb saved-state area (act+0x58, see act_machine_switch_pcb in
 * pcb.c) -- NOT a real kernel stack.  alltraps switches to thread->kernel_stack
 * after saving registers; the t_nmi stub (deliberately) does not, so running the
 * C watchdog there underflows the tiny saved-state slot down through act+0
 * (thr_acts.next) and the pcb segment/LDT state, corrupting the current thread's
 * activation -- the deterministic omen task-corruption.  t_nmi switches to this
 * CPU's slot (indexed by CPU_NUMBER) before calling nmi_watchdog(): one stack per
 * CPU so concurrent NMIs on an SMP box never share a stack.  HW blocks NMI
 * re-entry on a CPU until iret, so a single slot per CPU suffices.
 *
 * NB: the t_nmi stub computes the slot top as nmi_stacks + (cpu+1)<<12, so
 * NMI_STACK_SIZE is locked to 4096 -- the _Static_assert guards against drift.
 */
#define NMI_STACK_SIZE	4096
_Static_assert(NMI_STACK_SIZE == 4096,
	       "t_nmi in locore.S hardcodes shll $12 for the per-CPU slot index");
unsigned char nmi_stacks[NCPUS][NMI_STACK_SIZE] __attribute__((aligned(16)));

/*
 * Set by the "-W" boot argument.  parse_arguments() runs BEFORE the BSS is
 * zeroed (see model_dep.c mem_size / cons_is_com1), so a .bss flag set there
 * gets wiped back to 0 before machine_init reads it — force it into .data.
 */
int nmi_watchdog_enabled __attribute__((section(".data"))) = 0;

/*
 * #382: DDB CPU-park flag.  kdb_trap sets it and broadcasts an NMI to the
 * other CPUs; each one spins here (inside its NMI handler, ISR/EOI state
 * untouched) until the DDB session continues.  Without this, a DDB
 * session on live SMP deadlocks the box: the running CPUs eventually
 * issue a TLB-shootdown IPI, wait forever for the DDB'd CPU's ack while
 * sitting in their own IPI handler pre-EOI (in-service 0xF1), and with
 * PPR stuck at 0xF0 nothing below it — device IRQs included — is ever
 * delivered again.  Defined unconditionally so kdb_trap links on any
 * config.
 */
volatile int ddb_nmi_park = 0;

#if	NCPUS > 1

/* Intel architectural performance-monitoring MSRs. */
#define MSR_IA32_PERFEVTSEL0	0x186
#define MSR_IA32_PMC0		0x0C1

/* IA32_PERFEVTSELx fields. */
#define PES_EVENT_UNHALTED_CYC	0x3C	/* event 0x3C, umask 0x00 */
#define PES_USR			(1u << 16)
#define PES_OS			(1u << 17)
#define PES_INT			(1u << 20)	/* PMI (interrupt) on overflow */
#define PES_EN			(1u << 22)

/* LAPIC performance-counter LVT register (absent from apic.h). */
#define LAPIC_LVT_PERFCNT	0x00000340

/* Cycles between NMIs.  omen is ~2.8 GHz, so ~2.8e8 cycles ~= 100 ms. */
#define WD_RELOAD_CYCLES	280000000ULL

/* No clock tick across this many NMIs (~1 s at ~100 ms/NMI) ==> wedged. */
#define WD_STALE_LIMIT		10

static unsigned int	wd_last_tick[NCPUS];	/* #355: last per-cpu tick seen */
static unsigned int	wd_stale[NCPUS];
static int		wd_fired[NCPUS];
static int		wd_seen[NCPUS];	/* clock has ticked at least once */

static void
wd_puts(const char *s)
{
	while (*s)
		cnputc(*s++);
}

static void
wd_hex(unsigned int v)
{
	int i;

	cnputc('0');
	cnputc('x');
	for (i = 28; i >= 0; i -= 4)
		cnputc("0123456789abcdef"[(v >> i) & 0xF]);
}

/*
 * Reload PMC0 so it counts WD_RELOAD_CYCLES more unhalted cycles, then
 * overflows (delivering the next PMI/NMI).  The PMC is 48 bits wide; load
 * (2^48 - N) so the overflow lands after N cycles.  Both terms are compile
 * time constants, so no 64-bit division is emitted.
 */
static void
wd_arm_pmc(void)
{
	unsigned long long v = (1ULL << 48) - WD_RELOAD_CYCLES;

	wrmsr(MSR_IA32_PMC0,
	      (unsigned int)(v & 0xFFFFFFFFULL),
	      (unsigned int)((v >> 32) & 0xFFFFULL));
}

void
nmi_watchdog_init(void)
{
	int cpu;

	if (!nmi_watchdog_enabled)
		return;
	if (lapic_start == 0)
		return;

	cpu = cpu_number();
	wd_last_tick[cpu] = nmi_cpu_tick[cpu];
	wd_stale[cpu] = 0;
	wd_fired[cpu] = 0;
	wd_seen[cpu] = 0;

	/* Route the LAPIC perf-counter LVT to NMI delivery, unmasked. */
	LAPIC_REG32(LAPIC_LVT_PERFCNT) = LAPIC_LVT_DM_NMI;

	/* Arm the counter, then enable it with PMI-on-overflow. */
	wd_arm_pmc();
	wrmsr(MSR_IA32_PERFEVTSEL0,
	      PES_EVENT_UNHALTED_CYC | PES_USR | PES_OS | PES_INT | PES_EN, 0);

	printf("nmi_watchdog: armed on cpu %d (perfcnt LVT -> NMI, ~%u Mcyc/NMI)\n",
	       cpu, (unsigned int)(WD_RELOAD_CYCLES / 1000000ULL));
}

void
nmi_watchdog(struct i386_saved_state *regs)
{
	int cpu;
	unsigned int hb;
	unsigned int ebp;
	int i;

	/*
	 * #382: DDB CPU-park.  Checked before anything else (including the
	 * -W watchdog logic) so a DDB session freezes the whole box no
	 * matter how this NMI was produced.  Spinning here keeps the CPU's
	 * interrupted context — mid-IPI-handler included — perfectly
	 * intact; on release it resumes where it was.
	 *
	 * While parked, step out of the TLB-shootdown protocol the same
	 * way an idle CPU does (MARK_CPU_IDLE): drop out of cpus_active so
	 * a PMAP_UPDATE_TLBS initiator does not spin forever on our ack
	 * (pmap_tlb_ack_outstanding re-reads cpus_active every pass and
	 * lets us fall out of its wait set).  On release, mirror
	 * MARK_CPU_ACTIVE lock-free: leave cpus_idle, flush + ack via
	 * pmap_tlb_shootdown_handler ("lock-free, safe at any spl"), then
	 * rejoin cpus_active — any update we missed while parked is
	 * covered by that unconditional full flush.
	 */
	if (ddb_nmi_park) {
		/* cpu_set == volatile unsigned long (intel/pmap.h) */
		extern volatile unsigned long cpus_active, cpus_idle;
		extern void pmap_tlb_shootdown_handler(void);
		unsigned long me = 1ul << cpu_number();

		__sync_fetch_and_or(&cpus_idle, me);
		__sync_fetch_and_and(&cpus_active, ~me);
		while (ddb_nmi_park)
			__asm__ __volatile__("pause" : : : "memory");
		__sync_fetch_and_and(&cpus_idle, ~me);
		pmap_tlb_shootdown_handler();
		__sync_fetch_and_or(&cpus_active, me);
		return;
	}

	/*
	 * #382: emergency DDB door.  With -K armed but the perf-counter
	 * watchdog OFF (-W absent), the only NMI source is an external
	 * injection — QEMU's monitor "nmi" command (or a bare-metal NMI
	 * button).  Turn it into a debugger break: this works even when
	 * char_server is dead and every userspace break path with it,
	 * which is exactly when the debugger is needed most.  The monitor
	 * NMIs every CPU at once; only the first one through enters DDB,
	 * the rest return to what they were doing.
	 */
	if (!nmi_watchdog_enabled) {
		extern int ddb_kbd_break_enabled;	/* -K (model_dep.c) */
		extern void Debugger(const char *message);
		static volatile int ddb_nmi_in;

		if (ddb_kbd_break_enabled &&
		    !__sync_lock_test_and_set(&ddb_nmi_in, 1)) {
			Debugger("NMI break");
			__sync_lock_release(&ddb_nmi_in);
		}
		return;
	}

	if (lapic_start == 0)
		return;

	cpu = cpu_number();

	/*
	 * Re-arm first so we keep getting NMIs.  On overflow the LVT sets its
	 * mask bit; rewriting it (NMI delivery, mask clear) unmasks it.
	 */
	wd_arm_pmc();
	LAPIC_REG32(LAPIC_LVT_PERFCNT) = LAPIC_LVT_DM_NMI;

	/*
	 * In DDB the clock is legitimately stopped (IF=0, interactive).  That is
	 * not a wedge -- don't print the misleading "halted; power-cycle" banner
	 * over a live debugger session.  Treat it as a heartbeat so the stale
	 * counter resets the moment we resume.
	 */
	if (db_active) {
		wd_stale[cpu] = 0;
		return;
	}

	hb = nmi_cpu_tick[cpu];			/* #355: THIS cpu's own tick */
	if (hb != wd_last_tick[cpu]) {		/* this cpu still ticking: healthy */
		wd_last_tick[cpu] = hb;
		wd_stale[cpu] = 0;
		wd_seen[cpu] = 1;
		return;
	}
	if (!wd_seen[cpu])			/* clock not started yet: wait */
		return;
	if (++wd_stale[cpu] < WD_STALE_LIMIT)
		return;
	if (wd_fired[cpu])			/* report once per CPU */
		return;
	wd_fired[cpu] = 1;

	wd_puts("\n*** NMI WATCHDOG: cpu ");
	wd_hex((unsigned int)cpu);
	wd_puts(" wedged (no timer tick ~1s: IF=0 spin/deadlock) ***\n eip=");
	wd_hex(regs->eip);
	wd_puts(" cs=");
	wd_hex(regs->cs);
	wd_puts(" efl=");
	wd_hex(regs->efl);
	wd_puts("\n eax=");
	wd_hex(regs->eax);
	wd_puts(" ebx=");
	wd_hex(regs->ebx);
	wd_puts(" ecx=");
	wd_hex(regs->ecx);
	wd_puts(" edx=");
	wd_hex(regs->edx);
	wd_puts("\n esi=");
	wd_hex(regs->esi);
	wd_puts(" edi=");
	wd_hex(regs->edi);
	wd_puts(" ebp=");
	wd_hex(regs->ebp);
	wd_puts(" esp=");
	wd_hex(regs->uesp);
	wd_puts("\n backtrace:");

	/*
	 * Walk the kernel %ebp frame chain directly (NMI hit kernel code at
	 * IF=0).  Frames live in kernel VAs and ascend; stop otherwise.
	 */
	ebp = regs->ebp;
	for (i = 0; i < 24 && ebp >= VM_MIN_KERNEL_ADDRESS; i++) {
		unsigned int *fr = (unsigned int *)ebp;
		unsigned int ret = fr[1];
		unsigned int next = fr[0];

		cnputc(' ');
		wd_hex(ret);
		if (next <= ebp)		/* not ascending: bail */
			break;
		ebp = next;
	}
	wd_puts("\n*** halted; power-cycle to retry ***\n");
}

#else	/* NCPUS > 1 */

/* UP build: no LAPIC perf-counter path.  Provide the symbols as no-ops. */
void nmi_watchdog_init(void) { }
void nmi_watchdog(struct i386_saved_state *regs) { (void)regs; }

#endif	/* NCPUS > 1 */
