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
#include <mach/mach_syscalls.h>
#include <mach/mach_port.h>
#include <mach/mach_interface.h>
#include <mach/thread_status.h>

extern void __uros_clone_trampoline(void);

/* In musl (src/internal/uros_main_thread.c), which has the thread's structure.
 * Two 32-bit words there, and these two words here. */
extern struct mach_thread_state *__uros_mach_thread_state(void);
_Static_assert(sizeof(struct mach_thread_state) == 8,
               "uros_mach_state in musl's struct __pthread is two words");
extern struct mach_thread_state *mach_thread_state(void);

#ifdef UROS_PTHREAD_SMOKE
extern void mach_print(const char *);
#endif

#if defined(__i386__)
/* From i386/posix_tls.c. */
extern int __uros_install_thread_tls_at(mach_port_t target_thread,
                                        unsigned int base_addr);
#endif

/* Linux switch_thread_switch options.  Matches kern/syscall_subr.h. */
#define UROS_SWITCH_OPTION_NONE 0
extern void   _exit(int);

/* urmach_thread_set_cleartid() is the kernel half of CLONE_CHILD_CLEARTID
 * (#257): it registers a user address that the kernel zeroes when the
 * calling thread terminates, so a joiner polling on it observes the exit
 * even though our futex stub doesn't queue waiters.  Declared by
 * <mach/mach_syscalls.h>, with the kernel's own types (#414). */

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
__uros_thread_bootstrap(int (*fn)(void *), void *arg,
                        uintptr_t tls_base, uintptr_t ctid_addr)
{
    BOOT_TRACE("bootstrap enter");

    (void)tls_base;     /* The parent in __uros_clone has set this thread's
                         * TLS up (#258): by the time we run, %gs or the FS
                         * base already reaches it. */

    /* Register the ctid so the kernel zeroes it atomically on
     * thread_terminate — closes the join-side race that the Phase 6b
     * userspace patch in __pthread_exit had to paper over. */
    if (ctid_addr)
        (void)urmach_thread_set_cleartid((unsigned long)ctid_addr);

    BOOT_TRACE("bootstrap calling fn");

    int rc = fn(arg);
    _exit(rc);
    for (;;) { /* unreachable */ }
}

#define TRACE(s) BOOT_TRACE(s)

/*
 * A thread gives back its reply port on the way out, with a trap: the RPC would
 * ask for a reply port, which is the thing being given back (#414).
 */
void
__uros_thread_leaving(void)
{
    struct mach_thread_state *state = mach_thread_state();
    mach_port_t port = state->reply_port;

    state->reply_port = MACH_PORT_NULL;
    if (port != MACH_PORT_NULL)
        (void)syscall_mach_port_mod_refs(mach_task_self(), port,
                                         MACH_PORT_RIGHT_RECEIVE, -1);
}

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
     * __uros_thread_bootstrap(fn, arg, tls_base, ctid_addr):
     *
     *   [high]
     *     ctid_addr   ← +16 from uesp
     *     tls_base    ← +12
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

    uintptr_t sp = ((uintptr_t)stack) & ~(uintptr_t)0xF;
#if defined(__x86_64__)
    /*
     * The arguments travel in registers here, so the stack holds only the
     * return address a call would have left, which is also what puts the
     * stack pointer where the ABI wants it at a function's first instruction.
     */
    sp -= sizeof(uintptr_t);
    *(uintptr_t *)sp = 0;                           /* fake return addr */
#else
    sp -= 32;                                       /* 16-byte align */
    ((uintptr_t *)sp)[0] = 0;                       /* fake return addr */
    ((uintptr_t *)sp)[1] = (uintptr_t)fn;
    ((uintptr_t *)sp)[2] = (uintptr_t)arg;
    ((uintptr_t *)sp)[3] = 0;     /* tls_base — installed below by parent */
    ((uintptr_t *)sp)[4] = (uintptr_t)ctid;
#endif

    /*
     * Each thread its own reply port, from before there is a second one
     * (#414).  Two threads receiving on one port take each other's replies,
     * and the one whose reply was taken waits for good (#299).  Asked every
     * time, as pthread_create() asks in libpthreads: it returns at once when
     * it is done already, and a forked child goes back to the task's state
     * and hands over again here.
     */
    mig_init(__uros_mach_thread_state);

    /* 1. Create the kernel thread, suspended. */
    mach_port_t new_thread = MACH_PORT_NULL;
    /* #542: before, not after — see mach.h. */
    mach_note_thread_created();
    kr = thread_create(mach_task_self(), &new_thread);
    if (kr != KERN_SUCCESS) {
        TRACE("thread_create failed");
        return -11; /* -EAGAIN */
    }
    TRACE("thread_create ok");

