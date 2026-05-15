/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * hello_exec.c — minimal test binary for exec_server v0.1.0 (#228).
 *
 * Loaded by exec_server, runs in a fresh task with empty IPC space.
 * Cannot use Mach IPC (no ports), only kernel traps:
 *   - mach_print  (trap 14, prints to kernel console)
 *   - mach_null   (trap 15, no-op for cycle counting)
 *
 * Strategy: print a recognisable banner via mach_print, then loop
 * forever.  exec_server's caller (ipc_bench) tears the task down
 * after observing the output / a timeout.
 *
 * No libc, no libmach, no libpthreads — bare assembly stubs for the
 * two traps we need.
 */

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Mach kernel-trap stubs                                              */
/* ------------------------------------------------------------------ */

/*
 * Mach uses the SYSENTER instruction (0x0F 0x34) for fast kernel
 * traps.  Convention on i386:
 *   eax = -trap_number
 *   ecx = stack pointer (for the kernel to find caller-saved state)
 *   edx = return address (for SYSEXIT)
 *
 * Trap 14 = mach_print (1 arg = const char *, on the user stack).
 * Trap 15 = mach_null (0 args).
 */

static __attribute__((naked, noinline)) void
mach_print(const char *s)
{
    (void)s;
    __asm__ volatile (
        "movl $-14, %%eax\n\t"
        "movl %%esp, %%ecx\n\t"
        "call 1f\n"
        "1:\n\t"
        "popl %%edx\n\t"
        "addl $(2f - 1b), %%edx\n\t"
        ".byte 0x0f, 0x34\n"
        "2:\n\t"
        "ret\n"
        ::: "eax", "ecx", "edx", "memory"
    );
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                         */
/* ------------------------------------------------------------------ */

/*
 * The System V ABI hands argc/argv/envp on the stack.  We don't
 * inspect them in v0.1.0 — exec_server has set them up correctly,
 * proving the stack-layout code works is enough.
 */

void
__attribute__((noreturn, used))
_start(void)
{
    mach_print("hello_exec: hello from exec_server v0.1.0\n");

    /* Loop forever.  Without Mach IPC we cannot cleanly self-
     * terminate; the parent task tears us down via task_terminate
     * after a brief wait. */
    for (;;) {
        __asm__ volatile (
            "movl $-15, %%eax\n\t"
            "movl %%esp, %%ecx\n\t"
            "call 1f\n"
            "1:\n\t"
            "popl %%edx\n\t"
            "addl $(2f - 1b), %%edx\n\t"
            ".byte 0x0f, 0x34\n"
            "2:\n\t"
            ::: "eax", "ecx", "edx"
        );
    }
}
