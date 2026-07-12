/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
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
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
/*
 * MUTEX_OWNER_TRACK (#383) — single point of truth for the mutex owner
 * tracking switch, shared between <kern/lock.h> (mutex_t fields) and
 * i386/i386_lock.S (the assembler that maintains them).  The two sides
 * MUST agree, so neither defines its own default.
 *
 * 1 = record own_thr/own_pc/ilk_thr on every mutex operation (3-4 extra
 *     stores on the hottest IPC paths, +12 bytes per mutex).  Invaluable
 *     while hunting kernel hangs: a stuck mutex names its holder and the
 *     acquiring call site (found the #383 act-lock leak).
 * 0 = zero-overhead production/benchmark build.
 */
#ifndef	_KERN_MUTEX_TRACK_H_
#define	_KERN_MUTEX_TRACK_H_

#define	MUTEX_OWNER_TRACK	1

#endif	/* _KERN_MUTEX_TRACK_H_ */
