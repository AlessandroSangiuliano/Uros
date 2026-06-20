/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */
/* CMU_HIST */
/*
 * Revision 2.1.3.3  92/09/15  17:15:38  jeffreyh
 * 	Do not propagate clock int to all CPUs at the same time
 * 	[92/07/24            bernadat]
 * 
 * Revision 2.1.3.2  92/06/24  17:59:32  jeffreyh
 * 	Fix to be able to compile STD+COMPAQ (Single cpu)
 * 	[92/06/09            bernadat]
 * 
 * Revision 2.1.3.1  92/04/30  11:58:19  bernadat
 * 	MP code for AT386 platforms (Corollary, SystemPro)
 * 	[92/04/08            bernadat]
 * 
 */
/* CMU_ENDHIST */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */

/*
 */

#include <cpus.h>
#include <mp_v1_1.h>

#if NCPUS > 1

#include <types.h>
#include <mach/machine.h>
#include <kern/lock.h>
#include <kern/processor.h>
#include <kern/misc_protos.h>
#include <kern/machine.h>
#include <i386/db_machdep.h>
#include <ddb/db_run.h>
#include <machine/AT386/mp/mp.h>
#include <machine/AT386/mp/boot.h>		/* #300: MP_BOOT, MP_MACH_START */
#include <i386/apic.h>				/* #300: LAPIC_ICR / LAPIC_ICRD */
#include <i386/setjmp.h>
#include <i386/misc_protos.h>
#include <i386/spl.h>				/* #302: splhi/splx for TSC calib */
#include <i386/pit.h>				/* #302: 8254 ports for TSC calib */
#include <i386/pio.h>				/* #302: inb/outb */

int	cpu_int_word[NCPUS];

extern void cpu_interrupt(int cpu);
extern int get_ncpus(void);
extern int lapic_timer_enabled;		/* #312: per-CPU LAPIC timer active (lapic.c) */

/*
 * Generate a clock interrupt on next running cpu
 *
 * Instead of having the master processor interrupt
 * all active processors, each processor in turn interrupts
 * the next active one. This avoids all slave processors
 * accessing the same R/W data simultaneously.
 */

void
slave_clock(void)
{
	register int cpu;

	/*
	 * #312: once the per-CPU LAPIC timers are calibrated and armed, every
	 * AP clocks hertz_tick() from its own local timer (lapic_timer_handler),
	 * so there is nothing to forward.  Skipping the MP_CLOCK IPI here is what
	 * removes the cross-CPU clock interrupt that fired above splsched and
	 * deadlocked a lock holder during a TLB shootdown (#317).
	 */
	if (lapic_timer_enabled)
		return;

	mp_disable_preemption();
	for (cpu=cpu_number()+1; cpu<NCPUS; cpu++)
               	if ( machine_slot[cpu].running == TRUE) {
			i_bit_set(MP_CLOCK, &cpu_int_word[cpu]);
			cpu_interrupt(cpu);
			mp_enable_preemption();
		  	return;
		}
	mp_enable_preemption();
}

void
interrupt_processor(
	int		cpu)
{
	i_bit_set(MP_TLB_FLUSH, &cpu_int_word[cpu]);
	cpu_interrupt(cpu);
}

/*ARGSUSED*/
void
init_ast_check(
	processor_t	processor)
{
}

void
cause_ast_check(
	processor_t	processor)
{
	int cpu = processor->slot_num;

	i_bit_set(MP_AST, &cpu_int_word[cpu]);
	cpu_interrupt(cpu);
}

