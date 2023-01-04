/* 
 * Mach Operating System
 * Copyright (c) 1991,1990 Carnegie Mellon University
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

#ifndef	_WRITE_H
#define	_WRITE_H

#include <stdio.h>

#include "statement.h"

extern void WriteUserHeader(FILE *file, const statement_t *stats);
extern void WriteServerHeader(FILE *file, const statement_t *stats);
extern void WriteInternalHeader(FILE *file, const statement_t *stats);
extern void WriteUser(FILE *file, const statement_t *stats);
extern void WriteUserIndividual(const statement_t *stats);
extern void WriteServer(FILE *file, const statement_t *stats);

#endif	/* _WRITE_H */
