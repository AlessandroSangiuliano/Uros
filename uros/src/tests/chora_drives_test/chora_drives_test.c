/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 */

/*
 * chora_drives_test.c -- the reading of a module's declaration, on the build
 * machine (#964).
 *
 * A boot shows the HAL reading the modules there are: chora_probe and
 * edu_probe each declare their device, chora_absent nothing.  What it cannot
 * show is a declaration nobody has written: two modules that name one device
 * alike, a class line against a vendor and device line, a line the HAL must
 * refuse.  chora_drives.c is pure, so those are lines written here.
 *
 *   [1]  lines the HAL reads are taken, and counted
 *   [2]  a section that is no whole number of lines, or none, is refused
 *   [3]  each malformed line is refused, and named by its index
 *   [4]  a vendor and device line names its device and no other
 *   [5]  a class line compares the bits of its mask, the interface included
 *   [6]  a vendor and device line comes before a class line
 *   [7]  the module with the highest score is chosen; two alike are not
 */

#include <stdio.h>
#include <string.h>

#include "chora_drives.h"

static int	passed, failed;

static void
arm(const char *name, int ok, const char *wrong)
{
	if (ok) {
		printf("chora_drives_test: %s: OK\n", name);
		passed++;
		return;
	}
	printf("chora_drives_test: %s: WRONG -- %s\n", name, wrong);
	failed++;
}

/* A section refused, where it should be, at that line and for that reason. */
static void
refused(const char *name, const struct chora_drives *lines, size_t size,
	int line, const char *what)
{
	struct chora_drives_fault	f = { 99, NULL };
	int				n = chora_drives_check(lines, size, &f);
	char				wrong[160];

	(void)snprintf(wrong, sizeof wrong, "answered %d, line %d, \"%s\"", n,
		       f.line, f.what != NULL ? f.what : "(none)");
	arm(name, n == -1 && f.line == line && f.what != NULL &&
	    strcmp(f.what, what) == 0, wrong);
}

/* QEMU's 82540EM, network class 02h, revision 3; its AHCI, class 010601h. */
#define E1000_VD	0x100e8086u
#define E1000_CLASS	0x02000003u
#define E1000E_VD	0x10d38086u
#define AHCI_VD		0x29228086u
#define AHCI_CLASS	0x01060102u

