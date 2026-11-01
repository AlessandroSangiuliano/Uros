/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The processor's performance-monitoring counters, programmed for the
 * profiler's phases (#804).
 *
 * The profiler (#411, #392) times a trap's phases by the TSC, which says
 * where the cycles go and not why: a cache miss, a TLB walk, a line fetched
 * from another core's cache and a longer path all lengthen a phase alike.
 * #772 found phases that grew in code it does not change, and nothing here
 * could say what grew.  So, once on every processor:
 *
 *   - CPUID.0AH says what there is (Intel SDM 325462-093US, Vol. 3B 22.2.1
 *     and 22.2.2): the version in EAX[7:0], the general counters' number and
 *     width in EAX[15:8] and EAX[23:16], the fixed ones' in EDX[4:0] and
 *     EDX[12:5], and in EBX a bit SET for each pre-defined event that is not
 *     there (Table 22-3);
 *   - pmc=N picks an event set (1 cache, 2 page walks, 3 the pre-defined
 *     events alone), which each processor writes into its own
 *     IA32_PERFEVTSELx (186H on), counting at ring 0 only; instructions
 *     retired and unhalted core cycles go in the first two fixed counters
 *     (IA32_FIXED_CTR_CTRL, 38DH), and IA32_PERF_GLOBAL_CTRL (38FH) turns
 *     them all on;
 *   - an event that is not the processor's is refused, and the refusal is
 *     said.  A counter never programmed reads zero, and a zero reads as
 *     "nothing happened".
 *
 * Every processor decides for itself, from its own CPUID and the command
 * line, which all of them share: the boot processor's decision is kept for
 * the report, but no processor waits for it.
 *
 * The events beyond the pre-defined ones are model events, with the codes of
 * Intel's tables for the 6th-generation core and the models its mapfile sends
 * there (intel/perfmon, SKL/events/skylake_core.json, V59 of 04/04/2024;
 * mapfile.csv: GenuineIntel-6-4E, -5E, -8E, -9E, -A5, -A6), omen's i7-7700HQ
 * (6-9E) among them.  On another model a set with a model event is refused.
 *
 * ⚠️ Intel only.  AMD's counters are other MSRs and other events, which #671
 * needs as well; until that backend is written an AMD processor is refused,
 * said, and never counted as zero.
 */

#include <stdint.h>

#include <cpus.h>			/* NCPUS */
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>		/* printf */

#include <boot/bootarg.h>
#include <cpu/pmc.h>
#include <cpu/regs.h>
#include <sync/atomic.h>
#include <time/tsc.h>

#define	MSR_IA32_PMC0			0xC1
#define	MSR_IA32_PERFEVTSEL0		0x186
#define	MSR_IA32_FIXED_CTR0		0x309
#define	MSR_IA32_FIXED_CTR_CTRL		0x38D
#define	MSR_IA32_PERF_GLOBAL_CTRL	0x38F
#define	MSR_IA32_DEBUGCTL		0x1D9
#define	MSR_IA32_PERF_CAPABILITIES	0x345	/* if CPUID.01H:ECX[15], PDCM */

#define	PERFEVTSEL_OS		(1u << 17)	/* count at ring 0 */
#define	PERFEVTSEL_EN		(1u << 22)
#define	FIXED_CTR_OS(n)		(1ULL << (4 * (n)))	/* ENABLE = 01b: ring 0 */
#define	GLOBAL_FIXED(n)		(1ULL << (32 + (n)))
#define	RDPMC_FIXED		(1u << 30)
#define	CPUID1_ECX_PDCM		(1u << 15)
#define	PERF_CAP_FREEZE_SMM	(1ULL << 12)	/* DEBUGCTL.FREEZE_WHILE_SMM is there */
#define	DEBUGCTL_FREEZE_SMM	(1ULL << 14)

#define	PMC_FIXED_USED		2	/* instructions retired, unhalted cycles */
#define	PMC_GENERAL_MAX		(PMC_MAX - PMC_FIXED_USED)

#define	PMC_SPIN_US		1000	/* the check in pmc_report() */

struct pmc_event {
	const char	*name;
	uint8_t		event, umask;
	int8_t		arch_bit;	/* its bit in CPUID.0AH:EBX; -1: a model event */
	uint8_t		role;		/* PMC_ROLE_*, what pmc_test.c loads it with */
};

/* pmc=1: where a load's line came from. */
static const struct pmc_event pmc_set_cache[] = {
	{ "L1D.REPLACEMENT",			0x51, 0x01, -1, PMC_ROLE_L1D_MISS },
	{ "L2_RQSTS.MISS",			0x24, 0x3f, -1, PMC_ROLE_L2_MISS },
	{ "LONGEST_LAT_CACHE.MISS",		0x2e, 0x41,  4, PMC_ROLE_LLC_MISS },
	{ "MEM_LOAD_L3_HIT_RETIRED.XSNP_HITM",	0xd2, 0x04, -1, PMC_ROLE_HITM },
};

/* pmc=2: page walks, for data and for code. */
static const struct pmc_event pmc_set_tlb[] = {
	{ "DTLB_LOAD_MISSES.WALK_COMPLETED",	0x08, 0x0e, -1, PMC_ROLE_DTLB_LOAD_WALK },
	{ "DTLB_STORE_MISSES.WALK_COMPLETED",	0x49, 0x0e, -1, PMC_ROLE_DTLB_STORE_WALK },
	{ "ITLB_MISSES.WALK_COMPLETED",		0x85, 0x0e, -1, PMC_ROLE_ITLB_WALK },
	{ "LONGEST_LAT_CACHE.MISS",		0x2e, 0x41,  4, PMC_ROLE_LLC_MISS },
};

/*
 * pmc=3: the pre-defined events alone (Table 22-3), the same codes on every
 * Intel processor that has them, so a set any of them can count.
 */
static const struct pmc_event pmc_set_arch[] = {
	{ "LLC Reference",			0x2e, 0x4f,  3, PMC_ROLE_LLC_REF },
	{ "LLC Misses",				0x2e, 0x41,  4, PMC_ROLE_LLC_MISS },
	{ "Branch Instruction Retired",		0xc4, 0x00,  5, PMC_ROLE_BRANCH },
	{ "Branch Misses Retired",		0xc5, 0x00,  6, PMC_ROLE_BRANCH_MISS },
};

static const struct pmc_set {
	const char		*what;
	const struct pmc_event	*events;
	int			n;
} pmc_sets[] = {
	{ 0, 0, 0 },
	{ "cache", pmc_set_cache, sizeof (pmc_set_cache) / sizeof (pmc_set_cache[0]) },
	{ "TLB", pmc_set_tlb, sizeof (pmc_set_tlb) / sizeof (pmc_set_tlb[0]) },
	{ "architectural", pmc_set_arch, sizeof (pmc_set_arch) / sizeof (pmc_set_arch[0]) },
};
#define	PMC_SETS	((int) (sizeof (pmc_sets) / sizeof (pmc_sets[0])))

/* Family 6 models whose core events are skylake_core.json's, by Intel's mapfile. */
static const uint8_t	pmc_skl_models[] = { 0x4e, 0x5e, 0x8e, 0x9e, 0xa5, 0xa6 };

/* Why pmc=N was refused. */
#define	PMC_OK			0
#define	PMC_NOT_A_NUMBER	1
#define	PMC_NO_SUCH_SET		2
#define	PMC_NOT_INTEL		3
#define	PMC_NO_ARCH		4
#define	PMC_VERSION_1		5
#define	PMC_TOO_FEW_GENERAL	6
#define	PMC_TOO_FEW_FIXED	7
#define	PMC_EVENT_ABSENT	8	/* a pre-defined event CPUID says is not there */
#define	PMC_EVENT_NOT_MODEL	9	/* a model event, and not this model's */

/* What one processor found, and what it chose. */
struct pmc_found {
	int		intel;
	uint32_t	family, model;
	uint32_t	version, general, general_bits, fixed, fixed_bits;
	uint32_t	arch_events, absent;	/* EAX[31:24], EBX */
	int		asked;			/* pmc= given */
	uint64_t	asked_value;
	int		set;			/* the set counting, 0 for none */
	int		refusal, bad_event;
	int		pdcm;			/* CPUID.01H:ECX[15]: IA32_PERF_CAPABILITIES is there */
};

static struct pmc_found	pmc_boot;		/* the boot processor's, for the report */
static volatile int	pmc_set_counting;	/* the same set, read by pmc_read() */
int			pmc_counting;		/* pmc_count(), for the profiler */

static volatile uint32_t pmc_seen;		/* processors past pmc_init_cpu() */
static volatile uint32_t pmc_programmed;	/* of them, the ones that programmed */
static volatile uint32_t pmc_kept;		/* ... and read every register back as written */
static volatile uint32_t pmc_frozen_in_smm;	/* ... and froze them while in SMM */

static int
pmc_is_skl_model(const struct pmc_found *f)
{
	unsigned	i;

	if (f->family != 6)
		return 0;
	for (i = 0; i < sizeof (pmc_skl_models); i++)
		if (pmc_skl_models[i] == f->model)
			return 1;
	return 0;
}

/* What this processor has, and the set it is to count, from CPUID and pmc=. */
static void
pmc_choose(struct pmc_found *f)
{
	uint32_t	a, b, c, d, max;
	int		r, i;
	uint64_t	v = 0;
	const struct pmc_set *s;

	cpuid(0, &max, &b, &c, &d);
	/* "GenuineIntel", in EBX, EDX, ECX. */
	f->intel = b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e;
	cpuid(1, &a, &b, &c, &d);
	f->pdcm = (c & CPUID1_ECX_PDCM) != 0;
	f->family = (a >> 8) & 0xf;
	f->model = (a >> 4) & 0xf;
	if (f->family == 0xf)
		f->family += (a >> 20) & 0xff;
	if (f->family == 6 || f->family >= 0xf)
		f->model |= ((a >> 16) & 0xf) << 4;
	if (f->intel && max >= 0xa) {
		cpuid(0xa, &a, &b, &c, &d);
		f->version = a & 0xff;
		f->general = (a >> 8) & 0xff;
		f->general_bits = (a >> 16) & 0xff;
		f->arch_events = (a >> 24) & 0xff;
		f->absent = b;
		f->fixed = d & 0x1f;
		f->fixed_bits = (d >> 5) & 0xff;
	}

	f->set = 0;
	f->refusal = PMC_OK;
	f->bad_event = -1;
	r = boot_value("pmc", &v);
	f->asked = r != 0;
	f->asked_value = v;
	if (r == 0)
		return;
	if (r != 1) {
		f->refusal = PMC_NOT_A_NUMBER;
		return;
	}
	if (v == 0)
		return;
	if (v >= (uint64_t) PMC_SETS) {
		f->refusal = PMC_NO_SUCH_SET;
		return;
	}
	s = &pmc_sets[v];
	if (!f->intel)
		f->refusal = PMC_NOT_INTEL;
	else if (f->version == 0)
		f->refusal = PMC_NO_ARCH;
	else if (f->version < 2)
		f->refusal = PMC_VERSION_1;
	else if (f->general < (uint32_t) s->n)
		f->refusal = PMC_TOO_FEW_GENERAL;
	else if (f->fixed < PMC_FIXED_USED)
		f->refusal = PMC_TOO_FEW_FIXED;
	for (i = 0; f->refusal == PMC_OK && i < s->n; i++) {
		const struct pmc_event *e = &s->events[i];

		if (e->arch_bit >= 0 &&
		    ((uint32_t) e->arch_bit >= f->arch_events ||
		     (f->absent >> e->arch_bit) & 1))
			f->refusal = PMC_EVENT_ABSENT;
		else if (e->arch_bit < 0 && !pmc_is_skl_model(f))
			f->refusal = PMC_EVENT_NOT_MODEL;
		if (f->refusal != PMC_OK)
			f->bad_event = i;
	}
	if (f->refusal == PMC_OK)
		f->set = (int) v;
}

/*
 * This processor's counters, the set's events in the general ones.
 *
 * 🔴 AND FROZEN WHILE THE FIRMWARE RUNS, WHERE THE PROCESSOR OFFERS IT.  An
 * SMI takes the processor into System Management Mode for the firmware's own
 * work -- on a laptop on battery, often -- and the counters go on counting
 * there unless IA32_DEBUGCTL.FREEZE_WHILE_SMM (bit 14) stops them (SDM
 * 325462-093US, 20.4.1; offered if IA32_PERF_CAPABILITIES[12], 22.8).  On
 * omen on battery a walk of known shape counted four times its instructions,
 * and a spin with interrupts off counted page walks, before this was set.
 */
static void
pmc_program(int set, int pdcm)
{
	const struct pmc_set	*s = &pmc_sets[set];
	uint64_t		sel, fixed, global, dbg;
	int			i, kept = 1;

	if (pdcm && (rdmsr(MSR_IA32_PERF_CAPABILITIES) & PERF_CAP_FREEZE_SMM)) {
		dbg = rdmsr(MSR_IA32_DEBUGCTL) | DEBUGCTL_FREEZE_SMM;
		wrmsr(MSR_IA32_DEBUGCTL, dbg);
		if (rdmsr(MSR_IA32_DEBUGCTL) == dbg)
			atomic_add32(&pmc_frozen_in_smm, 1);
	}

	wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, 0);
	for (i = 0; i < s->n; i++) {
		sel = s->events[i].event | ((uint32_t) s->events[i].umask << 8) |
		      PERFEVTSEL_OS | PERFEVTSEL_EN;
		/* Enable clear before the counter is written (22.2.1). */
		wrmsr(MSR_IA32_PERFEVTSEL0 + i, 0);
		wrmsr(MSR_IA32_PMC0 + i, 0);
		wrmsr(MSR_IA32_PERFEVTSEL0 + i, sel);
		if (rdmsr(MSR_IA32_PERFEVTSEL0 + i) != sel)
			kept = 0;
	}
	fixed = FIXED_CTR_OS(0) | FIXED_CTR_OS(1);
	wrmsr(MSR_IA32_FIXED_CTR_CTRL, 0);
	wrmsr(MSR_IA32_FIXED_CTR0, 0);
	wrmsr(MSR_IA32_FIXED_CTR0 + 1, 0);
	wrmsr(MSR_IA32_FIXED_CTR_CTRL, fixed);
	global = ((1ULL << s->n) - 1) | GLOBAL_FIXED(0) | GLOBAL_FIXED(1);
	wrmsr(MSR_IA32_PERF_GLOBAL_CTRL, global);
	if (rdmsr(MSR_IA32_FIXED_CTR_CTRL) != fixed ||
	    rdmsr(MSR_IA32_PERF_GLOBAL_CTRL) != global)
		kept = 0;

	atomic_add32(&pmc_programmed, 1);
	if (kept)
		atomic_add32(&pmc_kept, 1);
}

