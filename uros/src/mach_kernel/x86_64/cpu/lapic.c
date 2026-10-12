/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Local APIC (#438).
 */

#include <stdint.h>

#include <boot/bootarg.h>	/* #659: -2 */
#include <cpu/acpi.h>		/* #659: the widest id the MADT lists */
#include <cpu/iommu.h>		/* #659: the DMAR's opt-out */
#include <cpu/lapic.h>
#include <cpu/percpu.h>	/* #457: where the acknowledgement count lives */
#include <cpu/regs.h>
#include <pmap/pmap.h>
#include <time/pit.h>
#include <time/ruler.h>	/* #508: the rule the TSC shares */
#include <time/rulers.h>	/* #508: against the elected ruler */
#include <time/exact.h>		/* #594: the rule the TSC's sources follow */
#include <time/freq_source.h>	/* #508: the timer's exact sources */
#include <trap/trap.h>

#define MSR_APIC_BASE		0x1B
#define APIC_BASE_BSP		(1UL << 8)	/* this is the boot processor */
#define APIC_BASE_X2APIC	(1UL << 10)
#define APIC_BASE_ENABLE	(1UL << 11)
#define APIC_BASE_ADDR_MASK	0x000ffffffffff000ULL

/* Register offsets, in bytes from the base. */
#define LAPIC_ID		0x020
#define LAPIC_VERSION		0x030
#define LAPIC_TPR		0x080	/* task priority           */
#define LAPIC_EOI		0x0B0	/* end of interrupt        */
#define LAPIC_SVR		0x0F0	/* spurious vector, and the enable bit */
#define LAPIC_ISR		0x100	/* eight registers, 32 vectors apiece */
#define LAPIC_ICR_LOW		0x300
#define LAPIC_ICR_HIGH		0x310
#define LAPIC_LVT_TIMER		0x320	/* vector, mode and mask   */
#define LAPIC_TIMER_INIT	0x380	/* what it counts down from */
#define LAPIC_TIMER_CUR		0x390	/* where it is now          */
#define LAPIC_TIMER_DIV		0x3E0	/* the divisor, oddly coded */

#define LVT_MASKED		(1U << 16)
#define LVT_TIMER_PERIODIC	(1U << 17)
#define LVT_TIMER_DEADLINE	(2U << 17)	/* #459: bits 18:17 = 10b */

/*
 * The divisor field is not a number: its three bits are 3, 1 and 0, with bit
 * 2 unused, so the encodings do not run in order.  0b1011 is divide by one,
 * and 0b0011 — the value below — is divide by sixteen.
 *
 * Sixteen because both ends matter.  Undivided, a fast bus overflows the
 * thirty-two bit counter in a few seconds, which is not enough range for a
 * slow tick to be expressed as one countdown; divided far harder, a fast
 * tick starts being quantised by the divisor itself.  Sixteen leaves room at
 * both ends on every machine this will see.
 */
#define TIMER_DIVIDE_16		0x3

#define SVR_APIC_ENABLE		(1U << 8)

/* Interrupt command, low half. */
#define ICR_DELIVERY_FIXED	(0U << 8)
#define ICR_DELIVERY_NMI	(4U << 8)
#define ICR_DELIVERY_INIT	(5U << 8)
#define ICR_DELIVERY_STARTUP	(6U << 8)
#define ICR_LEVEL_ASSERT	(1U << 14)
#define ICR_TRIGGER_LEVEL	(1U << 15)
#define ICR_DELIVERY_PENDING	(1U << 12)

/*
 * Destination shorthands, which name a set of processors in the command
 * itself instead of one processor in the high half.  Two of the three are
 * worth having: "self" needs no destination at all, and "all but self" is
 * one message where naming them one at a time would be one each.
 */
#define ICR_DEST_SELF		(1U << 18)
#define ICR_DEST_ALL_BUT_SELF	(3U << 18)

static volatile uint8_t *lapic;

/*
 * 🔑 x2APIC MODE: THE SAME REGISTERS, AS MSRs (#659).
 *
 * In x2APIC mode the registers are MSRs 800H to 8FFH, one for each 16-byte
 * slot of the xAPIC page -- MSR 800H + offset / 16 -- with three exceptions
 * this file meets one of: the two halves of the command register are one
 * 64-bit MSR, 830H, and there is no 831H (SDM Vol. 3A 253668-093US 13.12.1.2,
 * Table 13-6).  The page is never mapped in a mode kept from the firmware,
 * and after lapic_choose_mode()'s switch it stays mapped and unread; the mode
 * is the processor's, set in its IA32_APIC_BASE, and every processor runs the
 * boot processor's.
 */
#define MSR_X2APIC(reg)		(0x800U + ((reg) >> 4))
#define MSR_X2APIC_ICR		0x830U

static int x2apic;
static int x2apic_planned;		/* #659: what the engines are written for */
static uint32_t x2apic_needed;		/* #659: an id xAPIC cannot name */
static int present;
static enum lapic_mode lapic_mode_reached = LAPIC_MODE_XAPIC;

static uint32_t lapic_read(unsigned reg)
{
	if (x2apic)
		return (uint32_t) rdmsr(MSR_X2APIC(reg));
	return *(volatile uint32_t *)(lapic + reg);
}

static void lapic_write(unsigned reg, uint32_t value)
{
	if (x2apic) {
		wrmsr(MSR_X2APIC(reg), value);
		return;
	}
	*(volatile uint32_t *)(lapic + reg) = value;
}

void lapic_init(uint64_t madt_base)
{
	uint64_t msr = rdmsr(MSR_APIC_BASE);
	uint64_t base = msr & APIC_BASE_ADDR_MASK;

	if (!(msr & APIC_BASE_ENABLE))
		panic("lapic: the local APIC is disabled in IA32_APIC_BASE");

	/*
	 * x2APIC replaces the memory-mapped registers with MSRs entirely, so
	 * the page below would be reading nothing.  Found on, the firmware
	 * turned it on, and it is kept (#659): it could be left only through
	 * the disabled state, losing every register (SDM 13.12.5), and a
	 * firmware that chose it may have had a reason.  The registers are
	 * MSRs from here on.
	 */
	if (msr & APIC_BASE_X2APIC) {
		x2apic = 1;
		present = 1;
		lapic_mode_reached = LAPIC_MODE_X2APIC_KEPT;
		return;
	}

	/*
	 * Two sources, and the MSR wins: the MADT records where the firmware
	 * placed the registers, the MSR where they are now.  A mismatch is
	 * worth saying out loud rather than silently preferring one — it means
	 * something moved them, and whatever did may have had reasons that
	 * matter elsewhere.
	 */
	if (madt_base != 0 && madt_base != base)
		panic("lapic: the MADT and IA32_APIC_BASE disagree about the base");

	lapic = (volatile uint8_t *)(uintptr_t)pmap_map_device(base, 0x1000);
	present = 1;
}

/*
 * 🔑 ONE DECISION, ONCE THE DMAR HAS BEEN READ (#659).
 *
 * Kept when the firmware left it on -- lapic_init() found that.  Turned on
 * when asked (-2), or when the MADT lists a processor whose id is 255 or past
 * it, which xAPIC mode cannot name at all (LAPIC_XAPIC_ID_MAX,
 * lapic_icr_destination()) -- the ids a firmware hands over in x2APIC mode
 * for, unless it chose otherwise (SDM 13.12.7).  Reaching
 * such an id with a device's interrupt takes remapping entries for 32-bit ids
 * (iommu_remaps_x2apic()), and without remapping x2APIC mode is
 * architecturally a guest's: "Intel VT-d Interrupt Remapping is a
 * prerequisite for using the x2APIC mode" (SDM 13.12.6), and Linux turns it on
 * without remapping only under a hypervisor.  So on bare metal it is planned
 * only with -i and engines that can carry 32-bit ids, and switched on only
 * once remapping came on with them.  Never turned on where the DMAR opts out;
 * a firmware that opts out and left it on anyway is said, not resolved.
 *
 * Made in two steps, because the mode is part of what the remapping engines
 * are given -- EIME on Intel; XTEn, GAEn and the 128-bit entry on AMD -- and
 * the engines are written before the switch.  lapic_plan_mode() decides, once
 * the DMAR has been read and before any engine is; lapic_choose_mode() makes
 * the switch the plan says, unless on bare metal remapping did not come on
 * with entries for 32-bit ids: then it stays in xAPIC mode and says so, the
 * plan -- what the engines were written for -- unchanged, and the entries
 * read by nobody.
 *
 * xAPIC to x2APIC keeps the registers software set, but the logical
 * destination, an id written to the ID register and the command register's
 * high half (SDM 13.12.5) -- none of which this kernel keeps -- so the switch
 * can come after the timer and the self-tests have run in xAPIC mode.
 */
void lapic_plan_mode(void)
{
	uint32_t a, b, c, d;

	/* No local APIC found (lapic_init() never ran): nothing to plan. */
	if (!present)
		return;

	if (x2apic) {
		x2apic_planned = 1;
		if (iommu_x2apic_discouraged())
			lapic_mode_reached = LAPIC_MODE_X2APIC_KEPT_OPTED_OUT;
		return;
	}

	/* The widest id the MADT lists, when nobody asked: the boot says which. */
	for (unsigned i = 0; i < acpi_cpu_count() && !boot_flag('2'); i++)
		if (acpi_cpu(i)->apic_id > LAPIC_XAPIC_ID_MAX
		    && acpi_cpu(i)->apic_id > x2apic_needed)
			x2apic_needed = acpi_cpu(i)->apic_id;
	if (!boot_flag('2') && x2apic_needed == 0)
		return;

	cpuid(1, &a, &b, &c, &d);
	if (!(c & (1U << 21)))
		lapic_mode_reached = LAPIC_MODE_X2APIC_ABSENT;
	else if (iommu_x2apic_discouraged())
		lapic_mode_reached = LAPIC_MODE_X2APIC_OPTED_OUT;
	else if (!(c & (1U << 31))
		 && !(boot_flag('i') && iommu_can_remap_x2apic()))
		lapic_mode_reached = LAPIC_MODE_X2APIC_NOT_ON_METAL;
	else
		x2apic_planned = 1;
}

void lapic_choose_mode(void)
{
	uint32_t a, b, c, d;
	int remapped = iommu_interrupts_remapped() && iommu_remaps_x2apic();

	if (!present || x2apic || !x2apic_planned)
		return;

	/*
	 * On bare metal only once remapping came on with entries for 32-bit
	 * ids: the plan could see only that it might.  Entries built for them
	 * by engines that then did not turn on are entries nothing reads.
	 */
	cpuid(1, &a, &b, &c, &d);
	if (!(c & (1U << 31)) && !remapped) {
		lapic_mode_reached = LAPIC_MODE_X2APIC_NOT_ON_METAL;
		return;
	}

	/*
	 * Read back, as every mode line says what it read (#659): EN was set
	 * when lapic_init() looked, and xAPIC to x2APIC is the one write SDM
	 * 13.12.5 allows, so a register that did not take it is a processor
	 * this kernel cannot drive -- every accessor from here on would be an
	 * MSR it refuses.
	 */
	wrmsr(MSR_APIC_BASE, rdmsr(MSR_APIC_BASE) | APIC_BASE_X2APIC);
	if (!(rdmsr(MSR_APIC_BASE) & APIC_BASE_X2APIC))
		panic("lapic: IA32_APIC_BASE did not take x2APIC mode (#659)");
	x2apic = 1;
	lapic_mode_reached = remapped ? LAPIC_MODE_X2APIC_TURNED_ON_REMAPPED
				      : LAPIC_MODE_X2APIC_TURNED_ON;
}

int lapic_x2apic_planned(void)
{
	return x2apic_planned;
}

uint32_t lapic_x2apic_needed(void)
{
	return x2apic_needed;
}

enum lapic_mode lapic_mode(void)
{
	return lapic_mode_reached;
}

int lapic_x2apic(void)
{
	return x2apic;
}

/*
 * #659: remapping may come on only with entries for 32-bit ids -- the local
 * APIC already in x2APIC mode, kept from the firmware, on bare metal, where
 * "software must enable this interrupt remapping with Extended Interrupt Mode
 * Enabled before it enabling x2APIC mode" (SDM 13.12.7) and AMD's engines
 * "must set MMIO 0x18[XTEn]=1 and MMIO 0x18[GAEn]=1" for it (Rev 3.11
 * §2.2.5.3).  Under a hypervisor x2APIC mode with eight-bit entries is the
 * guest's practice, as it is without remapping (lapic_plan_mode()).
 */
int lapic_remapping_needs_x2apic_entries(void)
{
	uint32_t a, b, c, d;

	if (!x2apic)
		return 0;
	cpuid(1, &a, &b, &c, &d);
	return !(c & (1U << 31));
}

/*
 * The boot processor's mode on this one too (#659): an application processor
 * comes out of INIT in the mode it was in before, which is xAPIC unless the
 * firmware chose otherwise, and every register here is an MSR or a page
 * according to the boot processor's choice.  xAPIC to x2APIC is one write
 * (SDM 13.12.5).  The other way is not, and a processor left in x2APIC mode
 * behind a boot processor that is not would answer none of the page's
 * addresses: said, since nothing here could be believed after it.
 *
 * 🔴 The first thing an application processor does once it can report a
 * fault (ap_start_c()), not the last before its interrupts: the accessors
 * follow the boot processor's choice, not the mode of the processor running
 * them, so until this ran a fault report's lapic_id() or a panic's halting
 * IPI was an x2APIC MSR on a processor in xAPIC mode, which "causes a
 * general-protection exception" (SDM 13.12.2) inside the report.
 */
void lapic_mode_here(void)
{
	uint64_t msr = rdmsr(MSR_APIC_BASE);

	if (!present)
		panic("lapic: asked to enable an APIC that was never mapped");

	if (x2apic && !(msr & APIC_BASE_X2APIC))
		wrmsr(MSR_APIC_BASE, msr | APIC_BASE_X2APIC);
	else if (!x2apic && (msr & APIC_BASE_X2APIC))
		panic("lapic: this processor is in x2APIC mode and the boot "
		      "processor is not (#659)");
}

int lapic_mode_matches(void)
{
	return ((rdmsr(MSR_APIC_BASE) & APIC_BASE_X2APIC) != 0) == (x2apic != 0);
}

void lapic_enable(void)
{
	/* Nothing to do on a processor ap_start_c() switched already. */
	lapic_mode_here();

	/*
	 * Accept every priority.  Reset leaves this at zero, which is what we
	 * want — but a startup interrupt is not a reset, and an application
	 * processor inherits whatever the firmware left here.  A threshold set
	 * high enough turns every interrupt this CPU is sent into silence, and
	 * silence is the one symptom that points nowhere.
	 */
	lapic_write(LAPIC_TPR, 0);

	/*
	 * And the enable bit, which shares its register with the spurious
	 * vector — so naming the vector and turning the APIC on are one write,
	 * and neither can be done without the other.
	 */
	lapic_write(LAPIC_SVR, SVR_APIC_ENABLE | LAPIC_SPURIOUS_VECTOR);
}

void lapic_eoi(void)
{
	percpu()->acked++;
	lapic_write(LAPIC_EOI, 0);
}

uint64_t lapic_ack_count(void)
{
	return percpu()->acked;
}

/*
 * The in-service register: one bit per vector, set from the moment the
 * processor accepts the interrupt until EOI clears it.
 *
 * Thirty-two vectors per register and sixteen bytes between registers, which
 * is the architecture's layout and not a choice -- the gaps are where the
 * other 96 bits of each 128-bit slot would be if they existed.
 */
int lapic_in_service(uint8_t vector)
{
	unsigned reg = LAPIC_ISR + ((unsigned)vector >> 5) * 0x10;

	return (lapic_read(reg) >> (vector & 31)) & 1;
}

/* Where the registers ended up, so the mapping itself can be inspected. */
uint64_t lapic_probe_va(void)
{
	return (uint64_t)(uintptr_t)lapic;
}

int lapic_present(void)
{
	return present;
}

uint32_t lapic_id(void)
{
	/*
	 * In x2APIC mode the register is the whole 32-bit id (SDM 13.12.5.1).
	 * In xAPIC mode the id sits in the top byte, not the bottom: the low
	 * bits are reserved, and reading the word as an id would give a number
	 * no CPU has.
	 */
	if (x2apic)
		return lapic_read(LAPIC_ID);
	return lapic_read(LAPIC_ID) >> 24;
}

int lapic_is_bsp(void)
{
	return (rdmsr(MSR_APIC_BASE) & APIC_BASE_BSP) != 0;
}

/*
 * Wait for the previous command to be accepted.  The interrupt command
 * register holds one message at a time, and writing a second before the
 * first has been delivered loses it — silently, which is the whole reason
 * this loop exists rather than a delay.
 */
static void icr_wait_idle(void)
{
	/*
	 * Nothing to wait for in x2APIC mode: the delivery status bit is gone,
	 * and "system software no longer has a reason to read the ICR"
	 * (SDM 13.12.9) -- what a read returns need not be what was written.
	 */
	if (x2apic)
		return;
	while (lapic_read(LAPIC_ICR_LOW) & ICR_DELIVERY_PENDING)
		cpu_pause();
}

/*
 * #593: the ablation that leaves the send open to an interrupt, and the count
 * that shows what the opening lets in: a send begun on a processor that was
 * already past the wait of another and not yet past its second write -- the
 * window the masking closes.  A send begun during another's wait is harmless
 * (the wait reads the register again) and is not counted.
 */
#ifndef	ABLATE_593_ICR_OPEN
#define	ABLATE_593_ICR_OPEN	0
#endif

#if ABLATE_593_ICR_OPEN
#include <cpus.h>		/* NCPUS */

/*
 * ⚠️ Indexed by the APIC's own ID register and not by cpu_number(): that
 * reads %gs, and a send is made from inside the swapgs window on purpose --
 * the #440 test sends an NMI with the user's %gs loaded -- where a %gs read
 * faults, and the faults nest until the stack is gone.  The first version of
 * this counter did exactly that, and the ablated kernel never finished its
 * boot: an instrument that changes the experiment.
 */
static volatile uint32_t	icr_inside[NCPUS];
static volatile uint64_t	icr_nested[NCPUS];	/* each by its own */

uint64_t lapic_icr_nested(void)
{
	uint64_t	sum = 0;
	unsigned	c;

	for (c = 0; c < NCPUS; c++)
		sum += icr_nested[c];
	return sum;
}

uint64_t lapic_icr_nested_on(uint32_t apic_id)
{
	return apic_id < NCPUS ? icr_nested[apic_id] : 0;
}
#endif

/*
 * #659: the command register's destination for `apic_id', in the mode given:
 * all 32 bits in x2APIC mode, the 64-bit register's high half (SDM 13.12.9),
 * and eight in xAPIC mode, in the high half's 31:24.  An id above
 * LAPIC_XAPIC_ID_MAX is refused there: shifted into those eight an id past
 * them kept its low byte, and an INIT for processor 256 was an INIT for
 * processor 0 -- #598's C16 had looked at the routing words and not at this
 * one -- and 255 is the broadcast, an INIT for every processor.
 */
int lapic_icr_destination(uint32_t apic_id, int x2apic_mode, uint32_t *high)
{
	if (x2apic_mode) {
		*high = apic_id;
		return 1;
	}
	if (apic_id > LAPIC_XAPIC_ID_MAX)
		return 0;
	*high = apic_id << 24;
	return 1;
}

static void icr_send(uint32_t apic_id, uint32_t command)
{
	uint32_t high = 0;

	/* Before anything is written: see lapic_icr_destination(). */
	if (!lapic_icr_destination(apic_id, x2apic, &high))
		panic("lapic: apic id %u does not fit the command register in "
		      "xAPIC mode, and would reach the processor with its low "
		      "byte (#659)", apic_id);

#if !ABLATE_593_ICR_OPEN
	int was_enabled = interrupts_enabled();

	/*
	 * 🔴 NO INTERRUPT BETWEEN THE WAIT AND THE SECOND WRITE (#593).
	 *
	 * The register is one per processor and a send is three steps on it,
	 * so an interrupt whose handler sends an IPI of its own, landing in
	 * the middle, rewrites the destination under the interrupted send:
	 * the interrupted command then leaves with the handler's destination,
	 * and the processor it was meant for never hears it.  Landing after
	 * the wait and before the writes is the same failure the wait exists
	 * to prevent -- a second message written over a first still pending.
	 *
	 * The tick already sends from interrupt context (hertz_tick can wake a
	 * thread and knock on the processor it is given to), and the HPET's
	 * broadcast sends from a class no spl level holds back, a hundred
	 * times a second.  Masked here and nowhere else, because this is the
	 * one place that knows the register takes more than one write.
	 */
	interrupts_disable();
#else
	/*
	 * Counted by the processor it happens on, so no two write one count:
	 * a send that interrupts this one runs in a handler with interrupts
	 * off, and nothing interrupts it in turn.
	 */
	unsigned self = lapic_id();

	if (self < NCPUS && icr_inside[self])
		icr_nested[self]++;
#endif

	icr_wait_idle();
#if ABLATE_593_ICR_OPEN
	if (self < NCPUS)
		icr_inside[self]++;
#endif

	if (x2apic) {
		/*
		 * One 64-bit write, the destination in its high half
		 * (SDM 13.12.9) -- and a fence before it, because unlike a
		 * store to an uncached page it is not serializing: "A WRMSR
		 * to an APIC register may complete before all preceding
		 * stores are globally visible; software can prevent this by
		 * inserting a serializing instruction or the sequence
		 * MFENCE;LFENCE before the WRMSR" (13.12.3).  Without it a
		 * cross-call could arrive before the request it announces.
		 */
		__asm__ volatile("mfence; lfence" : : : "memory");
		wrmsr(MSR_X2APIC_ICR, ((uint64_t) high << 32) | command);
	} else {
		/*
		 * The destination goes in the high half and the command in
		 * the low half, and writing the low half is what sends it —
		 * so the order is not a style question.
		 */
		lapic_write(LAPIC_ICR_HIGH, high);
		lapic_write(LAPIC_ICR_LOW, command);
	}

#if !ABLATE_593_ICR_OPEN
	if (was_enabled)
		interrupts_enable();
#else
	if (self < NCPUS)
		icr_inside[self]--;
#endif
}

void lapic_send_init(uint32_t apic_id)
{
	icr_send(apic_id, ICR_DELIVERY_INIT | ICR_LEVEL_ASSERT
		 | ICR_TRIGGER_LEVEL);
	icr_wait_idle();

	/*
	 * The de-assert, which the older parts require and the newer ones
	 * ignore.  Sending it costs a message; not sending it leaves those
	 * older parts holding the reset line.
	 */
	icr_send(apic_id, ICR_DELIVERY_INIT | ICR_TRIGGER_LEVEL);
	icr_wait_idle();
}

void lapic_send_startup(uint32_t apic_id, uint64_t entry_pa)
{
	/*
	 * The message carries a vector, and the target turns it into an
	 * address by shifting left twelve — so the entry point has to be a
	 * page number below 1 MiB, and anything else is not a startup address
	 * that can be expressed at all.
	 */
	if (entry_pa >= 0x100000 || (entry_pa & 0xFFF))
		panic("lapic: startup entry is not a page below 1 MiB");

	icr_send(apic_id, ICR_DELIVERY_STARTUP | ICR_LEVEL_ASSERT
		 | (uint32_t)(entry_pa >> 12));
	icr_wait_idle();
}

void lapic_send_self(uint8_t vector)
{
	/*
	 * No wait for delivery afterwards, unlike the startup sequence: this
	 * one is aimed at the processor that is asking, so waiting for the
	 * command to be accepted would be waiting for an interrupt that cannot
	 * be taken until this returns and enables it.  The caller looks for
	 * the effect instead.
	 */
	icr_send(0, ICR_DELIVERY_FIXED | ICR_DEST_SELF | ICR_LEVEL_ASSERT
		 | vector);
}

void lapic_send_ipi(uint32_t apic_id, uint8_t vector)
{
	icr_send(apic_id, ICR_DELIVERY_FIXED | ICR_LEVEL_ASSERT | vector);
	icr_wait_idle();
}

void lapic_send_nmi(uint32_t apic_id)
{
	/*
	 * No vector: NMI delivery ignores the field and the processor takes
	 * vector 2 by definition.  Named destination rather than the self
	 * shorthand even when the destination is this processor, because that
	 * shorthand is defined for fixed delivery only.
	 */
	icr_send(apic_id, ICR_DELIVERY_NMI | ICR_LEVEL_ASSERT);
	icr_wait_idle();
}

/* ------------------------------------------------------------------ */
/*  The timer (#409)                                                    */
/* ------------------------------------------------------------------ */
/*
 * Measured once, on the boot processor, and used by all of them.
 *
 * The rate is a property of the machine rather than of a processor — every
 * local APIC timer counts off the same bus or crystal clock — while the
 * timer itself is per processor, which is the whole reason for preferring it
 * to the 8254.  So one measurement is shared and each processor arms its own
 * countdown from it.
 *
 * ⚠️ It has to be measured on one processor at a time regardless: the ruler
 * is the 8254, and the 8254 is a single global device. Two processors
 * calibrating at once would be two callers programming one counter.
 */
static uint32_t timer_hz;

/*
 * The timer's current count, read as a counter that goes UP, which is what
 * time/ruler.c measures.  The timer is left counting down from the top in
 * one-shot mode while this is read; at any divisor a machine uses that is
 * seconds before it reaches zero, and the whole calibration takes well under
 * one.
 */
static uint64_t subject_timer(void)
{
	return (uint32_t)~lapic_read(LAPIC_TIMER_CUR);
}

/*
 * Against the 8254 read back, by the rule the TSC is calibrated by
 * (time/ruler.c: three runs, the median, four attempts), which is what #464
 * asked for when it retried this one and left the TSC giving up the boot.
 *
 * ⚠️ The old measurement waited on pit_delay_us(), programming the 8254 inside
 * its own interval, over 20 ms: the tick self-test showed the result once the
 * TSC stopped carrying the same error -- the tick 0.7% longer than asked for,
 * under KVM and TCG alike.
 */
static struct ruler_calibration timer_cal;

/*
 * The rates the machine states for this timer (#508, #594), after the divisor
 * this file sets, checked against the measurement by the rule time/exact.c
 * states for the TSC's sources as well: the first that agrees is adopted, a
 * later one more than 10 ppm from it is not used, one further from the
 * measurement than the rulers allow is WRONG.
 *
 * Two sources, in that order.  CPUID 0x15's crystal, when the leaf states it:
 * the manual says the timer counts that crystal whenever the leaf enumerates
 * the TSC's ratio to it (SDM Vol. 3A, 253668-093US, 13.5.4) -- 38.4 MHz on
 * OMEGA, 2.4 MHz after the divisor.  Then the hypervisor's timing leaf, whose
 * bus field is what a guest's timer counts: QEMU's is 1 GHz, 62.5 MHz after
 * the divisor.  No hypervisor this kernel has run under fills 0x15 in, so in a
 * guest the first is absent and the order has never had to choose.
 */
#define TIMER_DIVISOR		16	/* what TIMER_DIVIDE_16 selects */

static struct exact_choice timer_source = { .adopted = -1 };

uint32_t lapic_timer_calibrate(void)
{
	timer_hz = 0;
	if (!lapic_present())
		return 0;

	/*
	 * Masked throughout.  Calibration is a measurement, not a service, and
	 * a vector delivered in the middle of it would be an interrupt nobody
	 * has arranged to handle yet.
	 */
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
	lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
	lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);

	/*
	 * Against the ruler the vote elected (time/rulers.h): the narrowest of
	 * those it did not name, so a ruler found wrong measuring the TSC does
	 * not go on to measure this.
	 */
	{
		unsigned		id = rulers_elected();
		struct kernel_ruler	*k = rulers_get(id);

		rulers_start(id);
		if (ruler_calibrate(&k->r, subject_timer, 0xffffffffULL,
				    k->span, 1ULL << 34, 0, &timer_cal))
			timer_hz = (uint32_t)timer_cal.hz;
		rulers_stop(id);
	}

	lapic_write(LAPIC_TIMER_INIT, 0);	/* stop */

	{
		struct freq_exact	e;
		uint64_t		hz[FREQ_EXACT], adopted, bracket = 0;
		unsigned		i;

		freq_exact_read(&e);
		for (i = 0; i < FREQ_EXACT; i++)
			hz[i] = e.lapic_hz[i] / TIMER_DIVISOR;
		for (i = 0; i < RULER_RUNS; i++)
			if (timer_cal.run_hz[i] != 0
			    && timer_cal.run_ppm[i] > bracket)
				bracket = timer_cal.run_ppm[i];
		adopted = exact_choose(hz, timer_hz, bracket, &timer_source);
		if (adopted != 0)
			timer_hz = (uint32_t)adopted;
	}
	return timer_hz;
}

