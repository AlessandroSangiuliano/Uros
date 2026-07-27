/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * How much thread state a message can carry — x86-64 (#416, deciding for
 * #408).
 *
 * `thread_state_t` is declared in mach_types.defs as an inline array of at
 * most THREAD_STATE_MAX words, so this number is the ceiling on every flavour
 * of state that travels through thread_get_state and thread_set_state.  A
 * flavour larger than it cannot be carried at all — which is not a failure
 * that announces itself, because the count argument simply comes back short.
 *
 * ⚠️ It is a literal, and it has to be: this header is read by the C
 * preprocessor while it is chewing a .defs file, so there is no sizeof
 * available and no structure in scope.  What keeps it honest is the other
 * side — x86_64/thread/state.h includes this file and asserts that the
 * largest flavour it defines still fits.  Change a state structure and the
 * kernel stops building until this number is changed with it.
 *
 * The largest flavour is the floating-point one: four bytes of validity,
 * four of padding and the 512-byte image the processor's own save
 * instruction writes, which is 520 bytes and therefore 130 words of
 * natural_t.  The registers are 176 bytes (44 words) and the exception state
 * 24 (6), so neither of them sets the ceiling.
 *
 * ⚠️ It is much larger than i386's 32, and deliberately.  There
 * THREAD_MACHINE_STATE_MAX is the *register* state and the floating-point
 * flavour does not fit in a thread_state_t at all — a limit inherited rather
 * than chosen, and not one worth reproducing on a target that has room.
 */

#ifndef	_MACH_X86_64_THREAD_STATE_H_
#define _MACH_X86_64_THREAD_STATE_H_

#define THREAD_STATE_MAX	130

#endif	/* _MACH_X86_64_THREAD_STATE_H_ */
