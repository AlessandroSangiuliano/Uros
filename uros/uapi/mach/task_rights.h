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
 * mach/task_rights.h -- rights on a task, and the reduced ports that carry
 * some of them (#927).
 *
 * A send right to a task port is the task: its memory, its threads and its
 * port rights.  A process manager needs far less -- to end a task, stop it,
 * let it go on, read its counters and learn of its death -- and a server
 * that held full task ports for that held everything else as well.
 *
 * So the kernel names rights on a task, and a routine that takes a task
 * names the one it needs.  A reduced port carries a set of them:
 *
 *   - it is made from the full task port, by urmach_task_rights_port(), and
 *     from nothing else: a right can be taken away and never added;
 *   - there is one per set and per task: asking twice for one set gives the
 *     same port, so two registrations of one process name it alike;
 *   - it dies with the task, so a dead-name request on it says the task has
 *     died, as one on the task port does.
 *
 * The full task port carries every right: those named here, and everything
 * a routine that names none of them reaches -- memory, threads, port
 * rights, special ports.  A reduced port reaches none of that.
 *
 * The set with no bits is the task's identity: a port that names the task
 * and dies with it, and that no routine accepts yet (#938).  A right is
 * added here when a routine comes to name it -- reading a task's memory for
 * a debugger, say -- and not before: a bit nothing checks would read as a
 * right and be none.
 */

#ifndef	_MACH_TASK_RIGHTS_H_
#define	_MACH_TASK_RIGHTS_H_

/* task_info(): the task's counters and times, to read. */
#define	URMACH_TASK_RIGHT_INFO		0x00000001

/* task_terminate(), task_suspend(), task_resume(). */
#define	URMACH_TASK_RIGHT_LIFECYCLE	0x00000002

/* Every right a reduced port can carry today; urmach_task_rights_port()
 * refuses any other bit. */
#define	URMACH_TASK_RIGHTS_REDUCIBLE	(URMACH_TASK_RIGHT_INFO | \
					 URMACH_TASK_RIGHT_LIFECYCLE)

/* What a process is registered with proc_server by (#927). */
#define	URMACH_TASK_RIGHTS_PROCESS	(URMACH_TASK_RIGHT_INFO | \
					 URMACH_TASK_RIGHT_LIFECYCLE)

#endif	/* _MACH_TASK_RIGHTS_H_ */
