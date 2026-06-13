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
 * mp_stub.c — minimal symbols required to link the kernel with NCPUS > 1
 * before the real AP-boot machinery (#300 Incrementi 3-4) lands.
 *
 * The historical mp_v1_1.c is intentionally kept out of the build: it carries
 * 1230 lines of Corollary/SystemPro-specific interrupt plumbing that was
 * never exercised in this tree.  It will be replaced by a modern MPS 1.4
 * parser in Increment 3.  Until then, this file provides the externs the
 * rest of the kernel expects.  The runtime behaviour is "single CPU": no AP
 * is woken, cpu_start() refuses, mp_probe_cpus() reports a single processor.
 */

#include <types.h>
#include <mach/machine.h>
#include <mach/kern_return.h>
#include <kern/processor.h>
#include <kern/misc_protos.h>
#include <kern/cpu_data.h>	/* current_cpu_id() (#301) */
#include <vm/vm_kern.h>		/* kmem_alloc (#308) */
#include <i386/cpu_number.h>	/* cpu_number() (#304) */
#include <i386/lapic.h>		/* lapic_enable / lapic_send_ipi (#302) */
#include <i386/mp_desc.h>	/* mp_desc_table / interrupt_stack (#308) */
#include <i386/pic.h>		/* NINTR */
#include <i386/ipl.h>		/* SPLHI */
#include <chips/busses.h>	/* intr_t */

#define	AP_INTSTACK_BYTES	4096	/* INTSTACK_SIZE; locore.S sets this in assym */

#define NSPL	(SPLHI + 1)

/* Local APIC bookkeeping.
 *
 * cpu_number() in <i386/cpu_number.h> reads `lapic_id` to extract its CPU
 * index from the local APIC ID register, so the symbol must exist even
 * before the LAPIC machinery is wired up.  We point it at a benign zeroed
 * word in low memory; reads return CPU 0, which is correct while only the
 * BSP is alive. */
unsigned int	lapic_id_initdata = 0;
int		lapic_id = (int)&lapic_id_initdata;
vm_offset_t	lapic_start;

/* cpu_int_word[NCPUS], real_ncpus and wncpu are already defined by mp.c;
 * cpu_start(), cpu_control() and mp_probe_cpus() likewise live there.
 *
 * get_ncpus() and validate_cpus() live in mp_table.c (Increment 3): they
 * walk the MPS 1.4 FPS/PCMP to enumerate real processors instead of always
 * answering 1. */

/*
 * cpu_interrupt() — send the unified MP IPI to `cpu`.  Producers in mp.c
 * (interrupt_processor / cause_ast_check / slave_clock / remote_kdb) first
 * set their reason bit in cpu_int_word[cpu] (MP_TLB_FLUSH / MP_AST /
 * MP_CLOCK / MP_KDB), then call here; ipi_mp_handler() on the target drains
 * every pending bit and dispatches it.
 *
 * #316: route through LAPIC IPI_VECTOR_MP (was IPI_VECTOR_TLB_SHOOT, which
 * only ever ran the TLB-shootdown handler — so MP_AST / MP_CLOCK / MP_KDB
 * were silently dropped and cross-CPU reschedule never happened).  Sending
 * to ourselves is a no-op — process_pmap_updates is called inline by the
 * initiator once it owns the pmap lock, so a self-IPI would deadlock on
 * splvm.  Sending to a CPU that the MP table never reported is silently
 * dropped by lapic_send_ipi (mp_cpu_lapic_id_get returns 0xFF).
 */
void
cpu_interrupt(int cpu)
{
	if (cpu == cpu_number())
		return;
	lapic_send_ipi(cpu, IPI_VECTOR_MP);
}

/*
 * start_other_cpus() — called from kern/startup.c on the BSP after the
 * scheduler is up.  Walks every slot the MP table identified as a CPU,
 * skips the BSP itself, and asks cpu_start() (in mp.c) to bring each AP
 * online via the INIT/SIPI/SIPI sequence.
 *
 * #300 Increment 4.  Errors are logged but not fatal: a failed AP just
 * stays offline; the rest of the system keeps running on the CPUs that
 * did make it.
 */
extern int master_cpu;
extern int real_ncpus;
extern kern_return_t cpu_start(int slot);
extern void master_up(void);
extern vm_offset_t interrupt_stack[NCPUS];
extern vm_offset_t int_stack_top[NCPUS];

/*
 * #299 (4-CPU stability): two-phase bring-up barrier.  Each AP completes
 * its CPU-state init (CR4/FPU, LAPIC enable, FPU sanity) and reports
 * machine_slot[].running = TRUE so cpu_start()'s 1 s poll succeeds, then
 * parks here with interrupts still disabled until the BSP has finished
 * the whole start_other_cpus() loop and flips ap_can_run.  This closes
 * the "start a new AP while a previous AP is already running the
 * scheduler + arming its periodic LAPIC timer" window: APs only arm
 * their timer / enter the scheduler once *every* CPU is past bring-up.
 */
volatile boolean_t ap_can_run = FALSE;

/*
 * #308: allocate the per-AP kernel resources that interrupt_stack_alloc()
 * would have set up if mp_probe_cpus() had known the real CPU count.
 * With ACPI MADT detection deferred to phase 2 (#300), phase 1 sees a
 * conservative count of 1, so interrupt_stack_alloc() only sets up
 * slot 0 (the BSP) — leaving interrupt_stack[i>0] and mp_desc_table[i>0]
 * as zero.  svstart in start.S then uses interrupt_stack[lapic_id] as
 * the AP's kernel stack and mp_desc_init() dereferences mp_desc_table[i]
 * to copy in the per-CPU IDT/GDT/LDT; both crash hard if those slots
 * are NULL.  Lazily kmem_alloc() them here, on the BSP, right before
 * each cpu_start().
 */
static kern_return_t
ap_alloc_resources(int slot)
{
	vm_offset_t stack;
	vm_offset_t mpt;

	if (interrupt_stack[slot] == 0) {
		if (kmem_alloc(kernel_map, &stack, AP_INTSTACK_BYTES)
		    != KERN_SUCCESS)
			return KERN_RESOURCE_SHORTAGE;
		interrupt_stack[slot] = stack;
		int_stack_top[slot]   = stack + AP_INTSTACK_BYTES;
	}
	if (mp_desc_table[slot] == 0) {
		if (kmem_alloc(kernel_map, &mpt,
			       sizeof(struct mp_desc_table)) != KERN_SUCCESS)
			return KERN_RESOURCE_SHORTAGE;
		mp_desc_table[slot] = (struct mp_desc_table *)mpt;
	}
	return KERN_SUCCESS;
}

void
start_other_cpus(void)
{
	int slot;

	/* #301: BSP self-reports its per-CPU identity via %gs. */
	printf("smp: BSP cpu_id=%d via %%gs\n", current_cpu_id());

	/* #312: calibrate the LAPIC timer once, here on the BSP while it is the
	 * sole reader of the shared 8254 (no AP is running yet).  APs reuse the
	 * result when they arm their per-CPU periodic timer. */
	lapic_timer_calibrate();

	/* #302: self-IPI round-trip on the BSP. */
	lapic_send_ipi(master_cpu, IPI_VECTOR_CALL_FUNC);

	/* #308: release start_lock so the APs, which are currently
	 * spinning on it in pstart, can make forward progress.  The
	 * historical mp_v1_1.c::start_other_cpus did this as its first
	 * step; this mp_stub.c variant forgot to. */
	master_up();

	for (slot = 0; slot < real_ncpus; slot++) {
		if (slot == master_cpu)
			continue;
		if (ap_alloc_resources(slot) != KERN_SUCCESS) {
			printf("start_other_cpus: AP slot %d kmem_alloc failed\n",
			       slot);
			continue;
		}
		if (cpu_start(slot) != KERN_SUCCESS) {
			printf("start_other_cpus: AP slot %d did not come up\n",
			       slot);
			continue;
		}
		printf("smp: AP cpu_id=%d online (reported by BSP)\n", slot);
	}

	/* #299: every AP is past CPU-state init and parked at the barrier in
	 * slave_machine_init().  Release them together so none arms its LAPIC
	 * timer / enters the scheduler while another AP is still being
	 * brought up. */
	ap_can_run = TRUE;
}

/*
 * #309: bring the AP's CR0/CR4/FPU state up to par with what
 * machine_startup()/i386_init() did on the BSP.  Without this the
 * first SSE/AVX instruction in a userspace thread that the scheduler
 * lands on the AP takes #UD because CR4.OSFXSR is clear.
 *
 * Mirrors the BSP order from i386/AT386/model_dep.c:
 *   - i386_init() sets CR4.PGE after pmap_bootstrap.
 *   - machine_init() calls init_fpu() which sets CR0/CR4 FPU bits,
 *     CR4.OSXSAVE if XSAVE is available, and writes XCR0.
 *
 * sysenter_ap_init() is already called from mp_desc_init().  The
 * sha256-NI dispatch table is global (set once on the BSP) and the
 * IDT/GDT/LDT/TSS were already loaded in svstart, so this is the
 * full per-CPU CPU-state init the AP needs before being handed to
 * the scheduler.
 */
extern void init_fpu(void);
extern unsigned int get_cr4(void);
extern void set_cr4(unsigned int);
#define	CR4_PGE	0x80	/* enable global PTEs */

static void
ap_machine_init(void)
{
	set_cr4(get_cr4() | CR4_PGE);
	init_fpu();
}

/*
 * slave_machine_init() — entry point each AP jumps to once protected mode
 * and the kernel page tables are live.
 */
void
slave_machine_init(void)
{
	int my_cpu = current_cpu_id();

	/* Tell the BSP this AP is alive FIRST — cpu_start()'s 1 s busy-
	 * poll can otherwise time out before ap_machine_init's printfs
	 * (FPU detect / XSAVE enable) finish draining through the polled
	 * COM1 driver.  Setting the flag is just one store; nothing else
	 * on this CPU depends on the AP being further along yet. */
	machine_slot[my_cpu].running = TRUE;

	/* #309: get this AP's CR0/CR4/FPU state in sync with the BSP. */
	ap_machine_init();

	/* #302: enable this AP's local APIC.  Without this, IPIs and
	 * eventually LAPIC timer interrupts are silently dropped. */
	lapic_enable();

	/* #309 acceptance (AP arm): same SSE2 + x87 sanity check the
	 * BSP runs from setup_main.  Will #UD here if ap_machine_init
	 * didn't program CR4 / FPU correctly. */
	{
		extern void fpu_sanity_check(void);
		fpu_sanity_check();
	}

	/* #299: park here until the BSP has brought up *all* APs.  CPU-state
	 * init is done and machine_slot[].running is already TRUE, so the
	 * BSP's cpu_start() poll has succeeded for this slot; we just must
	 * not arm the periodic timer / enter the scheduler until every other
	 * AP is also past bring-up (see start_other_cpus). */
	while (!ap_can_run)
		__asm__ __volatile__("pause" ::: "memory");

	/* #312: arm this AP's local periodic LAPIC timer (calibrated on the
	 * BSP in start_other_cpus).  From now this CPU clocks hertz_tick()
	 * itself instead of waiting on the cross-CPU MP_CLOCK IPI; the tick is
	 * TPR-gated (vector 0x3f, class 3) so the first one only lands once the
	 * AP drops to spllo in the scheduler, with a valid current thread. */
	lapic_timer_start();
}

/*
 * mp_v1_1_io_lock() / mp_v1_1_io_unlock() — historical big-MP IO barrier
 * used by net_device.c et al.  Stub returns "lock acquired" so existing
 * call sites proceed; with no APs the barrier is a no-op.  The whole
 * mechanism gets reviewed/replaced by #303 (spinlock audit).
 */
boolean_t
mp_v1_1_io_lock(int why, struct processor **saved_processor_p)
{
	(void)why;
	if (saved_processor_p)
		*saved_processor_p = (struct processor *)0;
	return TRUE;
}

void
mp_v1_1_io_unlock(struct processor *p)
{
	(void)p;
}

/* mp_v1_1_init() lives in mp_table.c (Increment 3); the global
 * mp_v1_1_initialized flag is still owned by this file because mp.c and
 * a couple of legacy callers check it directly. */
boolean_t	mp_v1_1_initialized = FALSE;

/*
 * mp_v1_1_take_irq() / mp_v1_1_reset_irq() — would install/remove a per-CPU
 * IRQ handler in the historical mp_v1_1 vector tables.  Since mp_v1_1.c is
 * not built, these stubs decline so take_irq()/reset_irq() fall through to
 * the shared ivect[]/intpri[] path (and, under the I/O APIC, the RTE
 * unmask in autoconf.c).
 *
 * #311: they MUST return FALSE.  take_irq() runs its real body only when
 * `!mp_v1_1_take_irq()` is true; returning TRUE here turned take_irq() into
 * a no-op, so ivect[]/intpri[] were never populated and no RTE was ever
 * unmasked — device IRQs limped along on driver polling while the clock
 * (hardclock on IRQ0, which cannot poll) never ticked, hanging every
 * MACH_RCV_TIMEOUT on SMP.
 */
boolean_t
mp_v1_1_take_irq(int pic, int unit, int spl, void *intr)
{
	(void)pic;
	(void)unit;
	(void)spl;
	(void)intr;
	return FALSE;
}

boolean_t
mp_v1_1_reset_irq(int pic, int *unit, int *spl, void *intr)
{
	(void)pic;
	(void)unit;
	(void)spl;
	(void)intr;
	return FALSE;
}

/*
 * mp_v1_1_intr[] / mp_v1_1_unit[] — per-IRQ handler/argument tables that
 * i386/interrupt.S dispatches through ((,%ecx,4)).  Sized like the
 * historical mp_v1_1.c so the asm indexing stays in-bounds; left zeroed so
 * any vector taken into them is rejected by the dispatcher.
 */
intr_t	mp_v1_1_intr[NSPL * NINTR];
int	mp_v1_1_unit[NSPL * NINTR];

/*
 * kdb_console() — switch DDB I/O to a shared console when running on a
 * second CPU.  Stub: no-op (BSP keeps the same console it had on UP).
 */
void
kdb_console(void)
{
}
