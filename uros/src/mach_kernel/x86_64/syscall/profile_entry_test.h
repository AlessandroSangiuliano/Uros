/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A profiled trap taken off its processor between the entry and the dispatch
 * (#622, -e).
 */

#ifndef _X86_64_SYSCALL_PROFILE_ENTRY_TEST_H_
#define _X86_64_SYSCALL_PROFILE_ENTRY_TEST_H_

/*
 * Spinners free to move, two a processor at a user thread's priority, while
 * the userland boots; they stop when twenty profiled traps have been taken
 * off their processor between the entry and the dispatch (sixty seconds at
 * most).  Then the profile's counts say whether any of them opened with an
 * earlier call's reading.
 * Prints "profile_entry: PASS", "WRONG" or "NOT ASKED" when it ends, and
 * returns at once: the boot goes on into bootstrap_create() under it.
 */
void profile_entry_test_start(void);

#endif	/* _X86_64_SYSCALL_PROFILE_ENTRY_TEST_H_ */
