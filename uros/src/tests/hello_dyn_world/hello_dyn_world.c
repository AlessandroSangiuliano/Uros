/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * hello_dyn_world.c — the dynamic twin of hello_world (#289).
 *
 * Same body as hello_world.c (printf + return 0), but linked as an
 * ET_DYN/PIE with PT_INTERP=/lib/ld-musl-i386.so.1 and DT_NEEDED libc.so,
 * so it runs through ld-musl + the umbrella libc.so instead of the static
 * crt0 path.  This exercises the *dynamic* clean-exit path end to end:
 *
 *   __libc_start_main -> main -> return -> musl exit() (atexit +
 *   __stdio_exit flush) -> exit_group syscall -> libposix-uros h_exit ->
 *   task_terminate -> ush reaps via waitpid.
 *
 * hello_dyn (the #234 bring-up prover) loops forever and uses mach_print;
 * this one is the real-Unix-program shape: plain stdout, returns, exits.
 * Because it links the umbrella libc.so (real __uros_syscall* via
 * --whole-archive), it needs none of the static -Wl,-u workaround (#288).
 */

#include <stdio.h>

int
main(void)
{
    printf("hello\n");
    return 0;
}
