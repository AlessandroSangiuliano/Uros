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
 * GCC stack-smashing protector support for freestanding Mach userspace.
 *
 * When compiling without -fno-stack-protector, GCC inserts canary checks
 * in function prologues/epilogues.  Two symbols must be provided:
 *
 *   __stack_chk_guard  — the canary value (compared at function exit)
 *   __stack_chk_fail() — called when the canary has been overwritten
 *
 * The canary is initialized early in __start_mach() (crt0.c) before
 * any stack-protected function runs.
 */

extern void panic(const char *, ...);

/*
 * Stack canary value.  The byte pattern 0x00 0x0a 0xff terminates
 * common string-based overflows (NUL, newline, 0xFF).  It is
 * overwritten at startup by init_stack_guard() with a value derived
 * from the initial stack pointer for minimal per-run randomization.
 */
unsigned long __stack_chk_guard = 0x00000aff;

/*
 * Called by GCC-generated epilogues when the canary has been
 * corrupted.  Must never return.
 */
void __attribute__((noreturn))
__stack_chk_fail(void)
{
	panic("stack smashing detected");
	__builtin_unreachable();
}

/*
 * Hidden-visibility variant emitted by GCC on i386 with -fPIC.
 */
void
__stack_chk_fail_local(void)
{
	__stack_chk_fail();
}

/*
 * Initialize the canary with entropy from the stack pointer.
 * Called from __start_mach() in crt0.c before _mach_init_routine.
 * In a freestanding environment without /dev/urandom the stack
 * address provides the best available per-run variation.
 */
void
init_stack_guard(void)
{
	unsigned long sp;
	__asm__ volatile("movl %%esp, %0" : "=r" (sp));
	__stack_chk_guard = sp ^ 0xDEAD00FF;
}
