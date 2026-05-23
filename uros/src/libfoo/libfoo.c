/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libfoo.c — toy shared object for the Uros dlopen end-to-end test
 * (#234 Phase 7 increment 4).
 *
 * Deliberately minimal: one exported function, no libc calls.  The
 * test target dlopens this .so via musl's dlfcn, resolves
 * foo_answer() via dlsym and calls it.  If the value comes back
 * correctly through the dynamic-linker path, the whole chain
 * (ld-musl mmap + relocations + symbol resolution) is validated.
 *
 * Building anything that needs libc here would be honest but it
 * would also test ld-musl's NEEDED resolution against the umbrella
 * libc.so, which is already covered by hello_dyn.  Keeping libfoo
 * libc-free isolates the dlopen-vs-dlsym path on its own.
 */

#include <stdint.h>

/*
 * The marker value the test checks for.  Picked at random to be
 * unmistakable in serial logs if the dlsym path returns the wrong
 * pointer or jumps somewhere bogus.
 */
#define FOO_ANSWER_MAGIC  0x46010003u   /* 'F' 01 00 03 — Foo, build 1.0.3 */

uint32_t
foo_answer(void)
{
    return FOO_ANSWER_MAGIC;
}