void
pmc_init_cpu(int boot_processor)
{
	struct pmc_found	f = { 0 };

	pmc_choose(&f);
	if (boot_processor) {
		pmc_boot = f;
		pmc_set_counting = f.set;
		pmc_counting = f.set == 0 ? 0 :
			       PMC_FIXED_USED + pmc_sets[f.set].n;
	}
	if (f.set != 0)
		pmc_program(f.set, f.pdcm);
	atomic_add32(&pmc_seen, 1);
}

int
pmc_count(void)
{
	return pmc_counting;
}

int
pmc_role(int i)
{
	int	set = pmc_set_counting;

	if (set == 0 || i < 0 || i >= PMC_FIXED_USED + pmc_sets[set].n)
		return 0;
	if (i == 0)
		return PMC_ROLE_INST;
	if (i == 1)
		return PMC_ROLE_CYCLES;
	return pmc_sets[set].events[i - PMC_FIXED_USED].role;
}

const char *
pmc_name(int i)
{
	int	set = pmc_set_counting;

	if (set == 0 || i < 0 || i >= PMC_FIXED_USED + pmc_sets[set].n)
		return "?";
	if (i == 0)
		return "INST_RETIRED.ANY";
	if (i == 1)
		return "CPU_CLK_UNHALTED.THREAD";
	return pmc_sets[set].events[i - PMC_FIXED_USED].name;
}

