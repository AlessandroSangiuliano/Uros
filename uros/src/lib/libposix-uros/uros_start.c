/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros -- the Mach personality of a program of musl that does not
 * start through libmach's crt0 (#929).
 *
 * A program that starts through libmach's crt0 gets it there: mach_init(),
 * then __uros_libc_init().  One that starts through musl's crt1 reaches
 * __libc_start_main(), and musl knows nothing of Mach: a program linked
 * against libc.so, and one linked statically with a plain -lc, as a build
 * that is not ours links it.  This is what both need before their own code
 * runs:
 *
 *   mach_task_self_ seeded (mach_task_self_init(), the trap)  -- every Mach
 *     call names the task through it, and mach_init_ports() is one;
 *   the well-known ports looked up (mach_init_ports());
 *   the personality's tail, which needs those ports -- proc_server's identity
 *     (__uros_signals_init()) and the descriptor table an execve handed over
 *     (__uros_absorb_inherited_fds()).
 *
 * libc.so's constructor calls it (dyn_bootstrap.c), and musl's
 * __libc_start_main() calls it for every program that comes in through crt1;
 * in a dynamic program the second call finds it done.  The references are
 * strong: in a static link a weak one would not pull signals.c and
 * posix_exec_fds.c out of the archive, and the tail would be skipped.
 * mach_init() itself is not called -- libmach_core's is static, see
 * dyn_bootstrap.c for why.
 */

extern void mach_task_self_init(void);
extern void mach_init_ports(void);
extern void __uros_signals_init(void);
extern void __uros_absorb_inherited_fds(void);

void __uros_start_personality(void);

void
__uros_start_personality(void)
{
    static int done;

    /* Before main(), on the one thread there is: no lock to take. */
    if (done)
        return;
    done = 1;

    mach_task_self_init();
    mach_init_ports();
    __uros_signals_init();
    __uros_absorb_inherited_fds();
}
