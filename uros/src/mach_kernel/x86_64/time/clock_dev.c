/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * The clock devices this machine offers.
 *
 * ⚠️ This is the *device* interface -- clock_list[], the thing a task reaches
 * through host_get_clock_service() and clock_get_time().  It is not the
 * timebase: x86_64/time/ has had the PIT, the TSC and the local APIC timer
 * since #459, and they are what makes the scheduler tick.  What was missing
 * was the port a task can name.
 *
 * 🔥 What its absence cost.  `clock_count' was 0 and the file said so, calling
 * itself "a real gap rather than a decision".  It was right about being a gap
 * and wrong about being small: host_get_clock_service() takes the
 * `clock_id >= clock_count' branch for every id when the count is zero, so it
 * answered KERN_INVALID_ARGUMENT to everyone, always.  libmach's getclock()
 * turns that into a failure, and getclock() is what every absolute deadline in
 * libpthreads is computed from -- so pthread_mutex_timedlock(),
 * pthread_cond_timedwait(), the rwlock timed variants and
 * pthread_timedjoin_np() were all computing deadlines from a struct nobody had
 * written to.  The comment named the gap; nothing named the consumer.
 *
 * 🔑 The failure was invisible because it was not rare.  pthread_test's
 * timed-join arm passed about one boot in seven -- not because the clock
 * sometimes worked, but because the thread being joined had sometimes already
 * exited, and a join that finds its target finished does not consult the
 * deadline.  A defect that is masked by a race looks like a race.
 */

#include <mach/kern_return.h>
#include <mach/clock_types.h>
#include <mach/message.h>
#include <kern/clock.h>
#include <kern/posixtime.h>		/* bbc_gettime, utime_get, utime_set */
#include <kern/time_out.h>		/* tick -- microseconds per tick */
#include <x86_64/time/tsc.h>		/* rdtsc, tsc_hz -- the sub-tick ruler */

/*
 * ⚠️ <sys/time.h> is deliberately not included, although it is where the rest
 * of the kernel gets the wall clock.  That header carries a BSD compatibility
 * macro, `#define tv_sec seconds', and this file's whole job is to fill in
 * tvalspec_t -- whose fields are named tv_sec and tv_nsec.  Including it
 * rewrites every one of those into a member tvalspec does not have, and the
 * compiler then reports the *rewritten* name, so the error names a field the
 * source never contains.  kern/posixtime.h reaches the same clock without the
 * macro.
 */

/*
 * ── REALTIME_CLOCK ────────────────────────────────────────────────────────
 *
 * Backed by `time', the wall clock kern/posixtime.c already maintains: seeded
 * once from the battery-backed clock by utime_init() and advanced every tick
 * by utime_tick(), which hertz_tick() calls on the master processor.  Both
 * halves were already running here before this file existed; nothing about
 * the timebase changes.
 *
 * ⚠️ This differs from i386 on purpose.  There, REALTIME_CLOCK is time since
 * boot with a sub-tick interpolation read from the 8254 or the TSC; the zero
 * is the moment the machine started and getclock()'s own comment settles for
 * guaranteeing only that differences are elapsed time.  Here it is time of
 * day, which is what a caller asking for TIMEOFDAY means, and it satisfies the
 * difference guarantee as well.  The finer resolution is not free on this
 * machine -- the TSC is not invariant under emulation and #318 is where the
 * timebase itself is settled -- so the resolution reported below is the tick,
 * honestly, rather than a number the hardware cannot stand behind.
 */

/*
 * ⚠️ Named for what they do, not for the slot they fill.
 *
 * i386 calls these rtc_config/rtc_init/rtc_gettime, and `rtc_gettime' there
 * is a GLOBAL with a meaning kern/posixtime.c depends on: time since boot,
 * used to interpolate inside a tick.  This machine's REALTIME_CLOCK is time
 * of day.  Reusing the name for a different quantity is how a caller ends up
 * adding an interval to an absolute -- so these stay static, and stay called
 * what they are.  Only the ops tables are exported, because conf.c names
 * those and every architecture spells them the same way.
 */

static int
wall_config(void)
{
	/*
	 * Always present: it is a reading of a variable this kernel keeps, not
	 * an access to a device that may be absent.
	 */
	return (1);
}

static int
wall_init(void)
{
	return (0);
}

