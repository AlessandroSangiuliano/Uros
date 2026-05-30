/*
 * Copyright (c) 2026 Alessandro Sangiuliano <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * POSIX Pthread Library
 *   Spinlock support (POSIX.1-2001)
 *
 * Thin wrapper over the existing _spin_lock/_spin_unlock primitives
 * from AT386/i386_lock.S.
 */

#include "pthread_internals.h"

int
pthread_spin_init(pthread_spinlock_t *lock, int pshared)
{
	lock->sig = _PTHREAD_SPIN_SIG;
	LOCK_INIT(lock->spinlock);
	return (ESUCCESS);
}

int
pthread_spin_destroy(pthread_spinlock_t *lock)
{
	if (lock->sig != _PTHREAD_SPIN_SIG)
		return (EINVAL);
	lock->sig = _PTHREAD_NO_SIG;
	return (ESUCCESS);
}

int
pthread_spin_lock(pthread_spinlock_t *lock)
{
	if (lock->sig != _PTHREAD_SPIN_SIG)
		return (EINVAL);
	LOCK(lock->spinlock);
	return (ESUCCESS);
}

int
pthread_spin_trylock(pthread_spinlock_t *lock)
{
	if (lock->sig != _PTHREAD_SPIN_SIG)
		return (EINVAL);
	/*
	 * _spin_lock is blocking; for trylock we test atomically.
	 * pthread_lock_t is an int — 0 means unlocked.
	 */
	if (__sync_lock_test_and_set(&lock->spinlock, 1) != 0)
		return (EBUSY);
	return (ESUCCESS);
}

int
pthread_spin_unlock(pthread_spinlock_t *lock)
{
	if (lock->sig != _PTHREAD_SPIN_SIG)
		return (EINVAL);
	UNLOCK(lock->spinlock);
	return (ESUCCESS);
}
