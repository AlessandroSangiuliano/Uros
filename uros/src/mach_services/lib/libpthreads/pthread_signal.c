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
 * POSIX per-thread signals for Uros.
 *
 * Entirely userspace implementation using Mach semaphores for wakeup.
 * SIGKILL and SIGSTOP cannot be blocked (POSIX requirement).
 */

#include "pthread_internals.h"

/* Signals that cannot be blocked or caught (POSIX) */
#define _SIG_CANTBLOCK	((1UL << (SIGKILL - 1)) | (1UL << (SIGSTOP - 1)))

static int
_sig_valid(int sig)
{
	return (sig >= 1 && sig < _NSIG);
}

/*
 * pthread_sigmask — examine and/or change the signal mask of the calling thread.
 */
int
pthread_sigmask(int how, const sigset_t *set, sigset_t *oldset)
{
	pthread_t self = pthread_self();

	LOCK(self->lock);
	if (oldset != (sigset_t *)NULL)
		*oldset = self->sigmask;
	if (set != (const sigset_t *)NULL)
	{
		switch (how)
		{
		case SIG_BLOCK:
			self->sigmask |= *set;
			break;
		case SIG_UNBLOCK:
			self->sigmask &= ~(*set);
			break;
		case SIG_SETMASK:
			self->sigmask = *set;
			break;
		default:
			UNLOCK(self->lock);
			return (EINVAL);
		}
		/* SIGKILL and SIGSTOP can never be blocked */
		self->sigmask &= ~_SIG_CANTBLOCK;
	}
	UNLOCK(self->lock);
	return (ESUCCESS);
}

/*
 * pthread_kill — send a signal to a specific thread.
 *
 * If sig == 0, just validate the thread (error check, no signal sent).
 * Otherwise, add the signal to the thread's pending set and wake it
 * if it is blocked in sigwait() waiting for that signal.
 */
int
pthread_kill(pthread_t thread, int sig)
{
	if (thread->sig != _PTHREAD_SIG)
		return (ESRCH);
	if (sig == 0)
		return (ESUCCESS);		/* Just a validity check */
	if (!_sig_valid(sig))
		return (EINVAL);

	LOCK(thread->lock);
	thread->sigpending |= (1UL << (sig - 1));
	/*
	 * If the thread is blocked in sigwait() and this signal is in its
	 * wait set (sig_sem != MACH_PORT_NULL), wake it up.
	 */
	if (thread->sig_sem != MACH_PORT_NULL)
	{
		kern_return_t kern_res;
		MACH_CALL(semaphore_signal(thread->sig_sem), kern_res);
	}
	UNLOCK(thread->lock);
	return (ESUCCESS);
}

/*
 * sigwait — wait for a signal from the specified set.
 *
 * Atomically: check for pending signals in `set` that are not blocked.
 * If one is found, consume it and return immediately.
 * Otherwise, block on a Mach semaphore until pthread_kill() wakes us.
 */
int
sigwait(const sigset_t *set, int *sig)
{
	pthread_t self = pthread_self();
	kern_return_t kern_res;
	mach_port_t sem;
	unsigned long deliverable;
	int i;

	/* Create the sigwait semaphore on first use */
	LOCK(self->lock);
	if (self->sig_sem == MACH_PORT_NULL)
	{
		MACH_CALL(semaphore_create(mach_task_self(),
					   &self->sig_sem,
					   SYNC_POLICY_FIFO, 0), kern_res);
		if (kern_res != KERN_SUCCESS)
		{
			UNLOCK(self->lock);
			return (ENOMEM);
		}
	}
	sem = self->sig_sem;
	UNLOCK(self->lock);

	for (;;)
	{
		LOCK(self->lock);
		deliverable = self->sigpending & *set;
		if (deliverable)
		{
			/* Find lowest-numbered pending signal in the set */
			for (i = 1; i < _NSIG; i++)
			{
				if (deliverable & (1UL << (i - 1)))
				{
					self->sigpending &= ~(1UL << (i - 1));
					UNLOCK(self->lock);
					*sig = i;
					return (ESUCCESS);
				}
			}
		}
		UNLOCK(self->lock);

		/* Block until pthread_kill() posts our semaphore */
		MACH_CALL(semaphore_wait(sem), kern_res);
		/* Loop back to check pending signals again */
	}
}
