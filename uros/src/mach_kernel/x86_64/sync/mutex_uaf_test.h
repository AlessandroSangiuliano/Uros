/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * -u: the x86-64 mutex's release is its last access to the mutex (#753).
 *
 * The self-test checks the property the fix establishes -- that once
 * mutex_unlock() has swapped the word free, it never touches the mutex again.
 * The probe is the instrumentation point the test drives it through; it is
 * inert unless the test arms it, and the call site is added only by
 * ablations/753-widen-release-window.patch, so a shipped kernel never calls it.
 */

#ifndef _X86_64_SYNC_MUTEX_UAF_TEST_H_
#define _X86_64_SYNC_MUTEX_UAF_TEST_H_

#include <kern/lock.h>

/* The boot self-test (-u). */
extern void mutex_uaf_test(void);

/*
 * The probe: stands in for the processor that, in #753, takes the lock on the
 * fast path the instant the release frees the word, drops the last reference
 * and frees the object.  Called from inside mutex_unlock() right after the
 * swap, by the widen patch only.  Inert unless mutex_uaf_arm() armed it for
 * this mutex; when it fires it scribbles a sentinel over the mutex, as a free
 * and reuse would, so that any access the release makes after this point is
 * seen.
 */
extern volatile int mutex_uaf_armed;
extern void mutex_uaf_probe(mutex_t *m);

#endif /* _X86_64_SYNC_MUTEX_UAF_TEST_H_ */