/*
 * 🔥 THE TICK IS NOT THE RESOLUTION -- SUB-TICK INTERPOLATION FROM THE TSC.
 *
 * utime_get() returns a variable the tick advances, so without this the finest
 * distinguishable reading is one tick: ten milliseconds.  Every elapsed time a
 * program computes from this clock then comes out a whole multiple of 10000
 * microseconds, and a benchmark reports figures like "11.00 us/op" whose two
 * decimals are arithmetic on a single tick.  Anything cheaper than a tick
 * measures as ZERO -- ipc_bench timed 16384 trap calls at 0 us.
 *
 * 🔑 THIS EXACT SYMPTOM WAS DIAGNOSED AND FIXED ONCE ALREADY, on i386, in #344:
 * "numeri grossolani (3.00/2.00/2.00/2.00 us arrotondati = fallback mtime)".
 * The fix there is rtc_subtick_nsec() in i386/rtclock.c, and this is the same
 * mechanism written for this machine: the tick handler stamps the TSC when it
 * advances the kept time, and a reader converts the distance since that stamp.
 *
 * ⚠️ Clamped to one tick, and that clamp is not defensive tidiness.  A stale
 * anchor -- a tick that was late, or a processor whose TSC is not phase-locked
 * to the stamping one (#318) -- would otherwise add an arbitrary amount to a
 * wall clock, and a clock that jumps forward breaks every deadline computed
 * across the jump in the direction that does not time out.
 *
 * ⚠️ i386 carries the same property and so does this: the interpolation can
 * overshoot the next tick by whatever a late tick costs, so the clock is not
 * strictly monotonic across a tick boundary.  Bounded by one tick, which is
 * why the bound exists; a clock that cannot go backwards at all is a different
 * design and not what #344 built.
 */
volatile uint64_t	wall_tsc_at_tick;	/* stamped by the tick (master cpu) */

static uint32_t
wall_subtick_nsec(uint64_t anchor)
{
	uint64_t	hz = tsc_hz();
	uint64_t	per_us, delta, maxd, ns, cap;

	/*
	 * Zero when the calibration did not run or its two runs disagreed.  Then
	 * the tick is all there is, and saying so by returning nothing beats
	 * interpolating with a rate nobody measured.
	 */
	if (hz == 0)
		return (0);

	per_us = hz / 1000000ULL;		/* TSC counts per microsecond */
	if (per_us == 0)
		return (0);

	delta = rdtsc() - anchor;
	maxd  = per_us * (uint64_t) tick;	/* one tick's worth of counts */
	if (delta > maxd)
		delta = maxd;

	ns  = (delta * 1000ULL) / per_us;
	cap = (uint64_t) tick * NSEC_PER_USEC;
	if (ns > cap)
		ns = cap;

	return ((uint32_t) ns);
}

static kern_return_t
wall_gettime(tvalspec_t *cur_time)
{
	time_value_t	now;
	uint64_t	a0, a1, ns;
	unsigned int	sec;
	uint32_t	sub;
	int		tries = 0;

	/*
	 * utime_get() carries the seqlock the mapped copy was built for, so
	 * the reading is never one caught between the microseconds wrapping
	 * and the seconds being carried.  That matters more than resolution:
	 * a clock that goes a second backwards makes every deadline computed
	 * across the jump wrong in the direction that does not time out.
	 *
	 * 🔴 The anchor needs its own agreement with that reading, and the
	 * seqlock does not cover it.  A tick landing between the two would pair
	 * the OLD kept time with the NEW anchor, and the sum would go backwards
	 * by almost a whole tick -- which is precisely the failure the seqlock
	 * exists to prevent, reintroduced one variable along.  So the anchor is
	 * read on both sides and the pair is retaken if it moved.
	 */
	do {
		a0 = wall_tsc_at_tick;
		utime_get(&now);
		sub = wall_subtick_nsec(a0);
		a1 = wall_tsc_at_tick;
	} while (a0 != a1 && ++tries < 4);

	ns  = (uint64_t) now.microseconds * NSEC_PER_USEC + sub;
	sec = (unsigned int) now.seconds;

	/*
	 * The carry is not theoretical: microseconds runs to 999999, which is
	 * 999999000 nanoseconds, and one tick more than that is past a second.
	 */
	if (ns >= (uint64_t) NSEC_PER_SEC) {
		sec += (unsigned int) (ns / NSEC_PER_SEC);
		ns  %= NSEC_PER_SEC;
	}

	cur_time->tv_sec  = sec;
	cur_time->tv_nsec = (clock_res_t) ns;
	return (KERN_SUCCESS);
}

static kern_return_t
wall_settime(tvalspec_t *new_time)
{
	time_value_t	set;

	/*
	 * kern/clock.c has already rejected a tvalspec that is out of range
	 * and flushed the outstanding alarms; what is left is the write, and
	 * utime_set() is where the master-processor binding that write needs
	 * lives.  This is the same operation host_set_time() performs -- the
	 * difference is only which door the caller came through.
	 */
	set.seconds	 = (integer_t) new_time->tv_sec;
	set.microseconds = (integer_t) (new_time->tv_nsec / NSEC_PER_USEC);

	utime_set(set);
	return (KERN_SUCCESS);
}

