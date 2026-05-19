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

extern void __uros_clone_trampoline(void);

#ifdef UROS_PTHREAD_SMOKE
extern void mach_print(const char *);
#endif

/* From posix_tls.c. */
extern int __uros_install_thread_tls_at(mach_port_t target_thread,
                                        unsigned int base_addr);

/* Linux switch_thread_switch options.  Matches kern/syscall_subr.h. */
#define UROS_SWITCH_OPTION_NONE 0
extern int    syscall_thread_switch(unsigned long, int, int);
extern void   _exit(int);

#ifdef UROS_PTHREAD_SMOKE
#define BOOT_TRACE(s) mach_print("posix-uros/clone: " s "\n")
#else
#define BOOT_TRACE(s) do { } while (0)
#endif

/*
 * Entry point for newly-created pthread Mach threads.  Called by the
 * trampoline with the cdecl frame __uros_clone laid out; runs on the
 * new thread itself so the LDT install + LDTR-activation yield happen
 * in the right context (same idiom __uros_main_tls_init uses for the
 * main thread).
 */
void
__uros_thread_bootstrap(int (*fn)(void *), void *arg, unsigned int tls_base)
{
    BOOT_TRACE("bootstrap enter");

    if (tls_base) {
        if (__uros_install_thread_tls_at(mach_thread_self(), tls_base)) {
            /* Yield so the scheduler picks our pcb back up and
             * reloads LDTR from the descriptor we just installed —
             * same dance __uros_main_tls_init does. */
            (void)syscall_thread_switch(0, UROS_SWITCH_OPTION_NONE, 0);
            __asm__ __volatile__("movw %w0, %%gs" :: "r"(0x27));
        }
    }
    BOOT_TRACE("bootstrap calling fn");

    int rc = fn(arg);
    _exit(rc);
    for (;;) { /* unreachable */ }
}

#define TRACE(s) BOOT_TRACE(s)

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

    TRACE("enter");

    if (!fn || !stack)
        return -22; /* -EINVAL */

    /*
     * Lay out the new thread's initial stack as a cdecl call frame for
     * __uros_thread_bootstrap(fn, arg, tls_base):
     *
     *   [high]
     *     tls_base    ← +12 from uesp
     *     arg         ← +8
     *     fn          ← +4
     *     <ret addr>  ← +0 (overwritten when bootstrap pushes a frame
     *                  for further calls; we never return through it)
     *   [low / uesp]
     *
     * The trampoline just `call __uros_thread_bootstrap` — no register
     * juggling needed.  TLS install happens on the new thread itself
     * (same mechanism as the main thread in __uros_main_tls_init),
     * which sidesteps the kernel's "i386_set_ldt for a non-current
     * thread" code path that doesn't activate LDTR until the next
     * context switch.
     */
    uintptr_t tls_base = (flags & UROS_CLONE_SETTLS)
                            ? (uintptr_t)newtls
                            : 0;

    uintptr_t sp = ((uintptr_t)stack) & ~0xFu;
    sp -= 16;
    ((uintptr_t *)sp)[0] = 0;                       /* fake return addr */
    ((uintptr_t *)sp)[1] = (uintptr_t)fn;
    ((uintptr_t *)sp)[2] = (uintptr_t)arg;
    ((uintptr_t *)sp)[3] = tls_base;

    /* 1. Create the kernel thread, suspended. */
    mach_port_t new_thread = MACH_PORT_NULL;
    kr = thread_create(mach_task_self(), &new_thread);
    if (kr != KERN_SUCCESS) {
        TRACE("thread_create failed");
        return -11; /* -EAGAIN */
    }
    TRACE("thread_create ok");

    /* 2. Initial register state: trampoline at eip, args on the stack
     * at uesp; segments use the safe USER_CS/USER_DS defaults — TLS
     * (%gs) is installed on the new thread itself via
     * __uros_thread_bootstrap before fn is called. */
    struct i386_thread_state state = {0};
    state.eip  = (int)__uros_clone_trampoline;
    state.uesp = (int)sp;
    state.cs   = 0x17;
    state.ds = state.es = state.ss = state.fs = 0x1f;
    state.gs   = 0x1f;
    state.efl  = 0x202;

    /*
     * Use i386_REGS_SEGS_STATE rather than i386_THREAD_STATE: the
     * THREAD_STATE flavor unconditionally forces ds/es/fs/gs to
     * USER_DS in pcb.c, throwing away the LDT selector we just put
     * in state.gs.  REGS_SEGS_STATE honours our segment values
     * (after validating cs/ss have user privilege), which is what
     * pthread_create needs to land in the child with %gs pointing
     * at its TLS.
     */
    kr = thread_set_state(new_thread, i386_THREAD_STATE,
                          (thread_state_t)&state,
                          i386_THREAD_STATE_COUNT);
    if (kr != KERN_SUCCESS) {
        TRACE("thread_set_state failed");
        (void)thread_terminate(new_thread);
        return -11;
    }
    TRACE("thread_set_state ok");

    /* 4. Start it. */
    kr = thread_resume(new_thread);
    if (kr != KERN_SUCCESS) {
        TRACE("thread_resume failed");
        (void)thread_terminate(new_thread);
        return -11;
    }
    TRACE("thread_resume ok");

    /* 5. Use the Mach thread port name as the tid.  musl wants a
     * non-negative int; port names fit comfortably. */
    int tid = (int)new_thread;
    if (ptid) *ptid = tid;
    (void)ctid;     /* Phase 6b will honour CLONE_CHILD_CLEARTID */

    return tid;
}