#if defined(__x86_64__)
    /*
     * 2 and 3 are one step here: the FS base is a field of the state, so the
     * new thread has its TLS from its first instruction (#414).
     */
    struct x86_64_thread_state state = {0};
    state.rip     = (uintptr_t)__uros_clone_trampoline;
    state.rsp     = sp;
    state.rdi     = (uintptr_t)fn;
    state.rsi     = (uintptr_t)arg;
    state.rcx     = (uintptr_t)ctid;
    state.fs_base = tls_base;

    kr = thread_set_state(new_thread, x86_64_THREAD_STATE,
                          (thread_state_t)&state,
                          x86_64_THREAD_STATE_COUNT);
#else
    /*
     * 2. Install per-thread TLS BEFORE thread_resume.  This is the
     * #258 attempt: the kernel's act_machine_switch_pcb loads LDTR
     * from pcb->ims.ldt on every context switch, so by the time the
     * scheduler dispatches the new thread its LDT should already be
     * live and %gs = 0x27 should resolve.
     */
    int gs_sel = 0x1f;
    if (tls_base) {
        int sel = __uros_install_thread_tls_at(new_thread,
                                               (unsigned int)tls_base);
        if (sel)
            gs_sel = sel;
        TRACE("parent-side LDT install ok");
    }

    /* 3. Initial register state: trampoline at eip, args on the stack
     * at uesp; gs preloaded with the LDT selector. */
    struct i386_thread_state state = {0};
    state.eip  = (int)__uros_clone_trampoline;
    state.uesp = (int)sp;
    state.cs   = 0x17;
    state.ds = state.es = state.ss = state.fs = 0x1f;
    state.gs   = gs_sel;
    state.efl  = 0x202;

    /*
     * Use i386_REGS_SEGS_STATE: i386_THREAD_STATE unconditionally
     * forces gs = USER_DS in pcb.c:765, which would drop our LDT
     * selector.  REGS_SEGS_STATE honours state.gs after validating
     * cs/ss have user privilege.
     */
    kr = thread_set_state(new_thread, i386_REGS_SEGS_STATE,
                          (thread_state_t)&state,
                          i386_THREAD_STATE_COUNT);
#endif
    if (kr != KERN_SUCCESS) {
        TRACE("thread_set_state failed");
        (void)thread_terminate(new_thread);
        return -11;
    }
    TRACE("thread_set_state ok");

    /*
     * 4. Use the Mach thread port name as the tid.  musl wants a
     * non-negative int; port names fit comfortably.
     *
     * 🔴 BEFORE THE THREAD RUNS, as CLONE_PARENT_SETTID is (#733).  musl
     * reads self->tid to own a mutex and to take the thread list, and a
     * thread that got there first did both as nobody.  An owner-tracked
     * mutex taken as 0 and given back as itself answers EPERM and stays on
     * the robust list, and the next lock of it links it to itself.  The
     * thread then went round that list for ever on its way out, holding the
     * thread list, and the next fork() in the program stopped on that.
     */
    int tid = (int)new_thread;
    if (ptid) *ptid = tid;

    /* 5. Start it. */
    kr = thread_resume(new_thread);
    if (kr != KERN_SUCCESS) {
        TRACE("thread_resume failed");
        (void)thread_terminate(new_thread);
        return -11;
    }
    TRACE("thread_resume ok");

    /* ctid is passed through to the bootstrap on the new thread's
     * stack — it registers it with urmach_thread_set_cleartid so the
     * kernel clears *ctid on thread_terminate (#257). */

    return tid;
}

#if defined(__x86_64__)
/*
 * musl's __unmapself.s jumps here, from a detached thread that is about to
 * lose its stack (#414).  The two ports are read while there still is one, and
 * mach_task_self() is the cached name: the trap would add a user reference to
 * the task's port for every thread that ever ended this way.
 */
extern void __uros_unmap_and_exit(mach_port_t task, vm_offset_t base,
                                  vm_size_t size, mach_port_t thread)
    __attribute__((noreturn));

void
__uros_unmapself(void *base, size_t size)
{
    __uros_thread_leaving();
    __uros_unmap_and_exit(mach_task_self(), (vm_offset_t)base,
                          (vm_size_t)size, mach_thread_self());
}
#endif