uint64_t lapic_timer_measured_hz(void)
{
	return timer_cal.hz;
}

const struct exact_choice *lapic_timer_source(void)
{
	return &timer_source;
}

/*
 * How many attempts the calibration needed, for whoever prints the boot line.
 *
 * Exported rather than kept private because a retry nobody can see is a retry
 * nobody can tell from an absent one: a build where it never engages and a
 * build where it engages on every boot produce identical logs, and the first
 * regression in it would be invisible.  The number is the presence.
 */
unsigned lapic_timer_calibrate_attempts(void)
{
	return timer_cal.attempts;
}

uint32_t lapic_timer_hz_run(unsigned which)
{
	return which < RULER_RUNS ? (uint32_t)timer_cal.run_hz[which] : 0;
}

uint64_t lapic_timer_window_ppm(unsigned which)
{
	return which < RULER_RUNS ? timer_cal.run_ppm[which] : 0;
}

int lapic_timer_set_aside(void)
{
	return timer_cal.set_aside;
}

uint32_t lapic_timer_hz(void)
{
	return timer_hz;
}

int lapic_timer_start(unsigned hz, uint8_t vector)
{
	uint32_t count;

	if (timer_hz == 0 || hz == 0)
		return 0;

	count = timer_hz / hz;

	/*
	 * A count of zero means "stopped", not "as fast as possible", so a
	 * tick faster than the timer can express has to be refused rather than
	 * silently turned off.  This is the shape of failure that looks like a
	 * working kernel with no clock.
	 */
	if (count == 0)
		return 0;

	/*
	 * The divisor again on every processor: it is per-APIC state, and an
	 * application processor arrives with whatever reset left in it rather
	 * than with what the boot processor chose during calibration.
	 */
	lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
	lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_PERIODIC | vector);
	lapic_write(LAPIC_TIMER_INIT, count);
	return 1;
}

