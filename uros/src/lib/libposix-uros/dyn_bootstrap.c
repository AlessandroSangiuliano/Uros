/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * dyn_bootstrap.c — wire mach_init() into the dynamic-task startup
 * (#234 Phase 7 increment 4).
 *
 * Background
 * ----------
 * Static Uros binaries call mach_init() explicitly from their startup
 * paths.  That populates the well-known port globals (bootstrap_port,
 * name_server_port, environment_port, service_port, mach_cap_port) by
 * combining task_get_special_port(TASK_BOOTSTRAP_PORT) with
 * mach_ports_lookup(task_self).
 *
 * A dynamic task linked against the umbrella libc.so runs musl's
 * __libc_start_main → __libc_start_init → __init_array → main; musl
 * does not know about Mach and never calls mach_init().  Result: every
 * Mach-flavoured library call inside the dynamic task saw
 * MACH_PORT_NULL for the well-known ports and failed — surfaced by
 * vfs_open() at the first dlopen() in the #234 incr 4 QEMU run
 * (2026-05-23).
 *
 * Fix
 * ---
 * This source file is part of libposix-uros-pic only.  When that
 * archive is folded into libc.so via --whole-archive, the constructor
 * below ends up in libc.so's .init_array and runs at libc startup,
 * before main(), before any user code can touch a Mach port.  Static
 * binaries link the non-pic libposix-uros (without this file) and
 * keep their explicit mach_init() call path.
 *
 * Why not just call mach_init()?
 * ------------------------------
 * libmach ships two competing mach_init implementations in distinct
 * TUs (mach_init.c and mach_init_sa.c, tracked by
 * [[project_unify_libmach]]).  Naming mach_init from outside libmach
 * pulls both TUs into the umbrella link and explodes on multiple
 * definitions of mach_task_self_/vm_page_size/NDR_record/etc.  We
 * sidestep by invoking the primitives directly — each lives in its
 * own unambiguous TU (mig_support.c, mach_init_ports.c) — and by
 * seeding mach_task_self_ via the real SYSENTER trap (the
 * <mach_init.h> macro shadows the function name everywhere it's
 * included, so we re-export the trap under its own asm name).
 *
 * The Uros personality tail (#375)
 * --------------------------------
 * Seeding the ports is only half of what a static task gets before
 * main().  Static binaries run crt0's __uros_libc_init(), whose first
 * half brings up musl TLS + the stack canary and whose second half
 * wires the Uros POSIX personality: __uros_signals_init() (which
 * proc_register()s us — so __uros_my_pid / __uros_proc_port become
 * valid and printf can find the session's controlling tty instead of
 * falling back to the raw mach_print console) and
 * __uros_absorb_inherited_fds() (which reclaims any fd table execve
 * handed over).
 *
 * A dynamic task does NOT need the first half — musl's real
 * __libc_start_main already ran __init_tls + __init_ssp from the true
 * SysV auxv before .init_array, so this constructor executes with TLS
 * and the canary already live.  Calling __uros_libc_init() wholesale
 * would redo (and disturb) that.  So we run only its personality tail
 * here, giving dynamic tools the same proc identity + ctty routing as
 * static ones.  Both symbols are weak: a smoke build that folds this
 * TU without the rest of libposix-uros still links.
 */

extern void mig_init(int);
extern void mach_init_ports(void);
extern void __uros_signals_init(void) __attribute__((weak));
extern void __uros_absorb_inherited_fds(void) __attribute__((weak));

/* mach_task_self lives in libmach as a SYSENTER stub (trap 28) but the
 * <mach_init.h> macro `#define mach_task_self() mach_task_self_`
 * shadows it everywhere it's #included.  Declare it under a distinct
 * name so this TU can invoke the real trap without fighting the macro. */
extern unsigned int mach_task_self(void) __asm__("mach_task_self");
extern unsigned int mach_task_self_;

__attribute__((constructor))
static void
__uros_dyn_bootstrap(void)
{
    /* mach_init_ports() reads mach_task_self_ via the `mach_task_self()`
     * macro; if we don't seed it the trap arg is 0 and every
     * special-port lookup fails.  Static binaries seed it inside
     * mach_init() after `#undef mach_task_self`; we do the equivalent
     * here. */
    mach_task_self_ = mach_task_self();
    mig_init(0);
    mach_init_ports();

    /* Personality tail — needs the well-known ports seeded just above. */
    if (__uros_signals_init)
        __uros_signals_init();
    if (__uros_absorb_inherited_fds)
        __uros_absorb_inherited_fds();
}
