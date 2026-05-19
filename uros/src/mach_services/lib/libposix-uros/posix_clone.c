/*
 * libposix-uros — Mach-native pthread spawn (#256 / Phase 6a).
 *
 * Replaces the Linux clone(2) path for musl's pthread_create.  All
 * pthreads live as Mach threads inside the calling task (1:1
 * user/kernel mapping — Uros's stated threading model) and share VM,
 * file descriptors, signal handlers and TLS-storage scheme by virtue
 * of being in the same Mach task.  The CLONE_VM/FS/FILES/SIGHAND/...
 * flags musl passes are honoured implicitly.
 *
 * Recipe:
 *   1. thread_create(self) — suspended kernel-side thread.
 *   2. install_thread_tls (posix_tls.c) for CLONE_SETTLS — LDT
 *      descriptor at slot 4, selector 0x27 = TLS for the new thread.
 *   3. thread_set_state with eip=__uros_clone_trampoline,
 *      uesp=stack top with arg already pushed, ebp=fn, gs=selector.
 *   4. thread_resume.
 *   5. Parent returns the thread port name as the new tid; musl
 *      writes it into the pthread struct.
 *
 * Out of scope (Phase 6b/c):
 *   - CLONE_CHILD_CLEARTID + futex wake for pthread_join (busy-wait
 *     futex stub in handlers.c is enough for now)
 *   - CLONE_PARENT_SETTID for the rare path where caller wants the
 *     tid synchronously (we still write *ptid, but musl's join logic
 *     doesn't depend on it for the common case)
 *   - pthread_atfork hooks called from __uros_fork
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <stdint.h>
#include <stddef.h>

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/mach_interface.h>
#include <mach/thread_status.h>

/* From posix_tls.c — install LDT for a target thread, returns the
 * selector to load into %gs (0 on failure). */
struct __uros_user_desc;
extern int __uros_install_thread_tls(mach_port_t target_thread,
                                     struct __uros_user_desc *desc);

extern void __uros_clone_trampoline(void);

/* Linux clone(2) flag bits that musl pthread_create cares about. */
#define UROS_CLONE_SETTLS         0x00080000

int
__uros_clone(int (*fn)(void *),
             void *stack,
             int   flags,
             void *arg,
             int  *ptid,
             void *newtls,
             int  *ctid)
{
    kern_return_t kr;

    if (!fn || !stack)
        return -22; /* -EINVAL */

    /* Lay out the new thread's initial stack: 16-byte align then push
     * fn's argument so the trampoline's `call *fn` finds it at the
     * standard cdecl-first-arg position (4(%esp) after the call). */
    uintptr_t sp = ((uintptr_t)stack) & ~0xFu;
    sp -= 4;
    *(uintptr_t *)sp = (uintptr_t)arg;

    /* 1. Create the kernel thread, suspended. */
    mach_port_t new_thread = MACH_PORT_NULL;
    kr = thread_create(mach_task_self(), &new_thread);
    if (kr != KERN_SUCCESS)
        return -11; /* -EAGAIN */

    /* 2. Install per-thread TLS if requested.  newtls points at the
     * caller's `struct user_desc`; install_tls fills entry_number. */
    int gs_sel = 0x1f;     /* USER_DS — sensible default */
    if ((flags & UROS_CLONE_SETTLS) && newtls) {
        int sel = __uros_install_thread_tls(new_thread,
                                            (struct __uros_user_desc *)newtls);
        if (sel)
            gs_sel = sel;
    }

    /* 3. Initial register state: trampoline at eip, our stack at uesp,
     * fn in ebp (trampoline reads it from there before zeroing ebp),
     * TLS selector in gs.  Other segments are the standard user-mode
     * values shared by every Uros user task. */
    struct i386_thread_state state = {0};
    state.eip  = (int)__uros_clone_trampoline;
    state.uesp = (int)sp;
    state.ebp  = (int)fn;
    state.cs   = 0x17;
    state.ds = state.es = state.ss = state.fs = 0x1f;
    state.gs   = gs_sel;
    state.efl  = 0x202;

    kr = thread_set_state(new_thread, i386_THREAD_STATE,
                          (thread_state_t)&state,
                          i386_THREAD_STATE_COUNT);
    if (kr != KERN_SUCCESS) {
        (void)thread_terminate(new_thread);
        return -11;
    }

    /* 4. Start it. */
    kr = thread_resume(new_thread);
    if (kr != KERN_SUCCESS) {
        (void)thread_terminate(new_thread);
        return -11;
    }

    /* 5. Use the Mach thread port name as the tid.  musl wants a
     * non-negative int; port names fit comfortably. */
    int tid = (int)new_thread;
    if (ptid) *ptid = tid;
    (void)ctid;     /* Phase 6b will honour CLONE_CHILD_CLEARTID */

    return tid;
}