void lapic_timer_stop(void)
{
	if (!lapic_present())
		return;

	/* Count first, then mask: masking alone leaves it counting. */
	lapic_write(LAPIC_TIMER_INIT, 0);
	lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
}

/*
 * TSC-deadline mode (#459).  The LVT still selects the vector -- the deadline
 * replaces the COUNTDOWN, not the delivery path -- with mode bits 18:17 set
 * to 10b.  The countdown registers are ignored in this mode; the MSR is the
 * whole interface, and clock_event.c owns it.
 *
 * ⚠️ In xAPIC mode the SDM requires an MFENCE between the mode write and the
 * first deadline write: without it the WRMSR can be observed before the mode
 * is in effect, and the deadline is then read by a timer still in one-shot
 * mode -- an arm that appears to succeed and never fires.  In x2APIC mode the
 * mode write is a WRMSR too, which "the processor ensures the ordering of ...
 * no fencing is required" (SDM 13.5.4.1, #659); the fence stays, harmless.
 */
void lapic_timer_deadline_setup(uint8_t vector)
{
	if (!lapic_present())
		return;

	lapic_write(LAPIC_TIMER_INIT, 0);	/* leave no countdown behind */
	lapic_write(LAPIC_LVT_TIMER, LVT_TIMER_DEADLINE | vector);
	__asm__ __volatile__("mfence" : : : "memory");
}

