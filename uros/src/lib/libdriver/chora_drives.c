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
 * chora_drives.c -- the reading of a module's declaration (#964).
 *
 * Pure: bytes in, a number out, no RPC and no allocation.  The HAL calls it on
 * the bytes of a module it fetched, and chora_drives_test on lines it wrote,
 * on the build machine.  Not part of libdriver: a module only writes a
 * declaration, with chora_drives.h's macros, and the HAL compiles this file
 * among its own, as pci_scan does libpci's pci_bar.c.
 */

#include <string.h>

#include "chora_drives.h"

/*
 * A line copied out, since the section lies where the file put it: the HAL
 * reads a file it was handed, and makes no promise about its alignment.
 */
static void
line_at(const void *section, int i, struct chora_drives *out)
{
	memcpy(out, (const unsigned char *)section + (size_t)i * sizeof *out,
	       sizeof *out);
}

static int
refuse(struct chora_drives_fault *fault, int line, const char *what)
{
	fault->line = line;
	fault->what = what;
	return -1;
}

int
chora_drives_check(const void *section, size_t size,
		   struct chora_drives_fault *fault)
{
	struct chora_drives	d;
	size_t			n;
	int			i;

	if (section == NULL || size == 0)
		return refuse(fault, -1, "no line");
	if (size % sizeof d != 0)
		return refuse(fault, -1, "a size that is no whole number of lines");
	n = size / sizeof d;
	if (n > CHORA_DRIVES_MAX)
		return refuse(fault, -1, "more lines than the HAL reads");

	for (i = 0; i < (int)n; i++) {
		line_at(section, i, &d);
		if (d.zero != 0)
			return refuse(fault, i, "its unused field is not 0");
		switch (d.kind) {
		case CHORA_DRIVES_ID:
			if (d.class_code != 0 || d.class_mask != 0)
				return refuse(fault, i, "a vendor and device "
					      "line that names a class");
			/* What configuration space reads where no device is. */
			if (d.vendor == 0xffffu)
				return refuse(fault, i, "vendor ffff, which no "
					      "device has");
			break;
		case CHORA_DRIVES_CLASS:
			if (d.vendor != 0 || d.device != 0)
				return refuse(fault, i, "a class line that "
					      "names a vendor or device");
			/* A mask of nothing would be every device. */
			if (d.class_mask == 0)
				return refuse(fault, i, "a class line whose "
					      "mask compares nothing");
			if (((d.class_code | d.class_mask) >> 24) != 0)
				return refuse(fault, i, "a class wider than "
					      "24 bits");
			if ((d.class_code & ~d.class_mask) != 0)
				return refuse(fault, i, "class bits its mask "
					      "does not compare");
			break;
		default:
			return refuse(fault, i, "a kind the HAL does not read");
		}
	}
	fault->line = -1;
	fault->what = NULL;
	return (int)n;
}

int
chora_drives_score(const void *section, int lines, uint32_t vendor_device,
		   uint32_t class_rev)
{
	struct chora_drives	d;
	uint32_t		class_code = class_rev >> 8;
	int			i, score = 0;

	for (i = 0; i < lines; i++) {
		line_at(section, i, &d);
		if (d.kind == CHORA_DRIVES_ID &&
		    d.vendor == (vendor_device & 0xffffu) &&
		    d.device == (vendor_device >> 16))
			return CHORA_DRIVES_BY_ID;
		/* chora_drives_check() took no bit of the code outside the mask. */
		if (d.kind == CHORA_DRIVES_CLASS &&
		    (class_code & d.class_mask) == d.class_code)
			score = CHORA_DRIVES_BY_CLASS;
	}
	return score;
}

int
chora_drives_pick(const int *score, int n, int *chosen, int *rival)
{
	int	i, best = 0, first = -1, second = -1;

	for (i = 0; i < n; i++) {
		if (score[i] > best) {
			best = score[i];
			first = i;
			second = -1;
		} else if (best > 0 && score[i] == best && second < 0) {
			second = i;
		}
	}
	if (first < 0)
		return CHORA_PICK_NONE;
	*chosen = first;
	if (second < 0)
		return CHORA_PICK_ONE;
	*rival = second;
	return CHORA_PICK_TIE;
}
