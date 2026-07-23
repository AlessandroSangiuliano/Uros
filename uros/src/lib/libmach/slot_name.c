/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS 
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 */
/*
 * MkLinux
 */
/*
 *	File:	slot_name.c
 *	Author:	Avadis Tevanian, Jr.
 *	Date:	Feb 1987
 *
 *	Convert machine slot values to human readable strings.
 */

#include <mach.h>

/*
 *	Convert the specified cpu_type/cpu_subtype pair to their
 *	human readable form.
 */
void
slot_name(
	cpu_type_t	cpu_type,
	cpu_subtype_t	cpu_subtype,
	char		**cpu_name,
	char		**cpu_subname)
{
	const char *name, *subname;

	switch (cpu_type) {
	case CPU_TYPE_I386:
		name = "I386";
		switch (cpu_subtype) {
		case CPU_SUBTYPE_AT386: subname = "AT"; break;
		case CPU_SUBTYPE_EXL: subname = "EXL"; break;
		case CPU_SUBTYPE_iPSC386: subname = "PSC386"; break;
		case CPU_SUBTYPE_SYMMETRY: subname = "Symmetry"; break;
		case CPU_SUBTYPE_CBUS: subname = "CBUS"; break;
		case CPU_SUBTYPE_MBUS: subname = "MBUS"; break;
		default: 	 subname = "Unknown Subtype";  break;
		}
		break;
	case CPU_TYPE_I486:
		name = "I486";
		switch (cpu_subtype) {
		case CPU_SUBTYPE_AT386: subname = "AT"; break;
		case CPU_SUBTYPE_EXL: subname = "EXL"; break;
		case CPU_SUBTYPE_iPSC386: subname = "PSC386"; break;
		case CPU_SUBTYPE_SYMMETRY: subname = "Symmetry"; break;
		case CPU_SUBTYPE_CBUS: subname = "CBUS"; break;
		case CPU_SUBTYPE_MBUS: subname = "MBUS"; break;
		case CPU_SUBTYPE_MPS: subname = "Intel MPS"; break;
		default: 	 subname = "Unknown Subtype";  break;
		}
		break;
	case CPU_TYPE_PENTIUM:
		name = "Pentium";
		switch (cpu_subtype) {
		case CPU_SUBTYPE_AT386: subname = "AT"; break;
		case CPU_SUBTYPE_MPS: subname = "Intel MPS"; break;
		default: 	 subname = "Unknown Subtype";  break;
		}
		break;
	case CPU_TYPE_PENTIUMPRO:
		name = "Pentium Pro";
		switch (cpu_subtype) {
		case CPU_SUBTYPE_AT386: subname = "AT"; break;
		case CPU_SUBTYPE_MPS: subname = "Intel MPS"; break;
		default: 	 subname = "Unknown Subtype";  break;
		}
		break;
	case CPU_TYPE_POWERPC:
		name = "POWERPC";
		switch (cpu_subtype >> 16) {
		case CPU_SUBTYPE_PPC601:	subname = "PPC-601";	break;
		case CPU_SUBTYPE_PPC603:	subname = "PPC-603";	break;
		case CPU_SUBTYPE_PPC604:	subname = "PPC-604";	break;
		case CPU_SUBTYPE_PPC603e:	subname = "PPC-603e";	break;
		case CPU_SUBTYPE_PPC603ev:	subname = "PPC-603ev";	break;
		case CPU_SUBTYPE_PPC604e:	subname = "PPC-604e";	break;
		case CPU_SUBTYPE_PPC620:	subname = "PPC-620";	break;
		default:		subname = "Unknown Subtype";	break;
		}
		break;
	default:
		name = "Unknown CPU";
		subname = "";
		break;
	}
	*cpu_name = (char *)name;
	*cpu_subname = (char *)subname;
}
