/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What the processor and the hypervisor say about the clocks' frequencies,
 * before anybody measures them (#508).
 *
 * tsc_calibrate() and lapic_timer_calibrate() measure against the 8254 and
 * never ask.  Where the answer is available it is exact, and a measurement
 * then becomes a check on it rather than the only source.  This file only
 * READS: every value is reported as the source gave it, and nothing here
 * decides which source is believed.  That decision is #508's phase 3, and it
 * is taken from what these readers return on the machines we have, not from
 * what the manuals promise.
 */

#ifndef _X86_64_TIME_FREQ_SOURCE_H_
#define _X86_64_TIME_FREQ_SOURCE_H_

#include <stdint.h>

/*
 * CPUID leaves 0x15 and 0x16 (Intel SDM, "CPUID — CPU Identification").
 *
 * 0x15 states the TSC as a ratio to the core crystal, and the crystal's
 * frequency where the part enumerates it; TSC = crystal * numerator /
 * denominator.  0x16 states nominal frequencies in MHz, which are what the
 * part is sold as, not what it measures.
 *
 * ⚠️ Zero means "not stated", field by field: a numerator of zero means the
 * ratio is not enumerated, a crystal of zero means the crystal is not.  And
 * a leaf above the processor's highest basic leaf is not asked at all: on
 * Intel such a leaf returns the highest leaf's data, which would read as an
 * answer.
 */
struct freq_cpuid {
	uint32_t	max_leaf;	/* CPUID.0:EAX */
	int		has_15;		/* max_leaf >= 0x15 */
	uint32_t	tsc_denominator;	/* 0x15 EAX */
	uint32_t	tsc_numerator;		/* 0x15 EBX */
	uint32_t	crystal_hz;		/* 0x15 ECX */
	int		has_16;		/* max_leaf >= 0x16 */
	uint32_t	base_mhz;	/* 0x16 EAX[15:0] */
	uint32_t	max_mhz;	/* 0x16 EBX[15:0] */
	uint32_t	bus_mhz;	/* 0x16 ECX[15:0] */
};

void freq_cpuid_read(struct freq_cpuid *out);

/*
 * The hypervisor's own leaves, and KVM's paravirtual clock.
 *
 * Asked only when CPUID.1:ECX[31] says a hypervisor is present: on bare
 * metal the range at 0x40000000 is not defined, and what it returns is not
 * an answer.  0x40000010, where a hypervisor that implements it states the
 * TSC and the LAPIC bus in kHz, is asked only when the hypervisor's highest
 * leaf reaches it.
 *
 * KVM's paravirtual clock (Documentation/virt/kvm/x86/msr.rst) does not state
 * a frequency.  It states the conversion the host uses, nanoseconds =
 * ((tsc << shift) * mul) >> 32, and the frequency is what that conversion
 * implies.  It is asked only under KVM's signature and only through the MSR
 * whose CPUID bit is set: the MSR does not exist anywhere else, and writing
 * it would fault.
 */
struct freq_hypervisor {
	int		present;	/* CPUID.1:ECX[31] */
	char		signature[13];	/* 0x40000000 EBX, ECX, EDX */
	uint32_t	max_leaf;	/* 0x40000000 EAX */
	int		has_timing;	/* max_leaf >= 0x40000010 */
	uint32_t	tsc_khz;	/* 0x40000010 EAX */
	uint32_t	bus_khz;	/* 0x40000010 EBX */
	int		is_kvm;
	uint32_t	kvm_features;	/* 0x40000001 EAX */
	int		kvmclock_asked;	/* the MSR was written */
	uint32_t	kvmclock_version;	/* 0 = the host wrote nothing */
	uint32_t	kvmclock_mul;
	int8_t		kvmclock_shift;
	uint8_t		kvmclock_flags;
	uint64_t	kvmclock_tsc_hz;	/* implied by mul and shift */
};

void freq_hypervisor_read(struct freq_hypervisor *out);

#endif	/* _X86_64_TIME_FREQ_SOURCE_H_ */
