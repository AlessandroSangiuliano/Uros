/*
 * libposix-uros — POSIX signal personality (#252 / Phase 4).
 *
 * proc_server (v0.2.0, #238) owns the kernel-side bookkeeping: a
 * registry of (pid → signal_port) send rights and the proc_kill
 * dispatcher that fans out uncatchable signals to task_terminate /
 * task_suspend / task_resume and catchable signals to a mach_msg on
 * the registered signal_port.  Everything POSIX-visible — the
 * sigaction table, sigprocmask state, the SA_HANDLER invocation — is
 * the client's responsibility, and this is that client.
 *
 * Flow at task startup (__uros_signals_init, called from
 * __uros_libc_init):
 *
 *     1. Look up the "proc_server" port via netname (with retry —
 *        stage-1 servers race against proc_server starting up).
 *     2. proc_register(PROC_PID_NONE, mach_task_self(), name) to
 *        obtain our pid.
 *     3. Allocate a Mach receive right (signal_port).
 *     4. Insert a MAKE_SEND right and hand it to proc_server via
 *        proc_set_signal_port — once registered, every proc_kill
 *        targeting us posts a proc_signal_msg_t to this port.
 *     5. vm_allocate a 16 KB stack and thread_create_running into
 *        __uros_sig_thread_entry (asm), which calls __uros_sig_loop.
 *
 * Flow on every catchable signal:
 *
 *     signal handler thread loops on mach_msg(signal_port),
 *     validates signo, looks up sa_handler, and calls it.  SIG_DFL
 *     turns into the POSIX default action (terminate/ignore/stop —
 *     the table at the bottom of this file).  SIG_IGN drops on the
 *     floor.
 *
 * Out of scope (Phase 4b+):
 *   - Hardware traps via Mach exception ports (SIGSEGV/SIGFPE/SIGBUS)
 *   - SA_RESTART on syscall interruption
 *   - sigsuspend / sigwait / sigtimedwait / sigqueue
 *   - Pending-queue replay on sigprocmask UNBLOCK
 *   - Per-thread masks (pthread_sigmask — needs real pthread, Phase 6)
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mach_interface.h>
#include <mach/thread_status.h>
#include <mach/thread_switch.h>
#include <mach/mach_syscalls.h>          /* syscall_thread_switch */
#include <mach_init.h>
#include <servers/netname.h>

#include "proc_types.h"
#include "proc.h"

#include "uros/libposix.h"

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

#define UROS_SIG_STACK_SIZE  (16 * 1024)

/*
 * Per-process sigaction table.  Indices are PROC_SIG* numbers (Linux
 * i386 layout — matches what musl-side sigaction passes in).
 */
static struct sigaction  __sig_actions[PROC_NSIG];
static sigset_t          __sig_mask;
static int               __sig_initialised;

/* proc_server handle + our own pid + signal_port we own. */
mach_port_t              __uros_proc_port    = MACH_PORT_NULL;
proc_pid_t               __uros_my_pid       = 0;
static mach_port_t       __sig_port          = MACH_PORT_NULL;

/* Forward decl of the asm entry that thread_create_running jumps to. */
extern void __uros_sig_thread_entry(void);

/* ------------------------------------------------------------------ */
/* POSIX default actions                                              */
/* ------------------------------------------------------------------ */

/* What SIG_DFL does for each signo: terminate (T), ignore (I), stop (S). */
static const char __sig_default[PROC_NSIG] = {
    [PROC_SIGHUP]  = 'T', [PROC_SIGINT]  = 'T', [PROC_SIGQUIT] = 'T',
    [PROC_SIGILL]  = 'T', [PROC_SIGTRAP] = 'T', [PROC_SIGABRT] = 'T',
    [PROC_SIGBUS]  = 'T', [PROC_SIGFPE]  = 'T', [PROC_SIGKILL] = 'T',
    [PROC_SIGUSR1] = 'T', [PROC_SIGSEGV] = 'T', [PROC_SIGUSR2] = 'T',
    [PROC_SIGPIPE] = 'T', [PROC_SIGALRM] = 'T', [PROC_SIGTERM] = 'T',
    [PROC_SIGCHLD] = 'I', [PROC_SIGCONT] = 'I',
    [PROC_SIGSTOP] = 'S', [PROC_SIGTSTP] = 'S',
};

/* ------------------------------------------------------------------ */
/* netname retry — proc_server may not yet be visible on first probe. */
/* ------------------------------------------------------------------ */