/*
 * One-shot mode (#459).  Point the LVT at `vector' with the periodic bit
 * clear and leave the countdown stopped; lapic_timer_oneshot_arm() starts it.
 *
 * Separate from lapic_timer_start() rather than a flag on it, because the two
 * differ in more than a bit: a periodic timer is armed once and forgotten,
 * a one-shot one is dead until somebody arms it, and a caller that confuses
 * them gets a kernel whose clock fires exactly once.
 */
void lapic_timer_oneshot_setup(uint8_t vector)
{
	if (!lapic_present())
		return;

	/* The divisor on every processor: it is per-APIC state, and an
	 * application processor arrives with whatever reset left in it. */
	lapic_write(LAPIC_TIMER_DIV, TIMER_DIVIDE_16);
	lapic_write(LAPIC_TIMER_INIT, 0);		/* stopped until armed */
	lapic_write(LAPIC_LVT_TIMER, vector);		/* one-shot: no periodic */
}

/*
 * Start the countdown.  `count' is in calibrated timer ticks, so the caller
 * converts from time using lapic_timer_hz().
 *
 * A count of zero is refused rather than written: zero means STOPPED, so
 * writing it would arm nothing while looking like success -- the failure that
 * presents as a working kernel with no clock.
 */
int lapic_timer_oneshot_arm(uint32_t count)
{
	if (!lapic_present() || count == 0)
		return 0;

	lapic_write(LAPIC_TIMER_INIT, count);
	return 1;
}

void lapic_broadcast_ipi(uint8_t vector)
{
	icr_send(0, ICR_DELIVERY_FIXED | ICR_DEST_ALL_BUT_SELF
		 | ICR_LEVEL_ASSERT | vector);
	icr_wait_idle();
}
