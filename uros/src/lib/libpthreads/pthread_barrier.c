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
 *   Barrier support (POSIX.1-2001)
 *
 * A barrier blocks until 'count' threads have called wait().
 * One thread receives PTHREAD_BARRIER_SERIAL_THREAD, the rest get 0.
 * Uses a phase toggle to allow safe reuse after release.
 */

#include "pthread_internals.h"

int
pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
	attr->sig = _PTHREAD_BARRIER_ATTR_SIG;
	attr->pshared = 0;
	return (ESUCCESS);
}

int
pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
	attr->sig = _PTHREAD_NO_SIG;
	return (ESUCCESS);
}

int
pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr, int *pshared)
{
	if (attr->sig != _PTHREAD_BARRIER_ATTR_SIG)
		return (EINVAL);
	if (pshared != (int *)NULL)
		*pshared = attr->pshared;
	return (ESUCCESS);
}

int
pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared)
{
	if (attr->sig != _PTHREAD_BARRIER_ATTR_SIG)
		return (EINVAL);
	if (pshared == PTHREAD_PROCESS_PRIVATE)
	{
		attr->pshared = pshared;
		return (ESUCCESS);
	}
	if (pshared == PTHREAD_PROCESS_SHARED)
		return (ENOTSUP);
	return (EINVAL);
}

int
pthread_barrier_init(pthread_barrier_t *barrier,
		     const pthread_barrierattr_t *attr,
		     unsigned count)
{
	if (count == 0)
		return (EINVAL);

	LOCK_INIT(barrier->lock);
	barrier->sig = _PTHREAD_BARRIER_SIG;
	barrier->count = (int)count;
	barrier->waiting = 0;
	barrier->phase = 0;
	/* #324: the phase toggle below is itself the futex word — no Mach
	 * semaphore is needed. */
	return (ESUCCESS);
}

int
pthread_barrier_destroy(pthread_barrier_t *barrier)
{
	if (barrier->sig != _PTHREAD_BARRIER_SIG)
		return (EINVAL);

	LOCK(barrier->lock);
	if (barrier->waiting != 0) {
		UNLOCK(barrier->lock);
		return (EBUSY);
	}
	barrier->sig = _PTHREAD_NO_SIG;
	UNLOCK(barrier->lock);
	/* #324: no kernel object to free. */
	return (ESUCCESS);
}

int
pthread_barrier_wait(pthread_barrier_t *barrier)
{
	int my_phase;

	if (barrier->sig != _PTHREAD_BARRIER_SIG)
		return (EINVAL);

	LOCK(barrier->lock);
	my_phase = barrier->phase;
	barrier->waiting++;

	if (barrier->waiting == barrier->count) {
		/* Last thread: flip the phase and release everyone */
		barrier->waiting = 0;
		__atomic_store_n(&barrier->phase, !my_phase,
				 __ATOMIC_RELEASE);
		UNLOCK(barrier->lock);
		/* Wake all (count - 1) blocked threads on the phase word */
		_pthread_futex_wake_all(&barrier->phase);
		return (PTHREAD_BARRIER_SERIAL_THREAD);
	}

	/* Not last: block while the phase still reads my_phase.  A flip done
	 * after this snapshot makes urmach_futex return at once (no lost
	 * wakeup); the loop also absorbs any spurious wakeup. */
	UNLOCK(barrier->lock);
	while (__atomic_load_n(&barrier->phase, __ATOMIC_ACQUIRE) == my_phase)
		(void) _pthread_futex_wait(&barrier->phase, my_phase, 0);
	return (ESUCCESS);
}
