/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The battery-backed clock (#453).
 *
 * This is the only source of wall-clock time the machine has at boot: the
 * TSC counts since power-on and knows nothing about what year it is, and no
 * network is up yet.  kern/posixtime.c reads it once, in utime_init(), and
 * from there the kernel keeps its own time from the timer interrupt.
 *
 * The device is the MC146818-compatible CMOS RTC, still at ports 0x70/0x71 on
 * every x86-64 machine that boots without UEFI runtime services.  That part
 * genuinely has not changed since 1984, so it is kept.  What is not kept is
 * how i386/AT386/bbclock.c reads it, and the differences are the point of
 * this file:
 *
 *   ── The update race.  The RTC's registers are not coherent while it is
 *      rolling the seconds over.  i386 reads them once and hopes.  Here they
 *      are read repeatedly until two consecutive reads agree, which is the
 *      only method that cannot see a half-updated set: it does not matter
 *      when the update happens, only that two identical readings cannot have
 *      straddled one.
 *
 *   ── The format.  i386's bbc_config() *programs* register B to a format it
 *      then assumes, from a routine this machine does not call.  Here
 *      register B is read and believed: BCD or binary, 12-hour or 24-hour.
 *      Firmware that hands us binary mode is not a hypothetical -- it is what
 *      "the format bits exist" means.
 *
 *   ── The century.  i386 has a two-digit year and the pivot `yr < 70 ?
 *      yr + 100', which stops being right in 2070.  Here the century comes
 *      from CMOS register 0x32 when it holds something a century can be, and
 *      the pivot is only the fallback.
 *
 *   ── The calendar.  i386 walks a static int month[12] array, mutates
 *      month[1] to 29 for a leap year and puts it back -- which is a data
 *      race between any two callers and was only ever safe because there is
 *      one.  Here the conversion is Howard Hinnant's days_from_civil: closed
 *      form, no table, no state, correct for every year the fields can hold.
 *
 * ⚠️ There is no bbc_settime().  Writing the hardware clock is a policy
 * decision belonging to a userspace time daemon, not to the kernel, and
 * nothing in this tree calls it.  A stub returning KERN_SUCCESS would claim
 * the clock had been set.
 */

#include <stdint.h>

#include <mach/kern_return.h>
#include <mach/clock_types.h>
#include <kern/posixtime.h>	/* the interface, so it is checked (#448) */
#include <kern/misc_protos.h>	/* printf */

#include <cpu/regs.h>
#include <cpu/spl.h>

/*
 * The index/data port pair.  Writing an index selects a register; the value
 * is then read or written through the data port.
 *
 * ⚠️ Bit 7 of the index port is the NMI disable, and it is write-only: there
 * is no way to read back what it was.  Every index written here has it clear,
 * which leaves NMI enabled -- the state this kernel wants and the state it is
 * in when we get here.  A machine that had masked NMI around something would
 * have it unmasked by this read, which is why the whole sequence is done at
 * splhigh and at boot.
 */
#define	RTC_INDEX	0x70
#define	RTC_DATA	0x71

#define	RTC_SECONDS	0x00
#define	RTC_MINUTES	0x02
#define	RTC_HOURS	0x04
#define	RTC_DAY		0x07
#define	RTC_MONTH	0x08
#define	RTC_YEAR	0x09
#define	RTC_REG_A	0x0a
#define	RTC_REG_B	0x0b
#define	RTC_CENTURY	0x32		/* where ACPI's FADT points on every
					   machine that has one */

#define	REG_A_UIP	0x80		/* update in progress */
#define	REG_B_24HOUR	0x02		/* 0 = 12-hour, hour bit 7 = PM */
#define	REG_B_BINARY	0x04		/* 0 = BCD */

#define	HOUR_PM		0x80		/* in 12-hour mode, in the hour byte */

/* The set of fields one reading produces. */
struct rtc_reading {
	uint8_t	sec, min, hour, day, month, year, century;
};

static uint8_t
cmos_read(uint8_t index)
{
	outb(RTC_INDEX, index);
	return inb(RTC_DATA);
}

/*
 * Wait for the update-in-progress flag to clear.
 *
 * ⚠️ Bounded.  The RTC sets UIP for under 2 ms once a second, so this returns
 * almost immediately -- but a machine whose RTC is absent or wedged reads
 * 0xFF from an unclaimed port, UIP is set forever, and an unbounded loop
 * would hang the boot on a clock nobody needs to be correct.  Giving up is
 * safe: the read-twice-until-equal loop below does not depend on this, it is
 * only how the first attempt avoids being the one that straddles an update.
 */
static void
rtc_wait_ready(void)
{
	unsigned int	spins = 1000000;

	while (spins-- != 0 && (cmos_read(RTC_REG_A) & REG_A_UIP) != 0)
		cpu_pause();
}

static void
rtc_read_fields(struct rtc_reading *r)
{
	r->sec	   = cmos_read(RTC_SECONDS);
	r->min	   = cmos_read(RTC_MINUTES);
	r->hour	   = cmos_read(RTC_HOURS);
	r->day	   = cmos_read(RTC_DAY);
	r->month   = cmos_read(RTC_MONTH);
	r->year	   = cmos_read(RTC_YEAR);
	r->century = cmos_read(RTC_CENTURY);
}

static int
rtc_same(const struct rtc_reading *a, const struct rtc_reading *b)
{
	return a->sec == b->sec && a->min == b->min && a->hour == b->hour &&
	       a->day == b->day && a->month == b->month &&
	       a->year == b->year && a->century == b->century;
}

static unsigned int
from_bcd(uint8_t v)
{
	return (v & 0x0f) + ((v >> 4) * 10);
}

/*
 * Days since 1970-01-01 from a proleptic Gregorian date.
 *
 * Howard Hinnant's days_from_civil, which is exact for the whole range and
 * has no loop, no month-length table and no state.  It works by shifting the
 * year so that it starts in March: leap day then lands at the *end* of the
 * year, where it stops perturbing the day-of-year formula, and the whole
 * month length pattern collapses into (153 * m + 2) / 5.
 *
 * The 719468 subtracts the days from 0000-03-01 to 1970-01-01, moving the
 * epoch to where the caller expects it.
 */
static int64_t
days_from_civil(int64_t y, unsigned int m, unsigned int d)
{
	int64_t		era;
	uint64_t	yoe, doy, doe;

	y -= (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = (uint64_t)(y - era * 400);			/* [0, 399] */
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;	/* [0, 365] */
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;		/* [0, 146096] */

	return era * 146097 + (int64_t) doe - 719468;
}

kern_return_t
bbc_gettime(tvalspec_t *cur_time)
{
	struct rtc_reading	a, b;
	unsigned int		sec, min, hour, day, month, year, century;
	unsigned int		tries;
	uint8_t			regb;
	int			pm;
	int64_t			days;
	spl_t			s;

	s = splhigh();

	rtc_wait_ready();
	rtc_read_fields(&a);

	/*
	 * Read until two consecutive readings are identical.  Ten attempts is
	 * far more than the one retry an update can ever force; a device that
	 * cannot produce two equal readings in ten is not telling the time,
	 * and we say so rather than believe the tenth.
	 */
	for (tries = 0; tries < 10; tries++) {
		rtc_wait_ready();
		rtc_read_fields(&b);
		if (rtc_same(&a, &b))
			break;
		a = b;
	}
	if (tries == 10) {
		splx(s);
		printf("rtc: no stable reading from the battery-backed clock "
		       "-- wall-clock time starts at the epoch\n");
		return KERN_FAILURE;
	}

	regb = cmos_read(RTC_REG_B);

	splx(s);

	/*
	 * The PM bit lives in the hour byte and must come off before any
	 * conversion, or it would be read as part of the value.
	 */
	pm = 0;
	if ((regb & REG_B_24HOUR) == 0 && (a.hour & HOUR_PM) != 0) {
		pm = 1;
		a.hour &= (uint8_t) ~HOUR_PM;
	}

	if (regb & REG_B_BINARY) {
		sec = a.sec; min = a.min; hour = a.hour;
		day = a.day; month = a.month; year = a.year;
		century = a.century;
	} else {
		sec = from_bcd(a.sec); min = from_bcd(a.min);
		hour = from_bcd(a.hour); day = from_bcd(a.day);
		month = from_bcd(a.month); year = from_bcd(a.year);
		century = from_bcd(a.century);
	}

	/* 12 AM is hour 0, 12 PM is hour 12: the one case the bit gets wrong. */
	if ((regb & REG_B_24HOUR) == 0) {
		if (hour == 12)
			hour = 0;
		if (pm)
			hour += 12;
	}

	/*
	 * The century register when it holds a century, the pivot when it does
	 * not.  Machines without one read 0 or 0xFF from an unimplemented CMOS
	 * cell, and neither is a century -- so the test is on the value, not on
	 * a table of which machines have the register.
	 */
	if (century >= 19 && century <= 21)
		year += century * 100;
	else
		year += (year < 70) ? 2000 : 1900;

	if (month < 1 || month > 12 || day < 1 || day > 31 ||
	    hour > 23 || min > 59 || sec > 60) {
		printf("rtc: implausible date %u-%02u-%02u %02u:%02u:%02u "
		       "-- wall-clock time starts at the epoch\n",
		       year, month, day, hour, min, sec);
		return KERN_FAILURE;
	}

	days = days_from_civil((int64_t) year, month, day);

	cur_time->tv_sec  = (unsigned int)
			    (days * 86400 + hour * 3600 + min * 60 + sec);
	cur_time->tv_nsec = 0;

	return KERN_SUCCESS;
}
