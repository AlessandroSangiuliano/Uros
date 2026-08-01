/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * <machine/task.h> for x86-64 (#450).
 *
 * Whether a task carries machine state of its own.  The macro is defined
 * and empty: kern/task.h expands it inside struct task, so defining it to
 * nothing adds no member, and the name has to exist for the expansion.
 */

#ifndef _X86_64_TASK_H_
#define _X86_64_TASK_H_

#define	MACHINE_TASK

#endif /* _X86_64_TASK_H_ */
