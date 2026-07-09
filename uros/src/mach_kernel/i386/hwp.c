/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * hwp.c — Hardware P-states (Intel HWP / Speed Shift) at CPU bring-up (#358).
 *
 * UrMach never requests P-states.  In the legacy protocol (IA32_PERF_CTL)
 * that means core clocks sit wherever the firmware parked them; with HWP
 * enabled the silicon governs its own frequency.  Modern firmware usually
 * flips HWP on by itself before the OS loads — OMEGA does: the #357 cycle
 * counts showed fully autonomous boost to max turbo — but that is a firmware
 * courtesy, not an architectural contract.  So, once per CPU at bring-up:
 *
 *   - read the truth (IA32_PM_ENABLE) and report it once, from the BSP:
 *     no more inferring the clock story from benchmark cycle counts;
 *   - if the firmware left HWP off, enable it (one-way until reset) and
 *     program a sane autonomous request — min=lowest, max=highest,
 *     desired=0 — so the part is never stuck at the firmware's last ratio;
 *   - if the firmware already enabled it, leave its programming alone.
 *
 * '-E' (hwp_epp_performance) biases the Energy/Performance Preference to 0
 * (performance) in either case, for benchmark runs where ramp latency and
 * boost stickiness matter more than power.
 *
 * Everything is CPUID-gated: on silicon or hypervisors without HWP (QEMU/KVM
 * vCPUs expose CPUID.6 without the HWP bit) no MSR is touched and the BSP
 * prints a single "no HWP" line.  Deliberately no legacy IA32_PERF_CTL
 * fallback: every machine this runs on either has HWP (omen 7th-gen, OMEGA
 * 13th-gen) or hides P-states entirely (KVM), so that path would be dead,
 * untestable code.
 */

#include <kern/misc_protos.h>		/* printf */
#include <i386/proc_reg.h>		/* do_cpuid, rdmsr, wrmsr */
#include <i386/lock.h>			/* atomic_incl */
#include <i386/hwp.h>

#define	MSR_IA32_PM_ENABLE	0x770	/* bit 0: HWP enable (one-way)     */
#define	MSR_IA32_HWP_CAPS	0x771	/* hi/guar/eff/lowest perf, 8b each */
#define	MSR_IA32_HWP_REQUEST	0x774	/* min/max/desired/EPP, 8b each     */

#define	CPUID6_EAX_HWP		(1u << 7)	/* base HWP (PM_ENABLE etc.) */
#define	CPUID6_EAX_HWP_EPP	(1u << 10)	/* EPP field in HWP_REQUEST  */

/* '-E' boot arg.  Lives in .data, not BSS: parse_arguments() runs before
 * i386_init() clears the BSS, so a BSS flag would be wiped after being set
 * (same hazard as cons_is_com1 / fbcons_enabled, #337). */
int	hwp_epp_performance __attribute__((section(".data"))) = 0;

/* '-Q' boot arg: skip enabling HWP entirely -- leave PM_ENABLE as the firmware
 * left it (off on OMEGA).  For a same-binary A/B of the HWP win: the default
 * boot enables HWP, a '-Q' boot does not.  .data, not BSS (#337). */
int	hwp_skip_enable __attribute__((section(".data"))) = 0;

/* CPUs with HWP active after bring-up (BSP + APs); start_other_cpus()
 * prints the summary.  Plain counter, bumped with atomic_incl. */
int	hwp_cpus_on;

void
hwp_init_cpu(boolean_t bsp)
{
	unsigned int	a, b, c, d, eax6;
	unsigned int	lo, hi, cap_lo, cap_hi, req_lo, req_hi;
	boolean_t	already, has_epp;

	do_cpuid(0, 0, &a, &b, &c, &d);
	eax6 = 0;
	if (a >= 6)
		do_cpuid(6, 0, &eax6, &b, &c, &d);
	if ((eax6 & CPUID6_EAX_HWP) == 0) {
		if (bsp)
			printf("hwp: no HWP (cpuid.6 eax=0x%x) -- clocks stay "
			       "as the firmware left them (#358)\n", eax6);
		return;
	}
	has_epp = (eax6 & CPUID6_EAX_HWP_EPP) != 0;

	rdmsr(MSR_IA32_PM_ENABLE, &lo, &hi);
	already = (lo & 1) != 0;

	if (hwp_skip_enable) {
		/* -Q: don't enable, don't program -- report the untouched state
		 * so the A/B baseline is on the record. */
		if (bsp)
			printf("hwp: enable SKIPPED (-Q) -- PM_ENABLE=%u left as "
			       "firmware (A/B baseline) (#358)\n", lo & 1);
		return;
	}

	if (!already)
		wrmsr(MSR_IA32_PM_ENABLE, lo | 1, hi);	/* one-way until reset */

	rdmsr(MSR_IA32_HWP_CAPS, &cap_lo, &cap_hi);
	rdmsr(MSR_IA32_HWP_REQUEST, &req_lo, &req_hi);

	if (!already) {
		/* The firmware left HWP off: hand the hardware the full range
		 * (min=lowest, max=highest) with desired=0 = fully autonomous.
		 * EPP: performance under -E, else the balanced midpoint.  If
		 * the part has no EPP field those bits are reserved -- keep
		 * whatever was there.  req_hi (activity window / package bit)
		 * is preserved as read. */
		unsigned int orig = req_lo;

		req_lo  = (cap_lo >> 24) & 0xff;		/* min = lowest  */
		req_lo |= (cap_lo & 0xff) << 8;			/* max = highest */
		if (has_epp)
			req_lo |= (hwp_epp_performance ? 0x00u : 0x80u) << 24;
		else
			req_lo |= orig & 0xff000000u;
		wrmsr(MSR_IA32_HWP_REQUEST, req_lo, req_hi);
	} else if (hwp_epp_performance && has_epp && (req_lo >> 24) != 0) {
		/* The firmware runs the show; only bias its EPP to
		 * performance for this bench boot. */
		req_lo &= 0x00ffffffu;
		wrmsr(MSR_IA32_HWP_REQUEST, req_lo, req_hi);
	}

	atomic_incl((long *)&hwp_cpus_on, 1);

	if (bsp)
		printf("hwp: %s -- cap hi/gu/eff/lo=%u/%u/%u/%u "
		       "req min/max=%u/%u epp=0x%x (#358)\n",
		       already ? "already enabled by firmware"
			       : "was off -- enabled",
		       cap_lo & 0xff, (cap_lo >> 8) & 0xff,
		       (cap_lo >> 16) & 0xff, (cap_lo >> 24) & 0xff,
		       req_lo & 0xff, (req_lo >> 8) & 0xff, req_lo >> 24);
}
