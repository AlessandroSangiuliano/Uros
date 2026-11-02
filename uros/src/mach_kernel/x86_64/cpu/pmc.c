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
 *   - CPUID says what there is: on Intel the architectural performance
 *     monitoring of leaf 0AH (Intel SDM 325462-093US, Vol. 3B 22.2.1 and
 *     22.2.2) -- the version in EAX[7:0], the general counters' number and
 *     width in EAX[15:8] and EAX[23:16], the fixed ones' in EDX[4:0] and
 *     EDX[12:5], and in EBX a bit SET for each pre-defined event that is not
 *     there (Table 22-3); on AMD the six core counters of
 *     CPUID Fn8000_0001_ECX[PerfCtrExtCore] (AMD64 APM Vol. 2, 24593 Rev.
 *     3.42, 13.2);
 *   - pmc=N picks an event set (1 cache, 2 page walks, 3 the pre-defined
 *     events or AMD's nearest), each event named by what it measures, which
 *     each processor writes into its own event selects, counting at ring 0
 *     only, with instructions retired and unhalted core cycles beside them --
 *     on Intel in the first two fixed counters (IA32_FIXED_CTR_CTRL, 38DH)
 *     and turned on by IA32_PERF_GLOBAL_CTRL (38FH), on AMD, which has no
 *     fixed counter before PerfMonV2, in the first two of the six;
 *   - an event that is not the processor's is refused, and the refusal is
 *     said.  A counter never programmed reads zero, and a zero reads as
 *     "nothing happened".
 *
 * Every processor decides for itself, from its own CPUID and the command
 * line, which all of them share: the boot processor's decision is kept for
 * the report, but no processor waits for it.
 *
 * WHOSE CODES.  Intel's events beyond the pre-defined ones are model events,
 * with the codes of Intel's tables for the 6th-generation core and the models
 * its mapfile sends there (intel/perfmon, SKL/events/skylake_core.json, V59 of
 * 04/04/2024; mapfile.csv: GenuineIntel-6-4E, -5E, -8E, -9E, -A5, -A6), omen's
 * i7-7700HQ (6-9E) among them.  AMD's codes vary by implementation (24593,
 * 13.2.1) and are each processor's Processor Programming Reference's: 55922
 * Rev. 3.06 for Family 17h Model 60h (Zen 2, pavillion's Ryzen 5 4600H) and
 * 56569 Rev. 3.03 for Family 19h with ExtModel 5h (Zen 3, victus's Ryzen 5
 * 5600H, model 50h: the PPR is written for model 51h and says the base model
 * "varies with product").  A model none of those tables covers is refused.
 */

#include <stdint.h>

#include <cpus.h>			/* NCPUS */
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>		/* printf, sprintf */

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

/* AMD's six core counters (24593, A.6): event select n at 0200h + 2n, counter n beside it. */
#define	MSR_AMD_PERF_EVT_SEL0		0xC0010200
#define	MSR_AMD_PERF_CTR0		0xC0010201

#define	PERFEVTSEL_OS		(1u << 17)	/* count at ring 0, Intel and AMD alike */
#define	PERFEVTSEL_EN		(1u << 22)
#define	FIXED_CTR_OS(n)		(1ULL << (4 * (n)))	/* ENABLE = 01b: ring 0 */
#define	GLOBAL_FIXED(n)		(1ULL << (32 + (n)))
#define	RDPMC_FIXED		(1u << 30)
#define	CPUID1_ECX_PDCM		(1u << 15)
#define	PERF_CAP_FREEZE_SMM	(1ULL << 12)	/* DEBUGCTL.FREEZE_WHILE_SMM is there */
#define	DEBUGCTL_FREEZE_SMM	(1ULL << 14)
#define	CPUID_80000001_ECX_PERFCTR_EXT_CORE (1u << 23)

#define	PMC_FIXED_USED		2	/* instructions retired, unhalted cycles */
#define	PMC_GENERAL_MAX		(PMC_MAX - PMC_FIXED_USED)
#define	PMC_AMD_COUNTERS	6

#define	PMC_SPIN_US		1000	/* the check in pmc_report() */

#define	PMC_INTEL	1
#define	PMC_AMD		2

struct pmc_event {
	const char	*name;
	uint16_t	event;		/* AMD's are twelve bits (EVENT_SELECT[11:0]) */
	uint8_t		umask;
	int8_t		arch_bit;	/* its bit in CPUID.0AH:EBX; -1: a model event */
	uint8_t		role;		/* PMC_ROLE_*, what pmc_test.c loads it with */
};

/* Intel, pmc=1: where a load's line came from. */
static const struct pmc_event pmc_set_cache[] = {
	{ "L1D.REPLACEMENT",			0x51, 0x01, -1, PMC_ROLE_L1D_MISS },
	{ "L2_RQSTS.MISS",			0x24, 0x3f, -1, PMC_ROLE_L2_MISS },
	{ "LONGEST_LAT_CACHE.MISS",		0x2e, 0x41,  4, PMC_ROLE_LLC_MISS },
	{ "MEM_LOAD_L3_HIT_RETIRED.XSNP_HITM",	0xd2, 0x04, -1, PMC_ROLE_HITM },
};

/* Intel, pmc=2: page walks, for data and for code. */
static const struct pmc_event pmc_set_tlb[] = {
	{ "DTLB_LOAD_MISSES.WALK_COMPLETED",	0x08, 0x0e, -1, PMC_ROLE_DTLB_LOAD_WALK },
	{ "DTLB_STORE_MISSES.WALK_COMPLETED",	0x49, 0x0e, -1, PMC_ROLE_DTLB_STORE_WALK },
	{ "ITLB_MISSES.WALK_COMPLETED",		0x85, 0x0e, -1, PMC_ROLE_ITLB_WALK },
	{ "LONGEST_LAT_CACHE.MISS",		0x2e, 0x41,  4, PMC_ROLE_LLC_MISS },
};

/*
 * Intel, pmc=3: the pre-defined events alone (Table 22-3), the same codes on
 * every Intel processor that has them, so a set any of them can count.
 */
static const struct pmc_event pmc_set_arch[] = {
	{ "LLC Reference",			0x2e, 0x4f,  3, PMC_ROLE_LLC_REF },
	{ "LLC Misses",				0x2e, 0x41,  4, PMC_ROLE_LLC_MISS },
	{ "Branch Instruction Retired",		0xc4, 0x00,  5, PMC_ROLE_BRANCH },
	{ "Branch Misses Retired",		0xc5, 0x00,  6, PMC_ROLE_BRANCH_MISS },
};

/*
 * AMD.  The core counters see no L2 or L3 miss as such: a line filled into
 * the data cache is counted by where it came from (PMCx043, demand fills by
 * data source), so a miss of each level is the fills from beyond it.  The
 * names are the PPR's mnemonics with the unit mask's sources after a colon.
 * Zen 2 and Zen 3 differ in PMCx043's sources -- Zen 3 has a bit for another
 * CCX's cache -- and in PMCx085's, so each has its table.  No load of another
 * core's modified line, and no split of the data page walks into loads' and
 * stores': the core counters have neither, and the sets say so by not having
 * them.
 */
static const struct pmc_event pmc_zen_fixed[PMC_FIXED_USED] = {
	{ "ExRetInstr",				0x0c0, 0x00, -1, PMC_ROLE_INST },
	{ "LsNotHaltedCyc",			0x076, 0x00, -1, PMC_ROLE_CYCLES },
};

/* Zen 2, PPR 55922 Rev. 3.06: PMCx043 sources 0 L2, 1 another cache, 3 and 6 DRAM, 4 remote cache. */
static const struct pmc_event pmc_zen2_cache[] = {
	{ "LsRefillsFromSys:all",		0x043, 0x5b, -1, PMC_ROLE_L1D_MISS },
	{ "L2CacheReqStat:LsRdBlkC",		0x064, 0x08, -1, PMC_ROLE_L2_MISS },
	{ "LsRefillsFromSys:LclCache",		0x043, 0x02, -1, PMC_ROLE_L3_OR_PEER },
	{ "LsRefillsFromSys:Dram",		0x043, 0x48, -1, PMC_ROLE_LLC_MISS },
};
static const struct pmc_event pmc_zen2_tlb[] = {
	{ "LsL1DTlbMiss:L2Miss",		0x045, 0xf0, -1, PMC_ROLE_DTLB_WALK },
	{ "LsL1DTlbMiss:L2Hit",			0x045, 0x0f, -1, PMC_ROLE_DTLB_L2_HIT },
	{ "BpL1TlbMissL2TlbMiss",		0x085, 0x07, -1, PMC_ROLE_ITLB_WALK },
	{ "LsRefillsFromSys:Dram",		0x043, 0x48, -1, PMC_ROLE_LLC_MISS },
};
static const struct pmc_event pmc_zen2_arch[] = {
	{ "LsRefillsFromSys:beyondL2",		0x043, 0x5a, -1, PMC_ROLE_LLC_REF },
	{ "LsRefillsFromSys:Dram",		0x043, 0x48, -1, PMC_ROLE_LLC_MISS },
	{ "ExRetBrn",				0x0c2, 0x00, -1, PMC_ROLE_BRANCH },
	{ "ExRetBrnMisp",			0x0c3, 0x00, -1, PMC_ROLE_BRANCH_MISS },
};

/* Zen 3, PPR 56569 Rev. 3.03: PMCx043 sources 0 L2, 1 L3 or a CCX peer, 2 another CCX, 3 and 6 DRAM, 4 remote. */
static const struct pmc_event pmc_zen3_cache[] = {
	{ "LsDmndFillsFromSys:all",		0x043, 0x5f, -1, PMC_ROLE_L1D_MISS },
	{ "L2CacheReqStat:LsRdBlkC",		0x064, 0x08, -1, PMC_ROLE_L2_MISS },
	{ "LsDmndFillsFromSys:IntCache",	0x043, 0x06, -1, PMC_ROLE_L3_OR_PEER },
	{ "LsDmndFillsFromSys:MemIo",		0x043, 0x48, -1, PMC_ROLE_LLC_MISS },
};
static const struct pmc_event pmc_zen3_tlb[] = {
	{ "LsL1DTlbMiss:L2Miss",		0x045, 0xf0, -1, PMC_ROLE_DTLB_WALK },
	{ "LsL1DTlbMiss:L2Hit",			0x045, 0x0f, -1, PMC_ROLE_DTLB_L2_HIT },
	{ "BpL1TlbMissL2TlbMiss",		0x085, 0x0f, -1, PMC_ROLE_ITLB_WALK },
	{ "LsDmndFillsFromSys:MemIo",		0x043, 0x48, -1, PMC_ROLE_LLC_MISS },
};
static const struct pmc_event pmc_zen3_arch[] = {
	{ "LsDmndFillsFromSys:beyondL2",	0x043, 0x5e, -1, PMC_ROLE_LLC_REF },
	{ "LsDmndFillsFromSys:MemIo",		0x043, 0x48, -1, PMC_ROLE_LLC_MISS },
	{ "ExRetBrn",				0x0c2, 0x00, -1, PMC_ROLE_BRANCH },
	{ "ExRetBrnMisp",			0x0c3, 0x00, -1, PMC_ROLE_BRANCH_MISS },
};

struct pmc_set {
	const char		*what;
	const struct pmc_event	*events;
	int			n;
};

#define	PMC_SET(w, e)	{ w, e, sizeof (e) / sizeof (e[0]) }
#define	PMC_SETS	4	/* none, cache, TLB, architectural */

static const struct pmc_set pmc_sets_intel[PMC_SETS] = {
	{ 0, 0, 0 },
	PMC_SET("cache", pmc_set_cache),
	PMC_SET("TLB", pmc_set_tlb),
	PMC_SET("architectural", pmc_set_arch),
};
static const struct pmc_set pmc_sets_zen2[PMC_SETS] = {
	{ 0, 0, 0 },
	PMC_SET("cache", pmc_zen2_cache),
	PMC_SET("TLB", pmc_zen2_tlb),
	PMC_SET("architectural", pmc_zen2_arch),
};
static const struct pmc_set pmc_sets_zen3[PMC_SETS] = {
	{ 0, 0, 0 },
	PMC_SET("cache", pmc_zen3_cache),
	PMC_SET("TLB", pmc_zen3_tlb),
	PMC_SET("architectural", pmc_zen3_arch),
};

/* Family 6 models whose core events are skylake_core.json's, by Intel's mapfile. */
static const uint8_t	pmc_skl_models[] = { 0x4e, 0x5e, 0x8e, 0x9e, 0xa5, 0xa6 };

/* Why pmc=N was refused. */
#define	PMC_OK			0
#define	PMC_NOT_A_NUMBER	1
#define	PMC_NO_SUCH_SET		2
#define	PMC_NO_BACKEND		3	/* neither Intel's nor AMD's */
#define	PMC_NO_ARCH		4
#define	PMC_VERSION_1		5
#define	PMC_TOO_FEW_GENERAL	6
#define	PMC_TOO_FEW_FIXED	7
#define	PMC_EVENT_ABSENT	8	/* a pre-defined event CPUID says is not there */
#define	PMC_EVENT_NOT_MODEL	9	/* a model event, and not this model's */
#define	PMC_AMD_NO_EXT_CORE	10	/* the legacy four counters only */
#define	PMC_AMD_NO_PPR		11	/* a family and model no table here covers */

/* What one processor found, and what it chose. */
struct pmc_found {
	int		vendor;			/* PMC_INTEL, PMC_AMD or 0 */
	uint32_t	family, model;
	uint32_t	version, general, general_bits, fixed, fixed_bits;
	uint32_t	arch_events, absent;	/* EAX[31:24], EBX */
	int		ext_core;		/* AMD: PerfCtrExtCore */
	const struct pmc_set *sets;		/* the table its events come from, 0 for none */
	int		asked;			/* pmc= given */
	uint64_t	asked_value;
	int		set;			/* the set counting, 0 for none */
	int		refusal, bad_event;
	int		pdcm;			/* CPUID.01H:ECX[15]: IA32_PERF_CAPABILITIES is there */
};

static struct pmc_found	pmc_boot;		/* the boot processor's, for the report */
static volatile int	pmc_set_counting;	/* the same set, read by pmc_read() */
static const struct pmc_set *pmc_sets_counting;	/* and its table */
static int		pmc_vendor_counting;
int			pmc_counting;		/* pmc_count(), for the profiler */

static volatile uint32_t pmc_seen;		/* processors past pmc_init_cpu() */
static volatile uint32_t pmc_programmed;	/* of them, the ones that programmed */
static volatile uint32_t pmc_kept;		/* ... and read every register back as written */
static volatile uint32_t pmc_frozen_in_smm;	/* ... and froze them while in SMM */

static int
pmc_is_skl_model(const struct pmc_found *f)
{
	unsigned	i;

	if (f->vendor != PMC_INTEL || f->family != 6)
		return 0;
	for (i = 0; i < sizeof (pmc_skl_models); i++)
		if (pmc_skl_models[i] == f->model)
			return 1;
	return 0;
}

/* The AMD table a family and model's PPR gives, or 0. */
static const struct pmc_set *
pmc_amd_sets(const struct pmc_found *f)
{
	if (f->family == 0x17 && f->model == 0x60)
		return pmc_sets_zen2;
	if (f->family == 0x19 && (f->model >> 4) == 0x5)
		return pmc_sets_zen3;
	return 0;
}

/* What this processor has, and the set it is to count, from CPUID and pmc=. */
static void
pmc_choose(struct pmc_found *f)
{
	uint32_t	a, b, c, d, max, max_ext;
	int		r, i;
	uint64_t	v = 0;
	const struct pmc_set *s;

	cpuid(0, &max, &b, &c, &d);
	/* "GenuineIntel" and "AuthenticAMD", in EBX, EDX, ECX. */
	if (b == 0x756e6547 && d == 0x49656e69 && c == 0x6c65746e)
		f->vendor = PMC_INTEL;
	else if (b == 0x68747541 && d == 0x69746e65 && c == 0x444d4163)
		f->vendor = PMC_AMD;
	cpuid(1, &a, &b, &c, &d);
	f->pdcm = (c & CPUID1_ECX_PDCM) != 0;
	f->family = (a >> 8) & 0xf;
	f->model = (a >> 4) & 0xf;
	if (f->family == 0xf)
		f->family += (a >> 20) & 0xff;
	if (f->family == 6 || f->family >= 0xf)
		f->model |= ((a >> 16) & 0xf) << 4;
	if (f->vendor == PMC_INTEL && max >= 0xa) {
		cpuid(0xa, &a, &b, &c, &d);
		f->version = a & 0xff;
		f->general = (a >> 8) & 0xff;
		f->general_bits = (a >> 16) & 0xff;
		f->arch_events = (a >> 24) & 0xff;
		f->absent = b;
		f->fixed = d & 0x1f;
		f->fixed_bits = (d >> 5) & 0xff;
		f->sets = pmc_sets_intel;
	} else if (f->vendor == PMC_AMD) {
		cpuid(0x80000000, &max_ext, &b, &c, &d);
		if (max_ext >= 0x80000001) {
			cpuid(0x80000001, &a, &b, &c, &d);
			f->ext_core = (c & CPUID_80000001_ECX_PERFCTR_EXT_CORE) != 0;
		}
		f->general = f->ext_core ? PMC_AMD_COUNTERS : 4;
		f->sets = pmc_amd_sets(f);
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
	if (f->vendor == PMC_AMD) {
		if (!f->ext_core)
			f->refusal = PMC_AMD_NO_EXT_CORE;
		else if (f->sets == 0)
			f->refusal = PMC_AMD_NO_PPR;
		if (f->refusal == PMC_OK)
			f->set = (int) v;
		return;
	}
	if (f->vendor != PMC_INTEL) {
		f->refusal = PMC_NO_BACKEND;
		return;
	}
	s = &pmc_sets_intel[v];
	if (f->version == 0)
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
 * This processor's Intel counters, the set's events in the general ones.
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
pmc_program_intel(const struct pmc_set *s, int pdcm)
{
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

/*
 * This processor's AMD core counters: instructions and cycles in 0 and 1,
 * the set's events in 2 to 5.  Each select is written with its enable clear
 * first and the counter zeroed, then enabled, at ring 0 (OS) only, guest and
 * host alike (HG_ONLY 00b, 24593 Table 13-4).  No freeze in SMM: the APM's
 * counters chapter has none, and the report says the processor's SMIs are
 * counted.
 */
static void
pmc_program_amd(const struct pmc_set *s)
{
	const struct pmc_event	*e;
	uint64_t		sel;
	int			i, kept = 1;

	for (i = 0; i < PMC_FIXED_USED + s->n; i++) {
		e = i < PMC_FIXED_USED ? &pmc_zen_fixed[i] :
		    &s->events[i - PMC_FIXED_USED];
		sel = (e->event & 0xff) | ((uint32_t) e->umask << 8) |
		      PERFEVTSEL_OS | PERFEVTSEL_EN |
		      ((uint64_t) (e->event >> 8) << 32);
		wrmsr(MSR_AMD_PERF_EVT_SEL0 + 2 * i, 0);
		wrmsr(MSR_AMD_PERF_CTR0 + 2 * i, 0);
		wrmsr(MSR_AMD_PERF_EVT_SEL0 + 2 * i, sel);
		if (rdmsr(MSR_AMD_PERF_EVT_SEL0 + 2 * i) != sel)
			kept = 0;
	}

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
		pmc_vendor_counting = f.vendor;
		pmc_sets_counting = f.set == 0 ? 0 : f.sets;
		pmc_set_counting = f.set;
		pmc_counting = f.set == 0 ? 0 :
			       PMC_FIXED_USED + f.sets[f.set].n;
	}
	if (f.set != 0) {
		if (f.vendor == PMC_AMD)
			pmc_program_amd(&f.sets[f.set]);
		else
			pmc_program_intel(&f.sets[f.set], f.pdcm);
	}
	atomic_add32(&pmc_seen, 1);
}

int
pmc_count(void)
{
	return pmc_counting;
}

/* The event counter i counts: one of the two fixed roles, or the set's. */
static const struct pmc_event *
pmc_event_at(int i)
{
	int	set = pmc_set_counting;

	if (set == 0 || i < 0 || i >= PMC_FIXED_USED + pmc_sets_counting[set].n)
		return 0;
	if (i >= PMC_FIXED_USED)
		return &pmc_sets_counting[set].events[i - PMC_FIXED_USED];
	if (pmc_vendor_counting == PMC_AMD)
		return &pmc_zen_fixed[i];
	return 0;
}

int
pmc_role(int i)
{
	const struct pmc_event	*e = pmc_event_at(i);

	if (e != 0)
		return e->role;
	if (pmc_set_counting == 0 || i < 0 || i >= PMC_FIXED_USED)
		return 0;
	return i == 0 ? PMC_ROLE_INST : PMC_ROLE_CYCLES;
}

const char *
pmc_name(int i)
{
	const struct pmc_event	*e = pmc_event_at(i);

	if (e != 0)
		return e->name;
	if (pmc_set_counting == 0 || i < 0 || i >= PMC_FIXED_USED)
		return "?";
	return i == 0 ? "INST_RETIRED.ANY" : "CPU_CLK_UNHALTED.THREAD";
}

void
pmc_read(uint64_t *v)
{
	int	set = pmc_set_counting, i, n;

	if (set == 0)
		return;
	n = pmc_sets_counting[set].n;
	if (pmc_vendor_counting == PMC_AMD) {
		for (i = 0; i < PMC_FIXED_USED + n; i++)
			v[i] = rdpmc((uint32_t) i);
		return;
	}
	v[0] = rdpmc(RDPMC_FIXED | 0);
	v[1] = rdpmc(RDPMC_FIXED | 1);
	for (i = 0; i < n; i++)
		v[PMC_FIXED_USED + i] = rdpmc((uint32_t) i);
}

/* What the processor has, in words, into buf: the report's last line. */
static int
pmc_has(char *buf, const struct pmc_found *f)
{
	if (f->vendor == PMC_AMD)
		return sprintf(buf, "AMD core performance counters, %u of them%s; "
		       "family 0x%x model 0x%x%s", f->general,
		       f->ext_core ? " (PerfCtrExtCore)" : ", the legacy four",
		       f->family, f->model,
		       f->sets == pmc_sets_zen2 ? ", whose events are PPR "
		       "55922 Rev. 3.06's (Family 17h Model 60h)" :
		       f->sets == pmc_sets_zen3 ? ", whose events are PPR "
		       "56569 Rev. 3.03's (Family 19h, ExtModel 5h)" :
		       ", no PPR's events known here");
	if (f->vendor != PMC_INTEL)
		return sprintf(buf, "neither an Intel nor an AMD processor "
		       "(family 0x%x model 0x%x)", f->family, f->model);
	if (f->version == 0)
		return sprintf(buf, "CPUID.0AH reports no architectural "
		       "performance monitoring (family 0x%x model 0x%x)",
		       f->family, f->model);
	return sprintf(buf, "Intel architectural performance monitoring "
	       "version %u, %u general counters of %u bits and %u fixed of %u, "
	       "pre-defined events %u with absent mask 0x%x; family 0x%x model "
	       "0x%x%s", f->version, f->general, f->general_bits, f->fixed,
	       f->fixed_bits, f->arch_events, f->absent, f->family, f->model,
	       pmc_is_skl_model(f) ? ", whose model events are "
	       "skylake_core.json's (V59)" : ", no model events known");
}

/*
 * 🔴 EACH LINE ONE PRINTF, AND SHORT.  The report was one line put together
 * by a dozen printf()s, and in QEMU another thread's output came in after the
 * third ("<-cut", #757) -- and it ran past the console's 256 bytes (#802).  So
 * three lines, each built here first and printed whole.
 */
static char	pmc_line[256];

void
pmc_report(void)
{
	const struct pmc_found	*f = &pmc_boot;
	uint64_t		v0[PMC_MAX], v1[PMC_MAX], t0, hz = tsc_hz();
	int			i, n = pmc_count(), was, cpu;
	char			*p = pmc_line;

	if (f->set != 0) {
		p += sprintf(p, "pmc: counting the %s set at ring 0 on %u of %u "
			     "processors, every register read back as written "
			     "on %u, ", f->sets[f->set].what, pmc_programmed,
			     pmc_seen, pmc_kept);
		if (f->vendor == PMC_AMD)
			p += sprintf(p, "not frozen in SMM: AMD's core counters "
				     "have no such control");
		else
			p += sprintf(p, "frozen while in SMM on %u",
				     pmc_frozen_in_smm);
		printf("%s (#804)\n", pmc_line);
		p = pmc_line;
		p += sprintf(p, "pmc: the counters, in order:");
		for (i = 0; i < n; i++)
			p += sprintf(p, " %s", pmc_name(i));
		printf("%s (#804)\n", pmc_line);
	} else if (f->refusal != PMC_OK) {
		if (f->refusal == PMC_NOT_A_NUMBER)
			p += sprintf(p, "pmc: pmc= refused -- not a number");
		else
			p += sprintf(p, "pmc: pmc=%llu refused -- ",
				     (unsigned long long) f->asked_value);
		switch (f->refusal) {
		case PMC_NO_SUCH_SET:
			p += sprintf(p, "no such set: 1 is cache, 2 is TLB, 3 is "
				     "the pre-defined events");
			break;
		case PMC_NO_BACKEND:
			p += sprintf(p, "no backend for this processor's "
				     "counters: there are Intel's and AMD's");
			break;
		case PMC_NO_ARCH:
			p += sprintf(p, "nothing to program (under KVM, QEMU's "
				     "-cpu host,pmu=on is what offers it)");
			break;
		case PMC_VERSION_1:
			p += sprintf(p, "version 1 has no fixed counters");
			break;
		case PMC_TOO_FEW_GENERAL:
			p += sprintf(p, "the %s set needs %d general counters",
				     pmc_sets_intel[f->asked_value].what,
				     pmc_sets_intel[f->asked_value].n);
			break;
		case PMC_TOO_FEW_FIXED:
			p += sprintf(p, "two fixed counters are needed");
			break;
		case PMC_EVENT_ABSENT:
			p += sprintf(p, "%s is not there, by CPUID.0AH",
				     pmc_sets_intel[f->asked_value].events[f->bad_event].name);
			break;
		case PMC_EVENT_NOT_MODEL:
			p += sprintf(p, "%s is a model event, whose code Intel's "
				     "tables give for family 6 models 0x4e, 0x5e, "
				     "0x8e, 0x9e, 0xa5 and 0xa6 only",
				     pmc_sets_intel[f->asked_value].events[f->bad_event].name);
			break;
		case PMC_AMD_NO_EXT_CORE:
			p += sprintf(p, "the legacy four counters only, and a set "
				     "needs six: CPUID Fn8000_0001_ECX"
				     "[PerfCtrExtCore] is clear");
			break;
		case PMC_AMD_NO_PPR:
			p += sprintf(p, "no event codes for this family and model: "
				     "the PPRs read here are 55922 (17h model 60h) "
				     "and 56569 (19h, ExtModel 5h)");
			break;
		}
		printf("%s (#804)\n", pmc_line);
	} else
		printf("pmc: off (pmc=1 counts cache events, pmc=2 page walks, "
		       "pmc=3 the pre-defined events) (#804)\n");
	p = pmc_line;
	p += sprintf(p, "pmc: the processor: ");
	p += pmc_has(p, f);
	printf("%s (#804)\n", pmc_line);

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
	p = pmc_line;
	p += sprintf(p, "pmc: on processor %d, over a %u us spin with "
		     "interrupts off:", cpu, PMC_SPIN_US);
	for (i = 0; i < n; i++)
		p += sprintf(p, "%s %llu", i == 0 ? "" : ",",
			     (unsigned long long) (v1[i] - v0[i]));
	printf("%s%s (#804)\n", pmc_line, v1[0] == v0[0] || v1[1] == v0[1] ?
	       " -- WRONG, an instruction or cycle counter did not move" : "");

	{
		uint64_t	t;

		if (boot_value("pmc_test", &t) == 1 && t != 0)
			pmc_test();
	}
}
