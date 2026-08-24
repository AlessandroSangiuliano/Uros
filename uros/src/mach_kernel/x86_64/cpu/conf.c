/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What devices this machine has, and what clocks (#453).
 *
 * The machine-independent device layer looks names up in dev_name_list[] and
 * resolves aliases through dev_indirect_list[]; the clock layer reads
 * clock_list[].  All three are the machine's own.  ⚠️ This comment used to say
 * they were all EMPTY, and said it for as long as they were; the device table
 * has one entry now (#426) and the sentence had to go with it -- a comment
 * that describes the file as it was is worse than none, because it is read as
 * a check.
 *
 * ── Why they are nearly empty, and why that is not "unfinished" ───────
 *
 * i386's conf.c lists a console, a disk, a keyboard and their kin, because
 * on that target the drivers live in the kernel.  This one has decided they
 * do not: network, block and character devices belong to servers in user
 * space, reached through the device master port, with the IOMMU making the
 * isolation real (#427, #432).
 *
 * So the question this table answers -- "which in-kernel drivers exist" --
 * has the answer "none", and it will keep having it.  What arrives later is
 * not entries here but the master-port path that lets a user-space driver
 * claim hardware.  The console below is not a counter-example: it drives no
 * hardware and owns nothing, it forwards to the kernel's own printf sink.
 *
 * ⚠️ An empty table is not the same as a missing one.  dev_lookup() walks
 * dev_name_count entries and finds nothing, which is a device open that
 * fails with D_NO_SUCH_DEVICE -- the correct answer for a device that is not
 * there.  Leaving the symbols undefined would instead be a kernel that does
 * not link, and a stub console would be a device that reports success and
 * moves no bytes.
 */

#include <device/conf.h>
#include <device/console_cdev.h>
#include <kern/clock.h>

/*
 * One entry, and it is not a driver (#426).
 *
 * ⚠️ The paragraphs above still hold: no in-kernel driver is coming back
 * here.  "console" is the exception they already describe elsewhere -- i386's
 * own comment calls its console cdev "the kernel/early-userspace printf sink"
 * -- because it drives no hardware.  It forwards to cnputc(), the same entry
 * the kernel's printf uses, over a serial port and framebuffer that
 * x86_64/ddb/cons.c has owned since the machine could print at all.
 *
 * It is here because printf_init() is the first thing every server does, and
 * until it existed the first line of userland on this target was
 * device_open("console") failing with D_NO_SUCH_DEVICE -- a correct answer
 * that left userland unable to say anything, including that.
 *
 * Slot 0 by convention: the machine-independent layer and the indirect list
 * below both expect the console first.
 *
 * WRITE-ONLY.  NO_READ and not NULL_READ, deliberately -- see console_cdev.c
 * for the null-buffer path that a "successful" read walks into.
 */
struct dev_ops	dev_name_list[] =
{
	/* name,	open,		close,		read,
	   write,	getstat,	setstat,	mmap,
	   async_in,	reset,		port_death,	subdev,
	   dev_info */
	{ "console",	consoleopen,	consoleclose,	NO_READ,
	  consolewrite,	NULL_GETS,	NULL_SETS,	NO_MMAP,
	  NO_ASYNC,	NULL_RESET,	NULL_DEATH,	0,
	  NO_DINFO },
};
int		dev_name_count = sizeof(dev_name_list)/sizeof(dev_name_list[0]);

/*
 * "console" resolves to itself.  The indirect list is how a NAME becomes a
 * device, and device_open() consults it: an empty one here would mean the
 * entry above could not be reached by the only name anybody uses.
 */
struct dev_indirect	dev_indirect_list[] = {
	{ "console",		&dev_name_list[0],	0 },
};
int			dev_indirect_count = sizeof(dev_indirect_list)
					/ sizeof(dev_indirect_list[0]);

/*
 * List of clock devices.
 *
 * ⚠️ This is NOT the timekeeping.  x86_64/time/ has the PIT and the TSC and
 * the local APIC timer, and they are what makes the scheduler tick.
 * clock_list[] is the *device* interface -- the one a task reaches through
 * host_get_clock_service().  The operations live in x86_64/time/clock_dev.c,
 * which explains what backs each of them.
 *
 * 🔥 This list used to be empty, with a comment calling that "a real gap
 * rather than a decision".  An empty list is not a missing feature here: it
 * makes host_get_clock_service() take its `clock_id >= clock_count' branch
 * for every id, so it refused every caller, so libmach's getclock() failed,
 * so every absolute deadline in libpthreads was computed from an
 * uninitialised struct.  The gap was named; its consumer was not.
 *
 * HIGHRES_CLOCK is absent rather than null-filled: kern/clock.c reads
 * clock_count, and a third entry would only be a slot that answers
 * KERN_FAILURE more slowly than a range check does.
 */
extern	struct clock_ops	rtc_ops;
extern	struct clock_ops	bbc_ops;

struct	clock	clock_list[] = {

	/* REALTIME_CLOCK */
	{ &rtc_ops,	0,		0,		0 },

	/* BATTERY_CLOCK */
	{ &bbc_ops,	0,		0,		0 },
};
int	clock_count = sizeof(clock_list) / sizeof(clock_list[0]);
