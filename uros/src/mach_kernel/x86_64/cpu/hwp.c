/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Hardware P-states (Intel HWP, "Speed Shift") at bring-up, and the clock each
 * processor ran at (#736; i386's half is #358).
 *
 * UrMach requests no P-state of its own.  Where the firmware leaves HWP off --
 * omen's and OMEGA's do, #358 found -- the cores run near base clock, and on
 * i386 turning it on made the IPC suites 1.26-1.28x faster on omen and about
 * 1.4-1.8x on OMEGA.  So, once on every processor at bring-up:
 *
 *   - CPUID.06H:EAX[7] says whether there is HWP at all.  Without it --
 *     every KVM vCPU, and AMD, whose counterpart is CPPC -- nothing is
 *     touched;
 *   - IA32_PM_ENABLE says whether the firmware turned it on.  If it did, its
 *     programming is left alone.  If it did not, this turns it on, one way
 *     until reset, and asks for the whole range, autonomously: minimum the
 *     lowest, maximum the highest, desired 0;
 *   - hwp=0 skips both, for the other arm of an A/B, and hwp_epp=N writes N
 *     into the Energy/Performance Preference -- of the request this kernel
 *     writes, or of the firmware's own -- where the processor has one.
 *     i386's -E, as a number.
 *
 * 🔑 AND THE CLOCK, MEASURED.  No number taken on metal said its clock: #446's
 * omen run says of itself that "the core clock is not measured".  So each
 * processor spins for HWP_CLOCK_MS by the TSC, with interrupts off, and reads
 * IA32_APERF and IA32_MPERF on either side.  MPERF counts at the processor's
 * nominal rate, which is the invariant TSC's, and APERF at the rate it ran,
 * so tsc_hz x dAPERF / dMPERF is the clock it ran at: busy, and a few
 * milliseconds after whatever this file asked for.  CPUID.06H:ECX[0] says
 * whether the two counters are there; without it the clock is said to be
 * unmeasured rather than guessed.
 *
 * ⚠️ Deliberately no legacy IA32_PERF_CTL path, for i386's reason: every
 * machine this runs on either has HWP or hides P-states from it (KVM), and
 * that path would be code no boot runs.
 */

#include <stdint.h>

#include <cpus.h>			/* NCPUS */
#include <kern/cpu_number.h>
#include <kern/misc_protos.h>		/* printf, sprintf */

#include <boot/bootarg.h>
#include <cpu/hwp.h>
#include <cpu/regs.h>
#include <sync/atomic.h>
#include <time/tsc.h>

#define	MSR_IA32_MPERF		0xE7
#define	MSR_IA32_APERF		0xE8
#define	MSR_IA32_PM_ENABLE	0x770	/* bit 0: HWP on, one way until reset */
#define	MSR_IA32_HWP_CAPS	0x771	/* highest, guaranteed, efficient, lowest */
#define	MSR_IA32_HWP_REQUEST	0x774	/* minimum, maximum, desired, EPP */

#define	CPUID6_EAX_HWP		(1u << 7)
#define	CPUID6_EAX_HWP_EPP	(1u << 10)
#define	CPUID6_ECX_APERFMPERF	(1u << 0)

#define	HWP_EPP_BALANCED	0x80
#define	HWP_CLOCK_MS		10	/* the spin the clock is measured over */

/* What each processor found or did, for hwp_report(); 0 is not there yet. */
#define	HWP_NONE	1	/* no HWP: left alone */
#define	HWP_SKIPPED	2	/* hwp=0: left alone */
#define	HWP_ALREADY	3	/* on already, by the firmware */
#define	HWP_ENABLED	4	/* off, and turned on here */

static uint8_t		hwp_state[NCPUS];
static uint32_t		hwp_mhz[NCPUS];		/* 0: not measured */
static volatile uint32_t hwp_done;		/* processors past hwp_init_cpu */

/* The boot processor's, for the report: what CPUID said, and the request. */
static uint32_t		hwp_eax6, hwp_ecx6;
static uint64_t		hwp_caps, hwp_req;
static int		hwp_epp_bad;		/* hwp_epp= given and not 0-255 */

/*
 * The clock this processor runs at, busy, in MHz; 0 when it cannot be told.
 */
static uint32_t
hwp_clock(void)
{
	uint64_t	hz = tsc_hz(), t0, a0, m0, a1, m1;
	int		was = interrupts_enabled();

	if (hz == 0)
		return 0;
	interrupts_disable();
	t0 = rdtsc();
	a0 = rdmsr(MSR_IA32_APERF);
	m0 = rdmsr(MSR_IA32_MPERF);
	while (rdtsc() - t0 < hz / 1000 * HWP_CLOCK_MS)
		cpu_pause();
	a1 = rdmsr(MSR_IA32_APERF);
	m1 = rdmsr(MSR_IA32_MPERF);
	if (was)
		interrupts_enable();
	if (m1 <= m0 || a1 <= a0)
		return 0;
	return (uint32_t) (hz / 1000000 * (a1 - a0) / (m1 - m0));
}

/*
 * HWP on, where it is offered and was off; the request programmed as #358's.
 * Returns what it found or did.
 */
static int
hwp_enable(uint32_t eax6, int boot_processor)
{
	uint64_t	pm, caps, req, v, epp = HWP_EPP_BALANCED;
	int		already, has_epp, epp_given = 0, r;

	if ((eax6 & CPUID6_EAX_HWP) == 0)
		return HWP_NONE;
	if (boot_value("hwp", &v) == 1 && v == 0)
		return HWP_SKIPPED;

	r = boot_value("hwp_epp", &v);
	if (r == 1 && v <= 0xff) {
		epp = v;
		epp_given = 1;
	} else if (r != 0 && boot_processor)
		hwp_epp_bad = 1;
	has_epp = (eax6 & CPUID6_EAX_HWP_EPP) != 0;

	pm = rdmsr(MSR_IA32_PM_ENABLE);
	already = (pm & 1) != 0;
	if (!already)
		wrmsr(MSR_IA32_PM_ENABLE, pm | 1);

	caps = rdmsr(MSR_IA32_HWP_CAPS);
	req = rdmsr(MSR_IA32_HWP_REQUEST);
	if (!already) {
		/*
		 * The whole range with desired 0, the hardware choosing within
		 * it.  The upper half -- activity window and package control
		 * -- is kept as read, as i386 keeps it, and the EPP byte where
		 * the processor has none, since it is reserved there.
		 */
		uint64_t	orig = req;

		req &= ~0xffffffffULL;
		req |= (caps >> 24) & 0xff;		/* minimum: lowest  */
		req |= (caps & 0xff) << 8;		/* maximum: highest */
		if (has_epp)
			req |= epp << 24;
		else
			req |= orig & 0xff000000ULL;
		wrmsr(MSR_IA32_HWP_REQUEST, req);
	} else if (epp_given && has_epp) {
		req = (req & ~0xff000000ULL) | (epp << 24);
		wrmsr(MSR_IA32_HWP_REQUEST, req);
	}

	if (boot_processor) {
		hwp_caps = caps;
		hwp_req = req;
	}
	return already ? HWP_ALREADY : HWP_ENABLED;
}

void
hwp_init_cpu(int boot_processor)
{
	int		cpu = cpu_number();
	uint32_t	a, b, c, d, eax6 = 0, ecx6 = 0;

	cpuid(0, &a, &b, &c, &d);
	if (a >= 6)
		cpuid(6, &eax6, &b, &ecx6, &d);
	if (boot_processor) {
		hwp_eax6 = eax6;
		hwp_ecx6 = ecx6;
	}

	hwp_state[cpu] = (uint8_t) hwp_enable(eax6, boot_processor);
	if (ecx6 & CPUID6_ECX_APERFMPERF)
		hwp_mhz[cpu] = hwp_clock();
	atomic_add32(&hwp_done, 1);
}

void
hwp_report(void)
{
	static char	line[NCPUS * 12 + 160];
	unsigned	n = hwp_done, count[HWP_ENABLED + 1] = { 0 }, measured = 0;
	char		*p = line;
	int		i;

	for (i = 0; i < NCPUS; i++)
		count[hwp_state[i]]++;

	if (count[HWP_ENABLED] + count[HWP_ALREADY] != 0)
		printf("hwp: on in %u of %u processors, %u turned on here and "
		       "%u already by the firmware%s; the boot processor's "
		       "highest/guaranteed/efficient/lowest %u/%u/%u/%u, "
		       "request minimum/maximum %u/%u, EPP 0x%x (#736)\n",
		       count[HWP_ENABLED] + count[HWP_ALREADY], n,
		       count[HWP_ENABLED], count[HWP_ALREADY],
		       hwp_epp_bad ? ", hwp_epp= ignored: not 0 to 255" : "",
		       (unsigned) (hwp_caps & 0xff),
		       (unsigned) ((hwp_caps >> 8) & 0xff),
		       (unsigned) ((hwp_caps >> 16) & 0xff),
		       (unsigned) ((hwp_caps >> 24) & 0xff),
		       (unsigned) (hwp_req & 0xff),
		       (unsigned) ((hwp_req >> 8) & 0xff),
		       (unsigned) ((hwp_req >> 24) & 0xff));
	else if (count[HWP_SKIPPED] != 0)
		printf("hwp: left as the firmware left it on %u of %u "
		       "processors (hwp=0), the other arm of an A/B (#736)\n",
		       count[HWP_SKIPPED], n);
	else
		printf("hwp: no HWP on any of %u processors (cpuid.6 eax=0x%x): "
		       "their clocks stay as the firmware left them (#736)\n",
		       n, hwp_eax6);

	for (i = 0; i < NCPUS; i++) {
		if (hwp_mhz[i] == 0)
			continue;
		p += sprintf(p, "%s %d:%u", measured == 0 ? "" : ",", i,
			     hwp_mhz[i]);
		measured++;
	}
	if (measured != 0)
		printf("clock: at bring-up, spinning %u ms, by APERF/MPERF "
		       "against a TSC of %u MHz, processor:MHz%s (#736)\n",
		       HWP_CLOCK_MS, (unsigned) (tsc_hz() / 1000000), line);
	else
		printf("clock: not measured -- %s (#736)\n",
		       (hwp_ecx6 & CPUID6_ECX_APERFMPERF)
		       ? "the TSC is not calibrated, or the counters did not move"
		       : "no APERF/MPERF on this processor (cpuid.6 ecx bit 0)");
}
