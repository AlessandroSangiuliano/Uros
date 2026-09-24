/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The kernel's rulers, and their vote (#508).  See rulers.h.
 */

#include <stdint.h>

#include <time/hpet.h>
#include <time/pit.h>
#include <time/pmtimer.h>
#include <time/ruler.h>
#include <time/rulers.h>

#define RULER_OWN_PPM	500	/* each ruler's rate, IA-PC HPET 1.0a 2.4.1 */

static uint64_t read_pm(void)
{
	return pmtimer_read();
}

static uint64_t read_hpet(void)
{
	return hpet_read32();
}

static struct kernel_ruler rulers[RULERS];
static struct rulers_verdict verdict = { .dissenter = -1, .kept = -1 };
static int found;

void rulers_find(void)
{
	if (found)
		return;
	found = 1;

	rulers[RULER_8254] = (struct kernel_ruler){
		.name = "8254", .r = pit_read_back, .present = 1,
		.span = PIT_RULER_SPAN,
	};

	rulers[RULER_PM].name = "PM timer";
	if (pmtimer_init()) {
		rulers[RULER_PM].r = (struct ruler){
			read_pm,
			pmtimer_width() == 32 ? 0xffffffffULL : 0x00ffffffULL,
#if	ABLATE_508_RULER_LIES
			/*
			 * #508: the PM timer's rate is taken as one part in
			 * thirty-two higher than the specification's, a ruler
			 * that lies, so the vote can be seen naming it.
			 */
			PMTIMER_HZ + PMTIMER_HZ / 32,
#else
			PMTIMER_HZ,
#endif
		};
		rulers[RULER_PM].present = 1;
		rulers[RULER_PM].span = PMTIMER_HZ * 3ULL / 100;
	}

	/*
	 * The counter's low half, one access where the whole counter is three
	 * (hpet.h), and it does not wrap for 43 s even at 100 MHz.
	 */
	rulers[RULER_HPET].name = "HPET";
	if (hpet_init()) {
		rulers[RULER_HPET].r = (struct ruler){
			read_hpet, 0xffffffffULL, hpet_hz(),
		};
		rulers[RULER_HPET].present = 1;
		rulers[RULER_HPET].span = hpet_hz() * 3 / 100;
	}
}

struct kernel_ruler *rulers_get(unsigned id)
{
	return id < RULERS ? &rulers[id] : 0;
}

void rulers_start(unsigned id)
{
	if (id == RULER_8254)
		pit_ruler_start();
}

void rulers_stop(unsigned id)
{
	if (id == RULER_8254)
		pit_ruler_stop();
}

/* The widest bracket among the runs that counted, in ppm. */
static uint64_t widest_ppm(const struct kernel_ruler *k)
{
	uint64_t w = 0;
	unsigned i;

	for (i = 0; i < RULER_RUNS; i++)
		if (k->tsc.run_hz[i] != 0 && k->tsc.run_ppm[i] > w)
			w = k->tsc.run_ppm[i];
	return w;
}

static int agree(const struct kernel_ruler *a, const struct kernel_ruler *b)
{
	uint64_t ha = a->tsc.hz, hb = b->tsc.hz;
	uint64_t spread = ha > hb ? ha - hb : hb - ha;
	uint64_t top = ha > hb ? ha : hb;
	uint64_t bound = (widest_ppm(a) + widest_ppm(b)) / 2
			 + 2 * RULER_OWN_PPM;

	return spread * 1000000 <= top * bound;
}

static uint64_t median3(uint64_t a, uint64_t b, uint64_t c)
{
	if (a > b)
		return b > c ? b : (a > c ? c : a);
	return a > c ? a : (b > c ? c : b);
}

static unsigned narrowest(const unsigned *ids, unsigned n)
{
	unsigned i, best = ids[0];

	for (i = 1; i < n; i++)
		if (widest_ppm(&rulers[ids[i]]) < widest_ppm(&rulers[best]))
			best = ids[i];
	return best;
}

void rulers_vote(struct rulers_verdict *v)
{
	unsigned ids[RULERS], n = 0, i;
	int ab, ac, bc;

	*v = (struct rulers_verdict){ .dissenter = -1, .kept = -1 };
	for (i = 0; i < RULERS; i++) {
		rulers[i].dissents = 0;
		if (rulers[i].present && rulers[i].tsc.hz != 0)
			ids[n++] = i;
	}
	v->answered = n;

	if (n == 0) {
		v->hz = 0;
	} else if (n == 1) {
		v->hz = rulers[ids[0]].tsc.hz;
	} else if (n == 2) {
		if (agree(&rulers[ids[0]], &rulers[ids[1]])) {
			v->hz = (rulers[ids[0]].tsc.hz
				 + rulers[ids[1]].tsc.hz) / 2;
		} else {
			v->no_majority = 1;
			v->kept = (int)narrowest(ids, n);
			v->hz = rulers[v->kept].tsc.hz;
		}
	} else {
		ab = agree(&rulers[ids[0]], &rulers[ids[1]]);
		ac = agree(&rulers[ids[0]], &rulers[ids[2]]);
		bc = agree(&rulers[ids[1]], &rulers[ids[2]]);

		if (ab + ac + bc >= 2) {
			v->hz = median3(rulers[ids[0]].tsc.hz,
					rulers[ids[1]].tsc.hz,
					rulers[ids[2]].tsc.hz);
		} else if (ab + ac + bc == 1) {
			v->dissenter = (int)(ab ? ids[2] : ac ? ids[1] : ids[0]);
			rulers[v->dissenter].dissents = 1;
			v->hz = 0;
			for (i = 0; i < n; i++)
				if ((int)ids[i] != v->dissenter)
					v->hz += rulers[ids[i]].tsc.hz / 2;
		} else {
			v->no_majority = 1;
			v->kept = (int)narrowest(ids, n);
			v->hz = rulers[v->kept].tsc.hz;
		}
	}

	verdict = *v;
}

const struct rulers_verdict *rulers_verdict(void)
{
	return &verdict;
}

uint64_t rulers_bracket_ppm(void)
{
	uint64_t w = 0, x;
	unsigned i;

	for (i = 0; i < RULERS; i++)
		if (rulers[i].present && rulers[i].tsc.hz != 0
		    && !rulers[i].dissents) {
			x = widest_ppm(&rulers[i]);
			if (x > w)
				w = x;
		}
	return w;
}

unsigned rulers_elected(void)
{
	unsigned ids[RULERS], n = 0, i;

	for (i = 0; i < RULERS; i++)
		if (rulers[i].present && rulers[i].tsc.hz != 0
		    && !rulers[i].dissents)
			ids[n++] = i;
	return n ? narrowest(ids, n) : RULER_8254;
}

int rulers_long(void)
{
	if (rulers[RULER_HPET].present && rulers[RULER_HPET].tsc.hz != 0
	    && !rulers[RULER_HPET].dissents)
		return RULER_HPET;
	if (rulers[RULER_PM].present && rulers[RULER_PM].tsc.hz != 0
	    && !rulers[RULER_PM].dissents)
		return RULER_PM;
	return -1;
}