static kern_return_t
lookup_proc_server(mach_port_t *out)
{
    mach_port_t name_port = name_server_port;
    kern_return_t kr = KERN_FAILURE;
    int spins;

    /* MIG name_t is a fixed-size char[80] in/out string buffer; pass
     * properly sized local arrays to satisfy the (strict) overflow
     * checks gcc emits at -Wstringop-overflow. */
    char host[80] = "";
    char serv[80] = "proc_server";

    if (name_port == MACH_PORT_NULL)
        return KERN_INVALID_ARGUMENT;

    for (spins = 0; spins < 200; spins++) {
        kr = netname_look_up(name_port, host, serv, out);
        if (kr == NETNAME_SUCCESS)
            return KERN_SUCCESS;
        (void)syscall_thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);
    }
    return kr;
}

/* ------------------------------------------------------------------ */
/* Handler thread bottom — invoked by sig_thread_entry.S              */
/* ------------------------------------------------------------------ */

void
__uros_sig_loop(void)
{
    struct {
        proc_signal_msg_t  msg;
        char               pad[64];     /* room for full trailer */
    } buf;

    for (;;) {
        kern_return_t kr = mach_msg(&buf.msg.head,
                                    MACH_RCV_MSG,
                                    0,                  /* send size */
                                    sizeof(buf),        /* recv max */
                                    __sig_port,
                                    MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
            continue;
        if (buf.msg.head.msgh_id != PROC_SIGNAL_MSGID)
            continue;

        int signo = buf.msg.signo;
        if (signo <= 0 || signo >= PROC_NSIG)
            continue;

        void (*h)(int) = __sig_actions[signo].sa_handler;
        if (h == SIG_IGN)
            continue;
        if (h == SIG_DFL) {
            switch (__sig_default[signo]) {
            case 'I':   /* ignore */
                continue;
            case 'T':   /* terminate */
                __uros_syscall1(252 /* SYS_exit_group */, 128 + signo);
                continue;     /* unreachable */
            case 'S':   /* stop — Phase 4b will handle properly */
            default:
                continue;
            }
        }

        /* SA_SIGINFO is best-effort in Phase 4 — we don't fill siginfo_t. */
        h(signo);
    }
}

/*
 * Called by the asm stub if __uros_sig_loop ever returns (it
 * shouldn't).  Politely terminate the handler thread without taking
 * the whole task down.
 */
void
__uros_sig_thread_die(void)
{
    (void)thread_terminate(mach_thread_self());
    for (;;) { /* unreachable */ }
}

/* ------------------------------------------------------------------ */
/* Thread bootstrap                                                   */
/* ------------------------------------------------------------------ */

static kern_return_t
spawn_handler_thread(void)
{
    vm_address_t  stack = 0;
    kern_return_t kr;

    kr = vm_allocate(mach_task_self(), &stack, UROS_SIG_STACK_SIZE, TRUE);
    if (kr != KERN_SUCCESS)
        return kr;

    /*
     * thread_create_running drops us at our entry with %esp pointing at
     * the supplied stack top.  We bias by 16 bytes to leave room for an
     * artificial frame and a guard slot, then push a "return address"
     * which is in fact the entry of our die-stub — if __uros_sig_loop
     * ever returns through the asm wrapper, ret lands there and the
     * thread terminates cleanly.
     */
    uint32_t *sp = (uint32_t *)(stack + UROS_SIG_STACK_SIZE);
    sp -= 4;                                            /* alignment slack */
    *--sp = (uint32_t)__uros_sig_thread_die;            /* fake return */

    struct i386_thread_state state;
    memset(&state, 0, sizeof state);
    state.eip = (int)__uros_sig_thread_entry;
    state.uesp = (int)sp;
    state.ebp = 0;
    state.cs = 0x17;   /* USER_CS — matches kernel's i386 user GDT */
    state.ds = 0x1f;
    state.es = 0x1f;
    state.ss = 0x1f;
    state.fs = 0x1f;
    state.gs = 0x1f;
    state.efl = 0x202; /* IF=1, reserved bit 1 */

    thread_act_t handler;
    kr = thread_create_running(mach_task_self(),
                               i386_THREAD_STATE,
                               (thread_state_t)&state,
                               i386_THREAD_STATE_COUNT,
                               &handler);
    return kr;
}

/* ------------------------------------------------------------------ */
/* Init                                                               */
/* ------------------------------------------------------------------ */

void
__uros_signals_init(void)
{
    int rc;
    kern_return_t kr;

    if (__sig_initialised)
        return;
    __sig_initialised = 1;

    /* Initialise table: every signal defaults to SIG_DFL, empty mask. */
    for (int i = 0; i < PROC_NSIG; i++)
        __sig_actions[i].sa_handler = SIG_DFL;
    sigemptyset(&__sig_mask);

    /* 1. Resolve proc_server. */
    kr = lookup_proc_server(&__uros_proc_port);
    if (kr != KERN_SUCCESS)
        return;     /* leave SIG_DFL everywhere; signals just won't fire */

    /* 2. Self-register to obtain a pid.  proc_register's cmd_t arg is
     * MIG-declared as a fixed-size char[128]; size the local buffer
     * accordingly so gcc's stringop-overflow analyser doesn't complain. */
    char cmd_name[128] = "musl-task";
    rc = PROC_ERR_INVAL;
    (void)proc_register(__uros_proc_port, PROC_PID_NONE, mach_task_self(),
                        cmd_name, &__uros_my_pid, &rc);
    if (rc != PROC_OK)
        return;

    /* 3. Allocate the receive right our signals arrive on. */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &__sig_port);
    if (kr != KERN_SUCCESS)
        return;
    (void)mach_port_insert_right(mach_task_self(), __sig_port, __sig_port,
                                 MACH_MSG_TYPE_MAKE_SEND);

    /* 4. Register with proc_server. */
    rc = PROC_ERR_INVAL;
    (void)proc_set_signal_port(__uros_proc_port, __uros_my_pid,
                               __sig_port, &rc);
    if (rc != PROC_OK)
        return;

    /* 5. Spawn the handler thread.  We do this last so a failure in
     * any earlier step leaves us with no thread spinning on a port
     * proc_server doesn't know about. */
    (void)spawn_handler_thread();

    /* 6. Hardware exception path (#253 / Phase 4b): install a
     * dedicated exception port so a SEGV/FPE/ILL in this task becomes
     * a clean log+terminate instead of a kernel-level fault.  Failure
     * is non-fatal — the task just keeps the kernel's default
     * exception port and behaves as before. */
    extern void __uros_exceptions_init(void);
    __uros_exceptions_init();
}

