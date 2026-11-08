/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * libposix-uros -- thread-local storage on x86-64 (#414).
 *
 * The same contract as i386/posix_tls.c by another mechanism, and a much
 * shorter one: there is no descriptor to build and no selector to load, only a
 * base in a machine register.  The kernel loads it when a thread comes onto a
 * processor.
 *
 * musl's __set_thread_area.s jumps here from __init_tp, for the main thread.
 * A new pthread gets its base in the state __uros_clone gives it, and a forked
 * child in the state __uros_fork gives it, so neither passes through here.
 */

#include <mach/kern_return.h>
#include <mach/vm_types.h>
#include <mach/mach_syscalls.h>

/* 0, or -1 for __init_tp, which treats a failure as fatal. */
int
__uros_set_thread_area_tp(void *tp)
{
    if (urmach_thread_set_fs_base((vm_offset_t)tp) != KERN_SUCCESS)
        return -1;
    return 0;
}
