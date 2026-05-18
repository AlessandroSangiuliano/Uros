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
 * MIG reply port support for libpthreads.
 *
 * MIG-generated stubs call mig_get_reply_port() to obtain a per-thread
 * reply port.  This implementation stores the port in pthread TSD,
 * replacing the cthreads version that used cthread->reply_port.
 *
 * The three entry points (mig_get_reply_port, mig_dealloc_reply_port,
 * mig_put_reply_port) have the same signatures as the cthreads version
 * so that MIG stubs work unchanged.
 */

#include "pthread_internals.h"
#include <mach/mach_traps.h>

/*
 * We use a dedicated TSD key for the reply port.
 * Initialized once by mig_init().
 */
static pthread_key_t _mig_reply_port_key;
static int _mig_key_initialized;

/* Global reply port used before threading is initialized */
static mach_port_t _mig_global_reply_port = MACH_PORT_NULL;

/*
 * Non-static so that parent tasks can vm_write it to 0 in child tasks
 * created with task_create(inherit=TRUE).  Without this, child tasks
 * inherit _mig_multithreaded=1 but have no pthread runtime, causing
 * pthread_self() to return garbage and crash in mig_get_reply_port.
 */
int _mig_multithreaded;

static void
_mig_reply_port_destructor(void *val)
{
	mach_port_t port = (mach_port_t)(unsigned long)val;
	if (port != MACH_PORT_NULL)
		mach_port_mod_refs(mach_task_self(), port,
				   MACH_PORT_RIGHT_RECEIVE, -1);
}

/*
 * Called by mach_init (via crt0) with NULL before threading starts,
 * and called again after pthread_init with non-NULL.
 *
 * The parameter type is void* to match the cthreads signature
 * (cthread_t was a pointer).
 */
void
mig_init(void *initial)
{
	if (initial == (void *)0) {
		_mig_multithreaded = 0;
		_mig_global_reply_port = MACH_PORT_NULL;
	} else {
		_mig_multithreaded = 1;
		if (!_mig_key_initialized) {
			pthread_key_create(&_mig_reply_port_key,
					   _mig_reply_port_destructor);
			_mig_key_initialized = 1;
		}
		/* Recycle the global reply port into this thread's TSD */
		if (_mig_global_reply_port != MACH_PORT_NULL) {
			pthread_setspecific(_mig_reply_port_key,
					   (void *)(unsigned long)_mig_global_reply_port);
			_mig_global_reply_port = MACH_PORT_NULL;
		}
	}
}

/*
 * Called by MIG stubs whenever a reply port is needed.
 */
mach_port_t
mig_get_reply_port(void)
{
	mach_port_t port;

	if (_mig_multithreaded) {
		port = (mach_port_t)(unsigned long)
			pthread_getspecific(_mig_reply_port_key);
		if (port == MACH_PORT_NULL) {
			port = mach_reply_port();
			pthread_setspecific(_mig_reply_port_key,
					   (void *)(unsigned long)port);
		}
	} else {
		port = _mig_global_reply_port;
		if (port == MACH_PORT_NULL)
			_mig_global_reply_port = port = mach_reply_port();
	}
	return port;
}

/*
 * Called by MIG stubs after a timeout on the reply port.
 */
void
mig_dealloc_reply_port(mach_port_t port)
{
	mach_port_t reply_port;

	if (_mig_multithreaded) {
		reply_port = (mach_port_t)(unsigned long)
			pthread_getspecific(_mig_reply_port_key);
		pthread_setspecific(_mig_reply_port_key, (void *)0);
	} else {
		reply_port = _mig_global_reply_port;
		_mig_global_reply_port = MACH_PORT_NULL;
	}

	if (reply_port != MACH_PORT_NULL)
		mach_port_mod_refs(mach_task_self(), reply_port,
				   MACH_PORT_RIGHT_RECEIVE, -1);
}

/*
 * Called by MIG stubs after each successful RPC (no-op).
 */
void
mig_put_reply_port(mach_port_t port)
{
}