/* ------------------------------------------------------------------ */
/* Public POSIX surface — called by handlers.c via the syscall path   */
/* ------------------------------------------------------------------ */

int
__uros_sigaction(int signo, const struct sigaction *act, struct sigaction *old)
{
    if (signo <= 0 || signo >= PROC_NSIG)
        return -EINVAL;
    if (signo == PROC_SIGKILL || signo == PROC_SIGSTOP)
        return -EINVAL;

    if (old)
        *old = __sig_actions[signo];
    if (act)
        __sig_actions[signo] = *act;
    return 0;
}

int
__uros_sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    if (old)
        *old = __sig_mask;
    if (!set)
        return 0;
    switch (how) {
    case SIG_BLOCK:
        for (size_t i = 0; i < sizeof __sig_mask / sizeof(unsigned long); i++)
            ((unsigned long *)&__sig_mask)[i] |= ((const unsigned long *)set)[i];
        break;
    case SIG_UNBLOCK:
        for (size_t i = 0; i < sizeof __sig_mask / sizeof(unsigned long); i++)
            ((unsigned long *)&__sig_mask)[i] &= ~((const unsigned long *)set)[i];
        break;
    case SIG_SETMASK:
        __sig_mask = *set;
        break;
    default:
        return -EINVAL;
    }
    /* SIG_KILL / SIG_STOP can never be blocked. */
    sigdelset(&__sig_mask, PROC_SIGKILL);
    sigdelset(&__sig_mask, PROC_SIGSTOP);
    return 0;
}

int
__uros_kill(proc_pid_t pid, int signo)
{
    if (__uros_proc_port == MACH_PORT_NULL)
        return -ESRCH;
    if (signo < 0 || signo >= PROC_NSIG)
        return -EINVAL;

    int rc = PROC_ERR_INVAL;
    kern_return_t kr = proc_kill(__uros_proc_port, pid, signo, &rc);
    if (kr != KERN_SUCCESS)
        return -ESRCH;
    switch (rc) {
    case PROC_OK:               return 0;
    case PROC_ERR_NOT_FOUND:    return -ESRCH;
    case PROC_ERR_BAD_SIGNO:    return -EINVAL;
    case PROC_ERR_NO_SIGPORT:   return 0;   /* target hasn't registered */
    default:                    return -EPERM;
    }
}
