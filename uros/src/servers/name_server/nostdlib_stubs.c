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
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Stubs for symbols normally provided by the host C library.
 * Same pattern as bootstrap/nostdlib_stubs.c.
 */

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/message.h>

extern kern_return_t syscall_thread_switch(mach_port_t, int, mach_msg_timeout_t);

kern_return_t
thread_switch(mach_port_t thread, int option, mach_msg_timeout_t option_time)
{
	return syscall_thread_switch(thread, option, option_time);
}

/*
 * ⚠️ The instruction has to name the whole register.  `%esp' on x86-64 is the
 * low half of the stack pointer, and the assembler refuses the mismatch
 * rather than quietly returning an address with its top half missing -- which
 * is the good outcome, and the reason this is written twice instead of once
 * with a width nobody looks at.
 */
void *
cthread_sp(void)
{
	void *sp;
#if defined(__x86_64__)
	__asm__ __volatile__("movq %%rsp, %0" : "=r" (sp));
#elif defined(__i386__)
	__asm__ __volatile__("movl %%esp, %0" : "=r" (sp));
#else
#error "cthread_sp: no stack pointer for this architecture"
#endif
	return sp;
}

/*
 * ⚠️ size_t and not unsigned int.  These are the C library's own names, and
 * on x86-64 the caller's count is 64 bits wide: a parameter of the wrong
 * width takes the low half of it and copies a fraction of what was asked for.
 * Identical on i386, where the two types are the same size.
 */
void *
memset(void *s, int c, __SIZE_TYPE__ n)
{
	unsigned char *p = (unsigned char *)s;
	while (n--)
		*p++ = (unsigned char)c;
	return s;
}

void *
memcpy(void *dst, const void *src, __SIZE_TYPE__ n)
{
	unsigned char *d = (unsigned char *)dst;
	const unsigned char *s2 = (const unsigned char *)src;
	while (n--)
		*d++ = *s2++;
	return dst;
}