/*
 * cpu_start() — bring application processor `slot_num` out of reset and
 * into the kernel via the INIT/SIPI/SIPI sequence on the local APIC.
 *
 * Sequence per Intel SDM Vol. 3 §8.4:
 *   1. Drop the slave_boot.S trampoline at phys MP_BOOT (0x1000) and
 *      stash the entry point — &pstart converted to physical — at the
 *      well-known slot MP_MACH_START (0x800) that slave_pstart reads.
 *   2. Send INIT IPI (level-assert) targeted at the AP's LAPIC ID.
 *   3. Wait ~10 ms.
 *   4. Send STARTUP IPI with vector = MP_BOOT >> 12.
 *   5. Wait ~200 µs.
 *   6. Send STARTUP IPI a second time (spec recommendation; modern CPUs
 *      ignore the second one but it makes us robust on older steppings).
 *   7. Poll machine_slot[slot_num].running for up to ~1 s — the AP marks
 *      itself running once it reaches cpu_up() via slave_main().
 *
 * #300 Increment 4.  No I/O APIC / PIC interaction here yet (Phase C).
 */

extern char	slave_boot_base[];	/* AP real-mode trampoline */
extern char	slave_boot_end[];
extern void	pstart(void);		/* shared BSP/AP entry in start.S */
extern vm_offset_t	lapic_start;	/* LAPIC virtual base, set by mp_table.c */
extern unsigned char	mp_cpu_lapic_id_get(int slot);	/* mp_table.c */
extern unsigned char	mp_bsp_lapic_id_get(void);	/* mp_table.c */

#define MP_BOOT_VA	((vm_offset_t)MP_BOOT     + 0xC0000000U)
#define MP_MACH_START_VA ((vm_offset_t)MP_MACH_START + 0xC0000000U)
#define KV_TO_PA(x)	((unsigned int)(x) - 0xC0000000U)

#define LAPIC_REG32(off)	(*(volatile unsigned int *)(lapic_start + (off)))

/*
 * #302: TSC-calibrated micro-delay for the INIT/SIPI/SIPI dance and the
 * AP come-online poll.  The historical placeholder was a fixed-count
 * `volatile` busy loop calibrated by hand ("~200 iterations/µs"); that
 * estimate is host-dependent — on a fast core (e.g. Ryzen) the loop runs
 * far quicker than assumed, so the real wall-clock delays collapse below
 * the spec minima and APs miss their bring-up window (only one AP comes
 * online, nondeterministically).  We instead time the TSC against the
 * 8254 PIT once on the BSP, exactly like lapic_timer_calibrate(), and
 * spin on rdtsc for a genuine wall-clock interval.  Only the BSP's local
 * TSC is read for elapsed time, so none of #318's cross-core skew /
 * invariance concerns apply here.
 */
extern unsigned int	clknum;		/* rtclock.c: 8254 counts per second */
extern unsigned int	clks_per_int;	/* rtclock.c: 8254 counts per HZ tick */

unsigned int	mp_tsc_per_us;		/* 0 = not yet calibrated (fallback) */

/* Latch + read 8254 counter 0; valid only with the clock IRQ masked. */
#define	MP_READ_8254(v)	{					\
	outb(PITCTL_PORT, PIT_C0);				\
	(v)  = inb(PITCTR0_PORT);				\
	(v) |= inb(PITCTR0_PORT) << 8; }

static __inline__ unsigned long long
mp_rdtsc(void)
{
	unsigned int lo, hi;
	__asm__ __volatile__("rdtsc" : "=a" (lo), "=d" (hi));
	return ((unsigned long long)hi << 32) | lo;
}

/* 64-bit / 32-bit -> 32-bit via a single divl (no libgcc __udivdi3). */
static __inline__ unsigned int
mp_div64_32(unsigned long long num, unsigned int den)
{
	unsigned int q, r;

	__asm__("divl %4"
		: "=a" (q), "=d" (r)
		: "a" ((unsigned int)num), "d" ((unsigned int)(num >> 32)),
		  "rm" (den));
	return q;
}

/*
 * Calibrate mp_tsc_per_us by counting TSC ticks across a ~8 ms 8254 window.
 * Called once on the BSP from start_other_cpus(), before any AP runs and
 * while the BSP is the sole reader of the shared 8254 (no latch race).
 */
