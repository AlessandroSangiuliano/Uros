/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the processor and the hypervisor say about the clocks' frequencies
 * (#508).  Readers only; see freq_source.h.
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <pmap/layout.h>
#include <time/freq_source.h>

#define CPUID_HYPERVISOR_BIT	(1U << 31)	/* CPUID.1:ECX */
#define HV_LEAF_BASE		0x40000000U
#define HV_LEAF_TIMING		0x40000010U
#define KVM_LEAF_FEATURES	0x40000001U
#define KVM_FEATURE_CLOCKSOURCE2	(1U << 3)
#define MSR_KVM_SYSTEM_TIME_NEW	0x4b564d01U

void freq_cpuid_read(struct freq_cpuid *out)
{
	uint32_t a, b, c, d;

	*out = (struct freq_cpuid){ 0 };

	cpuid(0, &a, &b, &c, &d);
	out->max_leaf = a;

	if (out->max_leaf >= 0x15) {
		out->has_15 = 1;
		cpuid(0x15, &a, &b, &c, &d);
		out->tsc_denominator = a;
		out->tsc_numerator = b;
		out->crystal_hz = c;
	}
	if (out->max_leaf >= 0x16) {
		out->has_16 = 1;
		cpuid(0x16, &a, &b, &c, &d);
		out->base_mhz = a & 0xffff;
		out->max_mhz = b & 0xffff;
		out->bus_mhz = c & 0xffff;
	}
}

/*
 * The page the host writes KVM's clock parameters into.
 *
 * Thirty-two bytes, and aligned to sixty-four so it cannot straddle a page:
 * the MSR takes one physical address, and the host writes the structure
 * there as one piece.  Volatile because the writer is not this program.
 */
struct pvclock_time_info {
	uint32_t	version;
	uint32_t	pad0;
	uint64_t	tsc_timestamp;
	uint64_t	system_time;
	uint32_t	tsc_to_system_mul;
	int8_t		tsc_shift;
	uint8_t		flags;
	uint8_t		pad[2];
} __attribute__((packed));

_Static_assert(sizeof(struct pvclock_time_info) == 32,
	       "KVM's clock structure is thirty-two bytes");

static volatile struct pvclock_time_info kvmclock_page
	__attribute__((aligned(64)));

/*
 * The frequency a conversion implies.
 *
 * nanoseconds = ((ticks << shift) * mul) >> 32, so one tick is
 * mul * 2^shift / 2^32 ns, and the rate is the reciprocal of that.  2^32 * 10^9
 * is about 4.3e18 and fits in 64 bits; the division comes first so the shift
 * never has to carry it.
 */
static uint64_t pvclock_implied_hz(uint32_t mul, int8_t shift)
{
	uint64_t hz;

	if (mul == 0)
		return 0;

	hz = (1000000000ULL << 32) / mul;
	if (shift < 0)
		hz <<= -shift;
	else
		hz >>= shift;
	return hz;
}

/*
 * Ask KVM's clock once, and switch it off again.
 *
 * The host fills the page before this processor next runs guest code, so it
 * is filled by the time the write returns; the bounded wait is for a host
 * that does it later, and a version of zero afterwards is reported as what it
 * is -- the host wrote nothing -- not turned into a frequency of zero.  The
 * version protocol is the one the ABI states: odd means the host is in the
 * middle of an update, and a read is good only if the version was even and
 * did not change across it.
 *
 * ⚠️ Switched off because nothing uses it yet.  Leaving the host writing into
 * a page of the kernel image, for a clock nobody reads, is a standing
 * obligation taken on for a census.
 */
static void kvmclock_read(struct freq_hypervisor *out)
{
	uint32_t	v0, v1, mul;
	int8_t		shift;
	uint8_t		flags;
	unsigned	spins;

	out->kvmclock_asked = 1;
	wrmsr(MSR_KVM_SYSTEM_TIME_NEW,
	      kernel_va_to_phys(&kvmclock_page) | 1);

	for (spins = 0; spins < 100000; spins++) {
		v0 = kvmclock_page.version;
		__asm__ volatile("" ::: "memory");
		mul = kvmclock_page.tsc_to_system_mul;
		shift = kvmclock_page.tsc_shift;
		flags = kvmclock_page.flags;
		__asm__ volatile("" ::: "memory");
		v1 = kvmclock_page.version;
		if (v0 != 0 && (v0 & 1) == 0 && v0 == v1)
			break;
		__asm__ volatile("pause");
	}

	wrmsr(MSR_KVM_SYSTEM_TIME_NEW, 0);

	if (v0 == 0 || (v0 & 1) != 0 || v0 != v1)
		return;

	out->kvmclock_version = v0;
	out->kvmclock_mul = mul;
	out->kvmclock_shift = shift;
	out->kvmclock_flags = flags;
	out->kvmclock_tsc_hz = pvclock_implied_hz(mul, shift);
}

void freq_hypervisor_read(struct freq_hypervisor *out)
{
	uint32_t a, b, c, d;
	unsigned i;

	*out = (struct freq_hypervisor){ 0 };

	cpuid(1, &a, &b, &c, &d);
	if ((c & CPUID_HYPERVISOR_BIT) == 0)
		return;
	out->present = 1;

	cpuid(HV_LEAF_BASE, &a, &b, &c, &d);
	out->max_leaf = a;
	for (i = 0; i < 4; i++) {
		out->signature[i] = (char)(b >> (8 * i));
		out->signature[4 + i] = (char)(c >> (8 * i));
		out->signature[8 + i] = (char)(d >> (8 * i));
	}
	out->signature[12] = '\0';

	/* "KVMKVMKVM" and three NULs, as the ABI spells it. */
	out->is_kvm = b == 0x4b4d564b && c == 0x564b4d56 && d == 0x0000004d;

	if (out->max_leaf >= HV_LEAF_TIMING) {
		out->has_timing = 1;
		cpuid(HV_LEAF_TIMING, &a, &b, &c, &d);
		out->tsc_khz = a;
		out->bus_khz = b;
	}

	if (!out->is_kvm)
		return;

	cpuid(KVM_LEAF_FEATURES, &a, &b, &c, &d);
	out->kvm_features = a;
	if (out->kvm_features & KVM_FEATURE_CLOCKSOURCE2)
		kvmclock_read(out);
}

void freq_exact_read(struct freq_exact *out)
{
	struct freq_cpuid	cpu;
	struct freq_hypervisor	hv;

	*out = (struct freq_exact){ { 0 }, 0 };
#if	ABLATE_508_EXACT_HIDDEN
	/*
	 * #508: no exact source is seen, so the measured path runs on a
	 * machine that has one.  Never on in a kernel booted otherwise.
	 */
	return;
#endif
	freq_cpuid_read(&cpu);
#if	ABLATE_508_CPUID_15
	/*
	 * #508: the processor states 0x15 as OMEGA's does (crystal 38.4 MHz,
	 * TSC/crystal 156/2), because no guest offers the leaf filled in and
	 * this is the only way the path that adopts it runs.  Never on in a
	 * kernel booted for anything else.
	 */
	cpu.has_15 = 1;
	cpu.crystal_hz = 38400000;
	cpu.tsc_numerator = 156;
	cpu.tsc_denominator = 2;
#endif
	if (cpu.has_15 && cpu.crystal_hz != 0 && cpu.tsc_numerator != 0
	    && cpu.tsc_denominator != 0)
		out->tsc_hz[FREQ_CPUID] = (uint64_t)cpu.crystal_hz
			* cpu.tsc_numerator / cpu.tsc_denominator;

	freq_hypervisor_read(&hv);
	if (hv.has_timing && hv.tsc_khz != 0)
		out->tsc_hz[FREQ_TIMING] = (uint64_t)hv.tsc_khz * 1000;
	if (hv.has_timing && hv.bus_khz != 0)
		out->lapic_bus_hz = (uint64_t)hv.bus_khz * 1000;
	out->tsc_hz[FREQ_KVMCLOCK] = hv.kvmclock_tsc_hz;

#if	ABLATE_508_EXACT_LIES
	/*
	 * #508: the first source that states a rate states it one part in
	 * thirty-two high, so the measurement can be seen contradicting it.
	 */
	for (unsigned i = 0; i < FREQ_EXACT; i++)
		if (out->tsc_hz[i] != 0) {
			out->tsc_hz[i] += out->tsc_hz[i] / 32;
			break;
		}
#endif
}

const char *freq_exact_name(unsigned id)
{
	switch (id) {
	case FREQ_CPUID:	return "CPUID 0x15";
	case FREQ_TIMING:	return "the hypervisor's timing leaf";
	case FREQ_KVMCLOCK:	return "KVM's clock";
	}
	return "?";
}

