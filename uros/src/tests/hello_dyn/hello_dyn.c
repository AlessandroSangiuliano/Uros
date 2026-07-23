/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * hello_dyn.c — first DYNAMICALLY-linked test binary for Uros (#234, Phase 7).
 *
 * Unlike hello_exec (static ET_EXEC), this is an ET_DYN/PIE image with a
 * PT_INTERP of /lib/ld-musl-i386.so.1 and a DT_NEEDED on libc.so.  Running
 * it exercises the whole dynamic path:
 *
 *   exec_server maps the program + the interpreter (the umbrella libc.so),
 *   sets AT_BASE, and jumps to the interpreter's _dlstart;
 *   ld-musl self-relocates, relocates this program, recognises that its
 *   only DT_NEEDED (libc.so) IS itself, then runs musl's __libc_start_main —
 *   which initialises the TLS via __uros_set_thread_area_tp (a Mach trap)
 *   and calls main() below.
 *
 * If "hello_dyn: reached main via ld-musl" appears on the kernel console,
 * the dynamic linker bootstrap + libc startup + the in-libc __uros_syscall
 * backend all work.  We announce via the mach_print trap (always available,
 * no fds required) rather than write(2), so the proof does not depend on a
 * wired-up stdout.  We also touch a pure-libc routine (strlen) first to
 * confirm the program's own relocations against libc.so resolved.
 */

#include <string.h>

/* mach_print kernel trap (SYSENTER, PC-relative resume — PIC-safe), same
 * stub hello_exec uses.  Independent of libc, works in a bare task. */
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

int
main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    /* A pure-libc call (no syscall): if its PLT/GOT entry resolved, the
     * ldso relocated us against libc.so correctly. */
    const char *msg = "hello_dyn: reached main via ld-musl\n";
    if (strlen(msg) == 36)
        mach_print("hello_dyn: libc relocations OK (strlen)\n");

    mach_print(msg);

    /* Loop; the parent task_terminates us (no _exit path wired for the
     * very first dynamic bring-up). */
    for (;;) { }
    return 0;
}
