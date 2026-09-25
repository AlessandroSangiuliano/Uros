/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The exact sources, and which side wins (#508, #594).
 *
 * Every source that states a rate is checked against the rulers, in the order
 * <time/freq_source.h> gives -- the processor's own statement, then the
 * hypervisor's, then the one inferred from KVM's clock -- and the first that
 * agrees with them is ADOPTED.  The reason the exact source wins over the
 * measurement it agrees with: it is the same number every boot, where the
 * measurement carries whatever the host's NTP was doing at that moment (#508
 * measured the rulers climbing 136 ppm in four minutes on a host still
 * converging, and sitting 8.5 ppm off on one that had settled).  The distance
 * is printed either way.
 *
 * AGREES WITH THE RULERS: no further from the measured value than half the
 * widest bracket behind it, plus 500 ppm for the rulers' own accuracy (IA-PC
 * HPET 1.0a 2.4.1) and 500 ppm on the other side -- under a hypervisor the
 * rulers run on the host's clock, which Linux's NTP may steer by up to
 * MAXFREQ, 500 ppm.  A source further than that CONTRADICTS the rulers, and
 * that is WRONG: one of two things that cannot both be wrong by so much is.
 *
 * TWO EXACT SOURCES CANNOT DIFFER.  Each is exact to well under a ppm (a
 * leaf in kHz or Hz, a 32-bit multiplier), so two more than 10 ppm apart are
 * not both exact, and the later one is NOT USED.  #508 found the case on a
 * real configuration, not an ablation: under QEMU's `tsc-frequency=' the
 * timing leaf said 2994000 kHz, the rulers agreed with it, and KVM's clock
 * still said the host's 2994656 -- 219 ppm, well inside the rulers' window,
 * so only the precedence caught it.
 *
 * UNDER A HYPERVISOR, THE HOST'S NTP ALSO SLEWS THE PHASE (#594).  MAXFREQ
 * bounds only the frequency Linux's NTP sets.  The phase error it is handed
 * is drained on top of that, offset >> (SHIFT_PLL + time_constant) every
 * second (kernel/time/ntp.c, ntp_offset_chunk()), with the offset clamped to
 * MAXPHASE, 0.5 s, and time_constant to 0 and above.  So a host's clock may
 * run up to MAXPHASE >> SHIFT_PLL = 125 ms a second -- 125,000 ppm -- from
 * true time while it converges, and a guest's rulers with it.  #594 measured
 * it on a host just rebooted: timesyncd hands the kernel offsets under 0.4 s
 * at constant 1, and the rulers ran 3343 ppm from KVM's clock at boot and
 * 7325 ppm from the TSC a few minutes later -- here the exact source was found
 * WRONG and the rulers' value, 0.33% off, adopted.  Under a hypervisor that
 * term is added: a source contradicts the rulers only beyond what the host's
 * NTP can do to them within one measurement.  On bare metal the rulers are
 * crystals, and nothing slews them.
 *
 * NOTHING MEASURED, NOTHING ADOPTED.  With no ruler that answered, a source
 * cannot be checked, and it is not believed unchecked: the clock's rate stays
 * zero and its consumers say NOT ASKED (#586).
 *
 * ⚠️ One rule for the TSC and the LAPIC timer, and in one file.  The LAPIC
 * timer's single source used to be checked by a copy of the bound written
 * inline in lapic.c; a second source there (#594) would have needed a copy of
 * the precedence and the 10 ppm as well, and two copies of a rule are two
 * rules the day one of them is changed.
 */

#include <stdint.h>

#include <time/exact.h>

#define EXACT_AGREE_PPM		10
#define EXACT_BOUND_PPM		1000	/* 500 for the rulers, 500 for NTP */
#define NTP_MAXPHASE_NS		500000000	/* Linux, <linux/timex.h> */
#define NTP_SHIFT_PLL		2		/* the same header */
#define EXACT_PHASE_PPM		((NTP_MAXPHASE_NS / 1000) >> NTP_SHIFT_PLL)

static uint64_t ppm_apart(uint64_t a, uint64_t b)
{
	uint64_t spread = a > b ? a - b : b - a;

	return b ? spread * 1000000 / b : 0;
}

uint64_t exact_choose(const uint64_t hz[FREQ_EXACT], uint64_t measured,
		      uint64_t bracket_ppm, struct exact_choice *out)
{
	uint64_t	adopted_hz = 0;
	unsigned	id;

	*out = (struct exact_choice){ .adopted = -1 };
	out->measured = measured;
	out->phase_ppm = freq_under_hypervisor() ? EXACT_PHASE_PPM : 0;
	out->bound_ppm = bracket_ppm / 2 + EXACT_BOUND_PPM + out->phase_ppm;

	for (id = 0; id < FREQ_EXACT; id++) {
		out->hz[id] = hz[id];
		if (hz[id] == 0) {
			out->verdict[id] = EXACT_ABSENT;
			continue;
		}
		if (measured == 0) {
			out->verdict[id] = EXACT_UNCHECKED;
			continue;
		}
		out->ppm[id] = ppm_apart(hz[id], measured);
		if (out->ppm[id] > out->bound_ppm) {
			out->verdict[id] = EXACT_CONTRADICTS;
			continue;
		}
		if (out->adopted < 0) {
			out->adopted = (int)id;
			adopted_hz = hz[id];
			out->verdict[id] = EXACT_ADOPTED;
			continue;
		}
		out->apart_ppm[id] = ppm_apart(hz[id], adopted_hz);
		out->verdict[id] = out->apart_ppm[id] <= EXACT_AGREE_PPM
				   ? EXACT_AGREES : EXACT_NOT_USED;
	}
	return adopted_hz;
}
