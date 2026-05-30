/*
 * libposix-uros — POSIX fork() over Mach task_create (#255 / Phase 5b).
 *
 * The setjmp-meets-task_create recipe:
 *
 *   parent                                  child
 *   ------                                  -----
 *   capture state (eax = 0)
 *   task_create(self, inherit_memory=TRUE)
 *   extract+insert critical port rights
 *     into the child's IPC space
 *   proc_register(child) → new pid
 *   vm_write &__uros_my_pid := new_pid
 *   state.eax = FORK_MARKER
 *   thread_create_running(child, state) -->  resume at captured EIP,
 *   return new_pid                            with eax = FORK_MARKER,
 *                                             branch into child path,
 *                                             reset cached task-self
 *                                             & call post-fork init,
 *                                             return 0
 *
 * The CoW VM means the child sees the parent's globals, stack and
 * heap as they were at the moment of task_create.  Port name values
 * are also CoW'd, but port NAMES only refer to send/receive rights
 * in the IPC space of the task that owns them.  The parent's IPC
 * space is not cloned by task_create — the child starts with an
 * empty one — so a port name like 0x803 in the child's BSS would
 * refer to nothing unless we re-insert the right at that name.  Hence
 * the extract+insert dance for {bootstrap, name_server, proc, exec}.
 *
 * Out of scope for Phase 5b: vfork (musl maps it to fork; that's
 * fine), pthread_atfork hooks, FP/SSE state across fork, stdio
 * buffer reset in the child.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/mach_syscalls.h>
#include <mach/message.h>
#include <mach/thread_status.h>
#include <mach_init.h>

#include "proc.h"
#include "proc_types.h"
#include "internal.h"      /* __uros_port_t */

#define UROS_FORK_MARKER  0xCAFEBABEu

extern int  __uros_capture_state(struct i386_thread_state *state);

extern mach_port_t  __uros_proc_port;
extern mach_port_t  __uros_exec_port;     /* defined as static — fwd here via getter */
extern unsigned int __uros_my_pid;
extern __uros_port_t __cached_task_self;  /* in handlers.c */
extern void          __uros_post_fork_init(void);
extern void          mig_reset_after_fork(void);  /* libmach_core */

/* posix_exec.c keeps __uros_exec_port file-static for lookup caching.
 * Expose a thin getter+setter so fork can inherit + the child can
 * reset.  Implemented at the bottom of posix_exec.c via this header-
 * less weak symbol so we don't broaden the public ABI. */
mach_port_t __uros_get_exec_port(void);
void        __uros_set_exec_port(mach_port_t p);

/* posix_tls.c: install the per-thread LDT TLS descriptor for the
 * calling thread with base = tp, and load %gs.  Returns 0 on success. */
extern int  __uros_set_thread_area_tp(void *tp);

/*
 * The calling thread's TLS pointer (%gs:0), captured by the parent just
 * before task_create and therefore CoW-visible to the child.  The
 * child's main thread comes up with %gs forced to USER_DS by
 * thread_create_running (i386_THREAD_STATE), so it must reinstall its
 * LDT TLS descriptor before any libc (stdio locks, errno, the SSP
 * canary) runs — see #259.  CoW means the child's TCB lives at the same
 * virtual address, so the parent's TP value is exactly what the child
 * reinstalls.
 */
static unsigned int __uros_fork_saved_tp;

/* ------------------------------------------------------------------ */
/* Port inheritance                                                    */
/* ------------------------------------------------------------------ */

/*
 * Copy a single send right from our IPC space to the child's, keeping
 * the same port NAME on both sides so the child's CoW'd globals stay
 * valid.  Best-effort: failure on a specific port leaves the child
 * without that capability, which may degrade gracefully (e.g. no
 * exec_server lookup → execve returns -ENOSYS).
 */
static void
inherit_send_right(mach_port_t parent, mach_port_t child, mach_port_t name)
{
    if (name == MACH_PORT_NULL)
        return;
    mach_port_t   right = MACH_PORT_NULL;
    mach_msg_type_name_t acquired = 0;
    kern_return_t kr;

    kr = mach_port_extract_right(parent, name,
                                 MACH_MSG_TYPE_COPY_SEND,
                                 &right, &acquired);
    if (kr != KERN_SUCCESS)
        return;

    /*
     * Insert into the child under the same name.  If a right already
     * lives at that name in the child (shouldn't, but defensive), we
     * silently swallow KERN_NAME_EXISTS — same port, same value.
     */
    (void)mach_port_insert_right(child, name, right, acquired);
}

/* ------------------------------------------------------------------ */
/* fork() entry point                                                  */
/* ------------------------------------------------------------------ */

extern mach_port_t bootstrap_port;        /* libmach_core */

