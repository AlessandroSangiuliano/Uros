/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Make a thread that is blocked with a continuation, and open the prompt on
 * it (#428).  Behind `-L'.
 *
 * The reasoning is in cont_probe.c.  In one line: the debugger's rule about
 * continuations had no subject on this target -- all three callers that block
 * with one need something this kernel has not got -- so the rule was written
 * and never executed.
 */

#ifndef _X86_64_DDB_CONT_PROBE_H_
#define _X86_64_DDB_CONT_PROBE_H_

void	cont_probe_start(void);

#endif	/* _X86_64_DDB_CONT_PROBE_H_ */
