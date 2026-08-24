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

static kern_return_t
wall_gettime(tvalspec_t *cur_time)
{
	time_value_t	now;

	/*
	 * utime_get() carries the seqlock the mapped copy was built for, so
	 * the reading is never one caught between the microseconds wrapping
	 * and the seconds being carried.  That matters more than resolution:
	 * a clock that goes a second backwards makes every deadline computed
	 * across the jump wrong in the direction that does not time out.
	 */
	utime_get(&now);

	cur_time->tv_sec  = (unsigned int) now.seconds;
	cur_time->tv_nsec = (clock_res_t) (now.microseconds * NSEC_PER_USEC);
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
		 * The tick, in nanoseconds.  `tick' is microseconds per tick
		 * and is what utime_tick() adds, so this is the real distance
		 * between two distinguishable readings -- not the width of the
		 * field they are returned in.
		 */
		*attr = (int) (tick * NSEC_PER_USEC);
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