void
pmc_read(uint64_t *v)
{
	int	set = pmc_set_counting, i;

	if (set == 0)
		return;
	v[0] = rdpmc(RDPMC_FIXED | 0);
	v[1] = rdpmc(RDPMC_FIXED | 1);
	for (i = 0; i < pmc_sets[set].n; i++)
		v[PMC_FIXED_USED + i] = rdpmc((uint32_t) i);
}

/* What the processor has, in words, for both lines of the report. */
static void
pmc_print_has(const struct pmc_found *f)
{
	if (!f->intel)
		printf("not an Intel processor (family 0x%x model 0x%x)",
		       f->family, f->model);
	else if (f->version == 0)
		printf("CPUID.0AH reports no architectural performance "
		       "monitoring (family 0x%x model 0x%x)", f->family,
		       f->model);
	else
		printf("Intel architectural performance monitoring version "
		       "%u, %u general counters of %u bits and %u fixed of %u, "
		       "pre-defined events %u with absent mask 0x%x; family "
		       "0x%x model 0x%x%s", f->version, f->general,
		       f->general_bits, f->fixed, f->fixed_bits,
		       f->arch_events, f->absent, f->family, f->model,
		       pmc_is_skl_model(f) ? ", whose model events are "
		       "skylake_core.json's (V59)" : ", no model events known");
}

