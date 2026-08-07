/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/ndr_def.h> for x86-64 (#450).
 *
 * The data representation this machine stamps into every MIG message: byte
 * order, character set, floating-point format.  A receiver that disagrees is
 * supposed to convert rather than misread, so this is the one place where an
 * architecture states how it lays bytes down.
 *
 * x86-64 answers the same as i386 on all three -- little endian, ASCII,
 * IEEE -- which is a fact about the two architectures, not an inheritance:
 * the values are stated here because the MI tree asks this machine, and it
 * would be equally correct for a future target to answer differently.
 *
 * Unlike its neighbours this header defines rather than declares, so exactly
 * one translation unit may include it.
 */

#ifndef _X86_64_NDR_DEF_H_
#define _X86_64_NDR_DEF_H_

#include <mach/ndr.h>

NDR_record_t NDR_record = {
	0,				/* mig_reserved */
	0,				/* mig_reserved */
	0,				/* mig_reserved */
	NDR_PROTOCOL_2_0,
	NDR_INT_LITTLE_ENDIAN,
	NDR_CHAR_ASCII,
	NDR_FLOAT_IEEE,
	0,
};

#endif /* _X86_64_NDR_DEF_H_ */