int
main(void)
{
	printf("chora_drives_test: the reading of a module's declaration, "
	       "%u-bit build, no module\n", (unsigned)(sizeof(void *) * 8));

	/* [1] Lines the HAL reads are taken, and counted. */
	{
		const struct chora_drives two[] = {
			CHORA_DRIVES_PCI_ID(0x8086, 0x100e),
			CHORA_DRIVES_PCI_CLASS(0x020000, 0xffff00),
		};
		struct chora_drives_fault f = { 99, "unset" };

		arm("[1] two lines read are two",
		    chora_drives_check(two, sizeof two, &f) == 2 &&
		    f.line == -1 && f.what == NULL,
		    "a section of two good lines was not counted as two");
	}

	/* [2] The section's size. */
	{
		const struct chora_drives one[] = {
			CHORA_DRIVES_PCI_ID(0x8086, 0x100e),
		};
		struct chora_drives many[CHORA_DRIVES_MAX + 1];
		int i;

		for (i = 0; i < CHORA_DRIVES_MAX + 1; i++)
			many[i] = one[0];
		refused("[2] no line", one, 0, -1, "no line");
		refused("[2] half a line", one, sizeof one - 1, -1,
			"a size that is no whole number of lines");
		refused("[2] a line past the most", many, sizeof many, -1,
			"more lines than the HAL reads");
		{
			struct chora_drives_fault f = { 99, NULL };

			arm("[2] the most lines are read",
			    chora_drives_check(many, sizeof many - sizeof one,
					       &f) == CHORA_DRIVES_MAX,
			    "CHORA_DRIVES_MAX lines were refused");
		}
	}

	/* [3] Each malformed line, the second of two, so its index shows. */
	{
		struct chora_drives l[2] = {
			CHORA_DRIVES_PCI_ID(0x8086, 0x100e),
			CHORA_DRIVES_PCI_ID(0x8086, 0x10d3),
		};
		const struct chora_drives good = l[1];

		l[1].zero = 1;
		refused("[3] the unused field set", l, sizeof l, 1,
			"its unused field is not 0");
		l[1] = good;
		l[1].class_mask = 0xff0000;
		refused("[3] a device line with a class", l, sizeof l, 1,
			"a vendor and device line that names a class");
		l[1] = good;
		l[1].vendor = 0xffff;
		refused("[3] vendor ffff", l, sizeof l, 1,
			"vendor ffff, which no device has");
		l[1] = (struct chora_drives)CHORA_DRIVES_PCI_CLASS(0x020000,
								    0xffff00);
		l[1].device = 0x100e;
		refused("[3] a class line with a device", l, sizeof l, 1,
			"a class line that names a vendor or device");
		l[1] = (struct chora_drives)CHORA_DRIVES_PCI_CLASS(0, 0);
		refused("[3] a mask of nothing", l, sizeof l, 1,
			"a class line whose mask compares nothing");
		l[1] = (struct chora_drives)CHORA_DRIVES_PCI_CLASS(0x02000000,
								    0xff000000);
		refused("[3] a class of 32 bits", l, sizeof l, 1,
			"a class wider than 24 bits");
		l[1] = (struct chora_drives)CHORA_DRIVES_PCI_CLASS(0x020001,
								    0xffff00);
		refused("[3] a bit outside the mask", l, sizeof l, 1,
			"class bits its mask does not compare");
		l[1] = good;
		l[1].kind = 3;
		refused("[3] a kind of no line", l, sizeof l, 1,
			"a kind the HAL does not read");
	}

	/* [4] A vendor and device line names its device and no other. */
	{
		const struct chora_drives l[] = {
			CHORA_DRIVES_PCI_ID(0x8086, 0x100e),
		};

		arm("[4] 8086:100e names the 82540EM",
		    chora_drives_score(l, 1, E1000_VD, E1000_CLASS) ==
		    CHORA_DRIVES_BY_ID, "its own device was not named");
		arm("[4] 8086:100e does not name the 82574L",
		    chora_drives_score(l, 1, E1000E_VD, E1000_CLASS) == 0,
		    "another device of the same vendor was named");
		/* Vendor and device the other way round: 100e:8086. */
		arm("[4] the halves are not swapped",
		    chora_drives_score(l, 1, 0x8086100eu, E1000_CLASS) == 0,
		    "a device was named by its halves swapped");
	}

	/* [5] A class line compares the bits of its mask. */
	{
		const struct chora_drives net[] = {
			CHORA_DRIVES_PCI_CLASS(0x020000, 0xffff00),
		};
		const struct chora_drives ahci[] = {
			CHORA_DRIVES_PCI_CLASS(0x010601, 0xffffff),
		};
		const struct chora_drives ide[] = {
			CHORA_DRIVES_PCI_CLASS(0x010600, 0xffffff),
		};
		const struct chora_drives sata[] = {
			CHORA_DRIVES_PCI_CLASS(0x010600, 0xffff00),
		};

		arm("[5] network, any interface, names the 82540EM",
		    chora_drives_score(net, 1, E1000_VD, E1000_CLASS) ==
		    CHORA_DRIVES_BY_CLASS, "the network class was not named");
		arm("[5] network does not name a disk",
		    chora_drives_score(net, 1, AHCI_VD, AHCI_CLASS) == 0,
		    "the network class named a disk controller");
		arm("[5] interface 01 names AHCI",
		    chora_drives_score(ahci, 1, AHCI_VD, AHCI_CLASS) ==
		    CHORA_DRIVES_BY_CLASS, "AHCI's interface was not named");
		arm("[5] interface 00 does not name AHCI",
		    chora_drives_score(ide, 1, AHCI_VD, AHCI_CLASS) == 0,
		    "the interface byte was not compared");
		arm("[5] a mask without the interface names AHCI",
		    chora_drives_score(sata, 1, AHCI_VD, AHCI_CLASS) ==
		    CHORA_DRIVES_BY_CLASS,
		    "a byte outside the mask was compared");
	}

	/* [6] A vendor and device line comes before a class line. */
	{
		const struct chora_drives l[] = {
			CHORA_DRIVES_PCI_CLASS(0x020000, 0xffff00),
			CHORA_DRIVES_PCI_ID(0x8086, 0x100e),
		};

		arm("[6] the device's line counts, the class's first or not",
		    chora_drives_score(l, 2, E1000_VD, E1000_CLASS) ==
		    CHORA_DRIVES_BY_ID,
		    "a class line written first hid the device's line");
	}

	/* [7] Which module. */
	{
		const int none[] = { 0, 0 };
		const int one[] = { 1, 2, 1 };
		const int tie[] = { 2, 0, 2 };
		const int late[] = { 1, 2, 2 };
		const int over[] = { 2, 1, 2, 2 };
		int c = -1, r = -1;

		arm("[7] none names it",
		    chora_drives_pick(none, 2, &c, &r) == CHORA_PICK_NONE,
		    "a device no module names was given one");
		arm("[7] no module at all",
		    chora_drives_pick(none, 0, &c, &r) == CHORA_PICK_NONE,
		    "an empty pool chose a module");
		c = r = -1;
		arm("[7] the device's line before two class lines",
		    chora_drives_pick(one, 3, &c, &r) == CHORA_PICK_ONE &&
		    c == 1, "the highest score was not the one chosen");
		c = r = -1;
		arm("[7] two alike are a tie, the first two named",
		    chora_drives_pick(tie, 3, &c, &r) == CHORA_PICK_TIE &&
		    c == 0 && r == 2, "two modules that name the device alike "
		    "were not a tie");
		c = r = -1;
		arm("[7] a tie found after a lower score",
		    chora_drives_pick(late, 3, &c, &r) == CHORA_PICK_TIE &&
		    c == 1 && r == 2, "the tie above a lower score was missed");
		c = r = -1;
		arm("[7] a third alike changes nothing",
		    chora_drives_pick(over, 4, &c, &r) == CHORA_PICK_TIE &&
		    c == 0 && r == 2, "the rival was not the second of the "
		    "three");
	}

	printf("chora_drives_test: %d of %d arms passed\n", passed,
	       passed + failed);
	/* Zero is success: ctest reads the exit code. */
	return failed == 0 ? 0 : 1;
}
