/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A shootdown whose thread is moved half-way (#638, -J).
 */

#ifndef _X86_64_PMAP_SHOOTDOWN_TEST_H_
#define _X86_64_PMAP_SHOOTDOWN_TEST_H_

/*
 * Two arms on threads free to move, each until twenty operations have moved
 * to another processor (thirty seconds at most): [1] pages repointed and shot
 * down, then read back on the processor the remapper ended on and by a reader
 * bound to each processor; [2] cross-calls sent to every processor, the
 * sender's own included.  Prints "shootdown_test: [n] PASS", "WRONG" or
 * "NOT ASKED" for each, and returns: the boot goes on.  A cross-call sent to
 * its own sender never returns; it ends the boot with the panic that names it.
 */
void shootdown_moved_test(void);

#endif	/* _X86_64_PMAP_SHOOTDOWN_TEST_H_ */
