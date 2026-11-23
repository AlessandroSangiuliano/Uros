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
 * chora_drives.h -- what a module of chora says it drives (#964).
 *
 * A module lists the devices it drives in one section of its ELF,
 * CHORA_DRIVES_SECTION: each line a vendor and a device, or a class under a
 * mask.  The HAL fetches the module's file from its pool and reads that
 * section from the bytes, with libelf, and never loads the module: no code of a
 * driver runs in the HAL.  It is how Linux's modules.alias is made from a
 * module's device table, without running the module.
 *
 * A module declares with one line of its source:
 *
 *	CHORA_DRIVES(CHORA_DRIVES_PCI_ID(0x1234, 0x11e8));
 *
 * A module that declares nothing is started by nobody but whoever names it.
 *
 * chora_drives.c reads a section; it is pure, so that the HAL and a test on
 * the build machine read the same way.
 */

#ifndef _CHORA_DRIVES_H_
#define _CHORA_DRIVES_H_

#include <stddef.h>
#include <stdint.h>

#define CHORA_DRIVES_SECTION	".uros_drives"

#define CHORA_DRIVES_ID		1	/* a vendor and a device */
#define CHORA_DRIVES_CLASS	2	/* a class code, under a mask */

/*
 * One line, 16 bytes, the same on every target: the HAL reads lines a module
 * of another build wrote.  A field the line's kind does not read is 0.
 */
struct chora_drives {
	uint16_t	kind;		/* CHORA_DRIVES_ID or CHORA_DRIVES_CLASS */
	uint16_t	vendor;		/* ID: as configuration space's 00h */
	uint16_t	device;		/* ID: as its 02h */
	uint16_t	zero;
	uint32_t	class_code;	/* CLASS: base << 16 | sub << 8 | interface */
	uint32_t	class_mask;	/* CLASS: which of those bits are compared */
};

_Static_assert(sizeof(struct chora_drives) == 16,
	       "a line of a module's declaration is 16 bytes on every target");

/* The most lines the HAL reads of one module. */
#define CHORA_DRIVES_MAX	32

#define CHORA_DRIVES_PCI_ID(v, d) \
	{ .kind = CHORA_DRIVES_ID, .vendor = (v), .device = (d) }
#define CHORA_DRIVES_PCI_CLASS(c, m) \
	{ .kind = CHORA_DRIVES_CLASS, .class_code = (c), .class_mask = (m) }

/*
 * The declaration, in its section.  `used' keeps a table that nothing in the
 * module refers to; the section keeps it apart, where the HAL looks.
 */
#define CHORA_DRIVES(...)						\
	static const struct chora_drives chora_drives_table[]		\
	__attribute__((section(CHORA_DRIVES_SECTION), used, aligned(4))) = \
	{ __VA_ARGS__ }

/*
 * Why a declaration was refused: the line, from 0, and what is wrong with it.
 * `line' is -1 when what is wrong is the section's size.
 */
struct chora_drives_fault {
	int		line;
	const char	*what;
};

/*
 * The lines of a section, when every line is one the HAL reads: from 1 to
 * CHORA_DRIVES_MAX.  -1 otherwise, with *fault saying why.  A declaration the
 * HAL cannot read whole is not believed in part.
 */
int	chora_drives_check(const void *section, size_t size,
			   struct chora_drives_fault *fault);

/*
 * How a section chora_drives_check() took names a device, from its
 * configuration space's 00h and 08h: CHORA_DRIVES_BY_ID when a line names its
 * vendor and device, CHORA_DRIVES_BY_CLASS when only a class line covers it,
 * 0 when none does.
 */
#define CHORA_DRIVES_BY_CLASS	1
#define CHORA_DRIVES_BY_ID	2

int	chora_drives_score(const void *section, int lines,
			   uint32_t vendor_device, uint32_t class_rev);

/*
 * Which of n modules drives a device, from each one's score: the one with the
 * highest, a vendor and device before a class.  Two with the same highest score
 * are a choice the HAL does not make: both would claim the device, and the
 * kernel would refuse the second.
 */
#define CHORA_PICK_NONE		0	/* no module names the device */
#define CHORA_PICK_ONE		1	/* *chosen */
#define CHORA_PICK_TIE		2	/* *chosen and *rival, the first two */

int	chora_drives_pick(const int *score, int n, int *chosen, int *rival);

#endif /* _CHORA_DRIVES_H_ */