static kern_return_t
wall_getattr(
	clock_flavor_t		flavor,
	clock_attr_t		attr,		/* OUT */
	mach_msg_type_number_t	*count)		/* IN/OUT */
{
	if (*count != 1)
		return (KERN_FAILURE);

	switch (flavor) {

	case CLOCK_GET_TIME_RES:
		/*
		 * 🔑 THE ANSWER CHANGED WHEN THE CLOCK DID, AND THAT IS PART OF
		 * THE CHANGE, NOT A FOLLOW-UP.
		 *
		 * This used to answer `tick' -- ten milliseconds -- and that was
		 * the truth while wall_gettime() returned a variable the tick
		 * advanced.  With the sub-tick interpolation above, two readings
		 * a microsecond apart are distinguishable, so continuing to
		 * report the tick would understate the clock by four orders of
		 * magnitude to every caller that asks before deciding whether
		 * this clock can measure what it wants.
		 *
		 * One microsecond rather than a nanosecond: the conversion
		 * divides by TSC-counts-per-MICROsecond, an integer, so the
		 * quotient is nanoseconds computed from a microsecond ruler.
		 * Claiming nanosecond resolution would be claiming the field
		 * width, which is exactly what the old comment here warned
		 * against.
		 *
		 * ⚠️ Falls back to the tick when the calibration did not run:
		 * then the interpolation returns nothing and the tick really is
		 * the resolution.
		 */
		*attr = tsc_hz() ? (int) NSEC_PER_USEC
				 : (int) (tick * NSEC_PER_USEC);
		break;

	default:
		return (KERN_INVALID_VALUE);
	}

	*count = 1;
	return (KERN_SUCCESS);
}

/*
 * ⚠️ Three slots are empty, and each is empty for its own reason rather than
 * for the general one.  kern/clock.c tests all three for null before calling
 * them and turns the null into a refusal, so an empty slot is this machine
 * saying it does not offer that operation -- which is a true statement, and
 * not the same thing as a function that returns KERN_SUCCESS having done
 * nothing.
 *
 *   c_setattr  would change the resolution of the tick.  On i386 that is real
 *              -- rtc_setattr reprograms the 8254 divisor.  Here the tick
 *              rate is chosen by x86_64/time/clock_event.c, which the
 *              scheduler is running on; a device that appeared to change it
 *              would either lie or reach across into the scheduler's timer.
 *
 *   c_maptime  needs a device with a d_mmap routine -- i386 names one,
 *              "rtclock", in its dev_name_list.  This machine's list has the
 *              console and nothing else.  The page itself already exists
 *              (kern/posixtime.c allocates and maintains it), so what is
 *              missing is the device entry, not the mechanism.
 *
 *   c_setalrm  needs a one-shot timer that is not the one the scheduler has
 *              already claimed.  When this machine grows a second one,
 *              clock_sleep() and clock_alarm() become reachable by filling
 *              this slot and nothing else changes.
 */
struct clock_ops rtc_ops = {
	wall_config,	wall_init,	wall_gettime,	wall_settime,
	wall_getattr,	0,		0,		0,
};

/*
 * ── BATTERY_CLOCK ─────────────────────────────────────────────────────────
 *
 * The CMOS clock itself, read by bbc_gettime() in x86_64/time/rtc.c.  Offered
 * separately from REALTIME_CLOCK because they answer different questions: this
 * one is what the machine believes when it is switched on, and it keeps its
 * answer across a reboot.
 */

static int
bbclk_config(void)
{
	tvalspec_t	probe;

	/*
	 * Ask the hardware once.  bbc_gettime() refuses when it cannot get two
	 * matching readings or when the date it assembles is implausible, and
	 * a clock that cannot be read at configure time is a clock this machine
	 * does not have -- kern/clock.c clears cl_ops on a zero here, so the
	 * device disappears instead of being present and wrong.
	 */
	return (bbc_gettime(&probe) == KERN_SUCCESS);
}

static int
bbclk_init(void)
{
	return (0);
}

static kern_return_t
bbclk_getattr(
	clock_flavor_t		flavor,
	clock_attr_t		attr,		/* OUT */
	mach_msg_type_number_t	*count)		/* IN/OUT */
{
	if (*count != 1)
		return (KERN_FAILURE);

	switch (flavor) {

	case CLOCK_GET_TIME_RES:
		/* One second: the CMOS clock has no sub-second field. */
		*attr = NSEC_PER_SEC;
		break;

	default:
		return (KERN_INVALID_VALUE);
	}

	*count = 1;
	return (KERN_SUCCESS);
}

/*
 * ⚠️ No c_settime: x86_64/time/rtc.c says why there is no bbc_settime() to put
 * here.  Writing the hardware clock is a policy decision about who owns the
 * machine's idea of time, and this file is not where that is taken.
 */
struct clock_ops bbc_ops = {
	bbclk_config,	bbclk_init,	bbc_gettime,	0,
	bbclk_getattr,	0,		0,		0,
};