void
pmc_report(void)
{
	const struct pmc_found	*f = &pmc_boot;
	const struct pmc_set	*s = &pmc_sets[f->set];
	uint64_t		v0[PMC_MAX], v1[PMC_MAX], t0, hz = tsc_hz();
	int			i, n = pmc_count(), was, cpu;

	printf("pmc: ");
	if (f->set != 0) {
		printf("counting the %s set at ring 0 on %u of %u processors, "
		       "every register read back as written on %u, frozen while "
		       "in SMM on %u: ", s->what, pmc_programmed, pmc_seen,
		       pmc_kept, pmc_frozen_in_smm);
		for (i = 0; i < n; i++)
			printf("%s%s", i == 0 ? "" : ", ", pmc_name(i));
		printf("; ");
	} else if (f->refusal != PMC_OK) {
		if (f->refusal == PMC_NOT_A_NUMBER)
			printf("pmc= refused -- not a number");
		else
			printf("pmc=%llu refused -- ",
			       (unsigned long long) f->asked_value);
		switch (f->refusal) {
		case PMC_NO_SUCH_SET:
			printf("no such set: 1 is cache, 2 is TLB, 3 is the "
			       "pre-defined events");
			break;
		case PMC_NOT_INTEL:
			printf("no backend for this processor's counters: AMD's "
			       "are the second backend #804 still lacks");
			break;
		case PMC_NO_ARCH:
			printf("nothing to program (under KVM, QEMU's -cpu "
			       "host,pmu=on is what offers it)");
			break;
		case PMC_VERSION_1:
			printf("version 1 has no fixed counters");
			break;
		case PMC_TOO_FEW_GENERAL:
			printf("the %s set needs %d general counters",
			       pmc_sets[f->asked_value].what,
			       pmc_sets[f->asked_value].n);
			break;
		case PMC_TOO_FEW_FIXED:
			printf("two fixed counters are needed");
			break;
		case PMC_EVENT_ABSENT:
			printf("%s is not there, by CPUID.0AH",
			       pmc_sets[f->asked_value].events[f->bad_event].name);
			break;
		case PMC_EVENT_NOT_MODEL:
			printf("%s is a model event, whose code Intel's tables "
			       "give for family 6 models 0x4e, 0x5e, 0x8e, 0x9e, "
			       "0xa5 and 0xa6 only",
			       pmc_sets[f->asked_value].events[f->bad_event].name);
			break;
		}
		printf("; ");
	} else
		printf("off (pmc=1 counts cache events, pmc=2 page walks, "
		       "pmc=3 the pre-defined events); ");
	pmc_print_has(f);
	printf(" (#804)\n");

	if (n == 0 || hz == 0)
		return;

	/*
	 * And the counters seen to count, on this processor over a spin of a
	 * known length with interrupts off -- RDPMC reads the counters of the
	 * processor it runs on: cycles that are not roughly the spin's at the
	 * clock it runs at, or instructions at zero, are counters not counting.
	 */
	was = interrupts_enabled();
	interrupts_disable();
	cpu = cpu_number();	/* the one the spin runs on: read before it can move (#626) */
	pmc_read(v0);
	t0 = rdtsc();
	while (rdtsc() - t0 < hz / 1000000 * PMC_SPIN_US)
		cpu_pause();
	pmc_read(v1);
	if (was)
		interrupts_enable();
	printf("pmc: on processor %d, over a %u us spin with interrupts off:",
	       cpu, PMC_SPIN_US);
	for (i = 0; i < n; i++)
		printf("%s %s %llu", i == 0 ? "" : ",", pmc_name(i),
		       (unsigned long long) (v1[i] - v0[i]));
	printf("%s (#804)\n", v1[0] == v0[0] || v1[1] == v0[1] ?
	       " -- WRONG, a fixed counter did not move" : "");

	pmc_test();
}