void
mp_tsc_calibrate(void)
{
	unsigned int		c0, c1, pit_counts, window, guard, elapsed_us;
	unsigned long long	t0, t1;
	spl_t			s;

	if (mp_tsc_per_us != 0)
		return;			/* once per boot */

	window = clknum / 125;		/* ~8 ms of 8254 counts, < one tick */

	s = splhi();			/* mask the clock IRQ: sole 8254 reader */

	/* Start near the top of an 8254 period so the window cannot wrap. */
	guard = 100000000u;
	do { MP_READ_8254(c0); }
	while (c0 < clks_per_int - clks_per_int / 32 && --guard);

	t0 = mp_rdtsc();
	guard = 100000000u;
	do { MP_READ_8254(c1); }
	while (c1 <= c0 && (c0 - c1) < window && --guard);
	t1 = mp_rdtsc();

	splx(s);

	pit_counts = (c1 <= c0) ? (c0 - c1) : 1;	/* 8254 counts elapsed */
	if (pit_counts == 0)
		pit_counts = 1;

	/* elapsed_us = pit_counts / (clknum / 1e6); done as 64/32 to keep
	 * precision and avoid overflow at any plausible TSC frequency. */
	elapsed_us = mp_div64_32((unsigned long long)pit_counts * 1000000u, clknum);
	if (elapsed_us == 0)
		elapsed_us = 1;

	mp_tsc_per_us = mp_div64_32(t1 - t0, elapsed_us);
	if (mp_tsc_per_us == 0)
		mp_tsc_per_us = 1;

	/* NB: the kernel printf has no %llu, so report only 32-bit values. */
	printf("mp: TSC calibrated -> %u TSC/us (~%u MHz) over %u us 8254 window\n",
	       mp_tsc_per_us, mp_tsc_per_us, elapsed_us);
}

/*
 * Busy-wait `us` microseconds.  Uses the calibrated TSC for a true
 * wall-clock delay; before calibration (or if it failed) falls back to
 * the historical fixed-count loop so the early/UP paths still work.
 */
static void
mp_busy_delay_us(unsigned int us)
{
	if (mp_tsc_per_us != 0) {
		unsigned long long deadline =
			mp_rdtsc() + (unsigned long long)us * mp_tsc_per_us;
		while (mp_rdtsc() < deadline)
			__asm__ __volatile__("pause");
		return;
	}

	{
		volatile unsigned int spin;
		unsigned int total = us * 200u;

		for (spin = 0; spin < total; spin++)
			;
	}
}

static void
mp_busy_delay_ms(unsigned int ms)
{
	while (ms-- > 0)
		mp_busy_delay_us(1000);
}

static void
mp_lapic_ipi_wait(void)
{
	while (LAPIC_REG32(LAPIC_ICR) & LAPIC_ICR_DS_PENDING)
		;
}

static void
mp_lapic_send_init(unsigned int dest_lapic_id)
{
	LAPIC_REG32(LAPIC_ICRD) =
	    (dest_lapic_id & 0xFFu) << LAPIC_ICRD_DEST_SHIFT;
	LAPIC_REG32(LAPIC_ICR)  =
	    LAPIC_ICR_DM_INIT | LAPIC_ICR_LEVEL_ASSERT;
	mp_lapic_ipi_wait();
}

static void
mp_lapic_send_sipi(unsigned int dest_lapic_id, unsigned int vector)
{
	LAPIC_REG32(LAPIC_ICRD) =
	    (dest_lapic_id & 0xFFu) << LAPIC_ICRD_DEST_SHIFT;
	LAPIC_REG32(LAPIC_ICR)  =
	    LAPIC_ICR_DM_STARTUP | (vector & LAPIC_ICR_VECTOR_MASK);
	mp_lapic_ipi_wait();
}