int
__uros_fork(void)
{
    struct i386_thread_state state;
    kern_return_t kr;
    int magic = __uros_capture_state(&state);

    /* ===== Child path ============================================ */
    if (magic == (int)UROS_FORK_MARKER) {
        /*
         * We are the freshly-created task, resumed at the captured
         * EIP with state.eax holding FORK_MARKER.  Everything below
         * is the post-fork cleanup the child does for itself.
         *
         * 1. The cached task-self port name is the parent's — drop it
         *    so the next IPC call refetches via the trap (which is
         *    per-task and Just Works).
         * 2. Signal_port / exception_port the parent owned refer to
         *    nothing in our brand-new IPC space.  Wipe them and let
         *    __uros_post_fork_init re-allocate + re-register.
         */
        __cached_task_self = 0;
        /* Reset the MIG caches (reply port + task-self) BEFORE anything
         * that issues an RPC: the parent's mig_reply_port is meaningless
         * in our fresh IPC space.  __uros_set_thread_area_tp installs the
         * LDT via the i386_set_ldt MIG RPC, so the reset must precede it.
         * __uros_post_fork_init re-runs this (idempotent). */
        mig_reset_after_fork();
        /* Reinstall our TLS: thread_create_running forced %gs to USER_DS,
         * so the canary, errno and stdio locks are unusable until the LDT
         * descriptor is back.  The TP was captured by the parent below
         * and CoW'd into our address space. */
        (void)__uros_set_thread_area_tp((void *)__uros_fork_saved_tp);
        /*
         * #292: do NOT run __uros_post_fork_init() here.  It spawns the
         * signal handler pthread, which must happen AFTER musl's __post_Fork
         * resets the thread list — so it is registered as a pthread_atfork
         * child handler (see __uros_signals_init) and fires from fork()'s
         * __fork_handler() once we return.
         */
        return 0;
    }

    /* ===== Parent path =========================================== */

    /*
     * 0. Capture the calling thread's TLS pointer (%gs:0) before
     *    task_create snapshots the VM, so the child can reinstall its
     *    LDT descriptor (the child resumes with %gs = USER_DS).
     */
    __asm__ __volatile__("movl %%gs:0, %0" : "=r"(__uros_fork_saved_tp));

    /*
     * 1. task_create with inherit_memory=TRUE.  The kernel makes a
     *    CoW copy of our entire VM map (including stack, heap, .data,
     *    .bss, all the musl + libposix-uros state).  inherit_memory
     *    is the only knob we have on the trap; the IPC space is NOT
     *    cloned, so we explicitly seed the few send rights the child
     *    needs to keep working.
     */
    mach_port_t  child_task = MACH_PORT_NULL;
    kr = syscall_task_create(mach_task_self(),
                             0 /* ledgers */, 0 /* num_ledgers */,
                             TRUE /* inherit_memory */,
                             &child_task);
    if (kr != KERN_SUCCESS)
        return -EAGAIN;

    /*
     * 2. Inherit the send rights that musl-linked code reaches for.
     *    Reusing the same NAME on both sides means the child's CoW'd
     *    globals (which still hold the parent's port-name values)
     *    keep working without any vm_write.
     */
    inherit_send_right(mach_task_self(), child_task, bootstrap_port);
    inherit_send_right(mach_task_self(), child_task, name_server_port);
    inherit_send_right(mach_task_self(), child_task, __uros_proc_port);
    inherit_send_right(mach_task_self(), child_task, __uros_get_exec_port());

    /*
     * 2b. Inherit the fs_server send rights behind every open libvfs fd
     *     (#262 step 2).  The child's CoW'd vfs fd table holds these
     *     port NAMES but they resolve to nothing until we re-insert the
     *     rights under the same names.  After this the child can read/
     *     write its inherited fds.  Note: parent and child share the
     *     fs_server-side open handle (only the client offset is CoW'd
     *     and independent); a per-task dup notion is a later refinement.
     */
    {
        extern int vfs_enum_open_ports(mach_port_t *ports, int max);
        mach_port_t fd_ports[64];
        int np = vfs_enum_open_ports(fd_ports, 64);
        for (int i = 0; i < np; i++)
            inherit_send_right(mach_task_self(), child_task, fd_ports[i]);
    }

    /*
     * 3. Register the child with proc_server.  The new pid is the
     *    return value the parent hands back; we also write it into
     *    the child's BSS at &__uros_my_pid so the child's signal
     *    re-init knows who it is.
     */
    proc_pid_t  new_pid = 0;
    int         prc     = PROC_ERR_INVAL;
    if (__uros_proc_port != MACH_PORT_NULL) {
        char cmd[128] = "forked";
        (void)proc_register(__uros_proc_port, __uros_my_pid,
                            child_task, cmd, &new_pid, &prc);
    }

    if (new_pid != 0) {
        proc_pid_t pid_copy = new_pid;
        (void)vm_write(child_task, (vm_address_t)&__uros_my_pid,
                       (vm_offset_t)&pid_copy, sizeof pid_copy);
    }

    /*
     * 4. Hand the child our captured state, EAX bumped to FORK_MARKER
     *    so it picks up at the child branch above.  thread_create_running
     *    both creates the thread and starts it atomically.
     */
    state.eax = UROS_FORK_MARKER;

    mach_port_t child_thread = MACH_PORT_NULL;
    kr = thread_create_running(child_task,
                               i386_THREAD_STATE,
                               (thread_state_t)&state,
                               i386_THREAD_STATE_COUNT,
                               &child_thread);
    if (kr != KERN_SUCCESS) {
        (void)task_terminate(child_task);
        (void)mach_port_deallocate(mach_task_self(), child_task);
        return -EAGAIN;
    }

    /* Drop the parent's send rights to the child's task/thread — the
     * child owns its own, and proc_server keeps a separate copy for
     * task_terminate during the SIGCHLD path. */
    (void)mach_port_deallocate(mach_task_self(), child_thread);
    (void)mach_port_deallocate(mach_task_self(), child_task);

    return (int)new_pid;
}
