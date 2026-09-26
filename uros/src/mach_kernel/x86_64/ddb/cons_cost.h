/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * What one line on the console costs, said once per boot (#551).
 */

#ifndef _X86_64_DDB_CONS_COST_H_
#define _X86_64_DDB_CONS_COST_H_

void cons_cost_report(void);

/* Where this boot's console bytes were handed over (#567): the final copy,
 * once, at halt. */
void cons_ring_report(void);

/* The same lines, every call: each begins with lead and says when (#599). */
void cons_ring_lines(const char *lead, const char *when);

#endif	/* _X86_64_DDB_CONS_COST_H_ */