kern_return_t
cpu_start(int slot_num)
{
	unsigned char	dest_lapic;
	unsigned int	pstart_pa;
	vm_size_t	trampoline_size;
	int		wait_ms;

	if (lapic_start == 0) {
		printf("cpu_start(%d): LAPIC not mapped, refusing\n", slot_num);
		return KERN_FAILURE;
	}

	dest_lapic = mp_cpu_lapic_id_get(slot_num);
	if (dest_lapic == 0xFF) {
		printf("cpu_start(%d): no LAPIC ID known\n", slot_num);
		return KERN_FAILURE;
	}
	if (dest_lapic == mp_bsp_lapic_id_get())
		return KERN_SUCCESS;	/* the BSP is already running */

	/*
	 * Copy the trampoline to its rendezvous address and seed the
	 * entry-point slot.  Both addresses are in identity-mapped low
	 * memory reachable through phystokv().
	 */
	trampoline_size = (vm_size_t)(slave_boot_end - slave_boot_base);
	memcpy((void *)MP_BOOT_VA, slave_boot_base, trampoline_size);

	pstart_pa = KV_TO_PA(pstart);
	*(volatile unsigned int *)MP_MACH_START_VA = pstart_pa;

	printf("cpu_start: slot=%d lapic=%d trampoline=%lu B entry=0x%x\n",
	       slot_num, dest_lapic, (unsigned long)trampoline_size,
	       pstart_pa);

	/*
	 * INIT/SIPI/SIPI dance.
	 */
	mp_lapic_send_init(dest_lapic);
	mp_busy_delay_ms(10);

	mp_lapic_send_sipi(dest_lapic, MP_BOOT >> 12);
	mp_busy_delay_us(200);

	mp_lapic_send_sipi(dest_lapic, MP_BOOT >> 12);
	mp_busy_delay_us(200);

	/*
	 * Wait up to ~2 s for the AP to reach cpu_up() and flip its
	 * machine_slot[].running flag.  With the TSC-calibrated delay this is
	 * a true 2 s of wall-clock margin, which covers a slow-to-schedule AP
	 * under host oversubscription (e.g. -smp == host thread count).
	 */
	for (wait_ms = 0; wait_ms < 2000; wait_ms++) {
		if (machine_slot[slot_num].running == TRUE) {
			printf("cpu_start: AP %d online (lapic=%d)\n",
			       slot_num, dest_lapic);
			return KERN_SUCCESS;
		}
		mp_busy_delay_ms(1);
	}

	printf("cpu_start: AP %d (lapic=%d) failed to come online\n",
	       slot_num, dest_lapic);
	return KERN_FAILURE;
}

/*ARGSUSED*/
kern_return_t
cpu_control(
	int			slot_num,
	processor_info_t	info,
	unsigned int		count)
{
	printf("cpu_control not implemented\n");
	return (KERN_FAILURE);
}

int real_ncpus;
int wncpu = -1;

/*
 * Find out how many cpus will run
 */
 
void
mp_probe_cpus(void)
{
	int i;

	/* 
	 * get real number of cpus
	 */

	real_ncpus = get_ncpus();

	if (wncpu <= 0)
		wncpu = NCPUS;

	/*
	 * Ignore real number of cpus it if number of requested cpus
	 * is smaller.
	 * Keep it if number of requested cpu is null or larger.
	 */

	if (real_ncpus < wncpu)
		wncpu = real_ncpus;
#if	MP_V1_1
    {
	extern void validate_cpus(int);

	/*
	 * We do NOT have CPUS numbered contiguously.
	 */
	
	validate_cpus(wncpu);
    }
#else
	for (i=0; i < wncpu; i++)
		machine_slot[i].is_cpu = TRUE;
#endif
}

/*
 * invoke kdb on slave processors 
 */

void
remote_kdb(void)
{
	int	my_cpu;
	register int	i;

	mp_disable_preemption();
	my_cpu = cpu_number();
	for (i = 0; i < NCPUS; i++)
		if ( i != my_cpu && machine_slot[i].running == TRUE) {
			i_bit_set(MP_KDB, &cpu_int_word[i]);
			cpu_interrupt(i);
		}
	mp_enable_preemption();
}

/*
 * Clear kdb interrupt
 */

void
clear_kdb_intr(void)
{
	mp_disable_preemption();
	i_bit_clear(MP_KDB, &cpu_int_word[cpu_number()]);
	mp_enable_preemption();
}

#else /* NCPUS > 1 */
int	cpu_int_word[NCPUS];
#endif /* NCPUS > 1 */
