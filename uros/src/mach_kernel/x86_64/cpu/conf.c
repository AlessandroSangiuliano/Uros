/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What devices this machine has, and what clocks (#453).
 *
 * The machine-independent device layer looks names up in dev_name_list[] and
 * resolves aliases through dev_indirect_list[]; the clock layer reads
 * clock_list[].  Both tables are the machine's own, and both are EMPTY here,
 * which is a statement rather than a placeholder.
 *
 * ── Why they are empty, and why that is not "unfinished" ──────────────
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
 * claim hardware.
 *
 * ⚠️ An empty table is not the same as a missing one.  dev_lookup() walks
 * dev_name_count entries and finds nothing, which is a device open that
 * fails with D_NO_SUCH_DEVICE -- the correct answer for a device that is not
 * there.  Leaving the symbols undefined would instead be a kernel that does
 * not link, and inventing a stub console here would be a device that reports
 * success and moves no bytes.
 */

#include <device/conf.h>
#include <kern/clock.h>

/*
 * No in-kernel device drivers.  See above -- this is the design, not a gap.
 *
 * The array has one unused element because C has no zero-length array and a
 * flexible one cannot be a definition; dev_name_count is zero, so nothing
 * ever reads it.  That is the smallest way to say "none" that the language
 * allows without a pointer the callers would have to check.
 */
struct dev_ops	dev_name_list[1];
int		dev_name_count = 0;

/*
 * No aliases either, for the same reason: an alias names a device in the
 * table above, and there are none.
 */
struct dev_indirect	dev_indirect_list[1];
int			dev_indirect_count = 0;

/*
 * No clock devices.
 *
 * ⚠️ This is NOT the same as having no timekeeping.  x86_64/time/ has the
 * PIT and the TSC and the local APIC timer, and they are what makes the
 * scheduler tick.  clock_list[] is the *device* interface -- the one a task
 * opens to read or set a wall clock, which on i386 is the battery-backed
 * RTC.
 *
 * That is a real gap rather than a decision, and it is a small one with a
 * clear shape: a wall clock belongs to whoever owns the RTC, and on this
 * machine that will be a user-space server for the same reason every other
 * device driver is.  #318 is where the timebase itself is settled.
 */
struct clock	clock_list[1];
int		clock_count = 0;
