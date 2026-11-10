/*
 * libposix-uros — POSIX waitpid, wait4 and waitid over proc_server (#254,
 * #934).
 *
 * Every form of the wait is one RPC, proc_wait() (proc.defs): proc_server
 * knows each process's parent, group, stops and exit, and reports a child
 * that has something in the reply.  With nothing to report and no WNOHANG it
 * keeps a send-once right to a port of this call's, and the first matching
 * report comes there as a proc_wait_msg_t, which the call receives.  A
 * zombie is reaped only once its report is queued, so a waiter that goes
 * away loses no child's status.
 *
 * The status comes from proc_server in Linux's encoding, which musl's W*
 * macros read; an exit with 128 + N is an exit, and a death by signal N a
 * death (PROC_EXIT_SIGNALED, #934).  wait4()'s rusage is the child's usage
 * as proc_server last read it: when the child declared its exit, or before
 * proc_server killed it.
 *
 * Not here: waitid()'s P_PIDFD, and si_uid, which is 0 as every uid is.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/message.h>

#include "proc.h"
#include "proc_types.h"

extern mach_port_t  __uros_proc_port;
extern unsigned int __uros_my_pid;

/* musl's <sys/wait.h> values (libposix does not pull musl headers here);
 * proc_types.h's PROC_WAIT_* are the same numbers. */
#define LX_WNOHANG      0x00000001
#define LX_WUNTRACED    0x00000002      /* waitid's WSTOPPED */
#define LX_WEXITED      0x00000004
#define LX_WCONTINUED   0x00000008
#define LX_WNOWAIT      0x01000000
#define LX__WNOTHREAD   0x20000000      /* thread selectors: no threads here */
#define LX__WALL        0x40000000
#define LX__WCLONE      0x80000000u

#define LX_P_ALL        0
#define LX_P_PID        1
#define LX_P_PGID       2

#define LX_SIGCHLD      17
#define LX_SIGCONT      18
#define LX_CLD_EXITED   1
#define LX_CLD_KILLED   2
#define LX_CLD_STOPPED  5
#define LX_CLD_CONTINUED 6

/*
 * The part of the kernel's siginfo_t a child's report fills.  The union
 * after si_code starts at the first offset aligned for a pointer, 16 on
 * x86-64 and 12 on i386, and its SIGCHLD member begins with the pid, the
 * uid and the status.  The whole siginfo_t is 128 bytes, zeroed first.
 */
struct lx_siginfo_chld {
    int32_t     si_signo;
    int32_t     si_errno;
    int32_t     si_code;
#if defined(__x86_64__)
    int32_t     _pad;
#endif
    int32_t     si_pid;
    uint32_t    si_uid;
    int32_t     si_status;
};
#define LX_SIGINFO_SIZE 128
_Static_assert(sizeof(struct lx_siginfo_chld) <= LX_SIGINFO_SIZE,
               "a child's report outgrew siginfo_t");
_Static_assert(offsetof(struct lx_siginfo_chld, si_pid) ==
               (sizeof(void *) == 8 ? 16 : 12),
               "siginfo_t's union is not where the kernel's is");

/* The kernel's struct rusage, which musl's wait4() hands down and turns
 * into its own: the two times as four longs, then fourteen longs. */
#define LX_RUSAGE_LONGS 18

/*
 * The one wait: `which' as waitpid() reads its pid, `options' PROC_WAIT_*.
 * Returns the reported child's pid with *status and *usage filled, 0 under
 * WNOHANG with nothing to report, or a negative errno.
 */
static int
wait_child(int which, int options, int *status, proc_rusage_t *usage)
{
    mach_port_t   notify = MACH_PORT_NULL;
    proc_pid_t    pid = 0;
    int           st = 0, rc = PROC_ERR_INVAL;
    kern_return_t kr;

    *status = 0;
    memset(usage, 0, sizeof(*usage));
    if (__uros_proc_port == MACH_PORT_NULL || __uros_my_pid == 0)
        return -ECHILD;         /* no process server: no children to wait for */

    /* Where a report comes if the wait has to wait: MIG makes proc_server
     * a send-once right from this receive right. */
    if (!(options & PROC_WAIT_NOHANG) &&
        mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                           &notify) != KERN_SUCCESS)
        return -ENOMEM;

    kr = proc_wait(__uros_proc_port, (proc_pid_t)__uros_my_pid, which,
                   options, notify, &pid, &st, usage, &rc);
    if (kr != KERN_SUCCESS)
        rc = PROC_ERR_NO_CHILD;     /* no process server to answer */

    if (rc == PROC_WAIT_QUEUED) {
        proc_wait_msg_t msg;

        kr = mach_msg(&msg.head, MACH_RCV_MSG, 0, sizeof(msg), notify,
                      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
        (void)mach_port_destroy(mach_task_self(), notify);
        if (kr != MACH_MSG_SUCCESS || msg.head.msgh_id != PROC_WAIT_MSGID)
            return -EINTR;
        *status = msg.status;
        *usage = msg.usage;
        return (int)msg.pid;
    }
    if (notify != MACH_PORT_NULL)
        (void)mach_port_destroy(mach_task_self(), notify);

    switch (rc) {
    case PROC_OK:
        *status = st;
        return (int)pid;
    case PROC_ERR_NOT_FOUND:
        return 0;               /* WNOHANG: children there, nothing yet */
    case PROC_ERR_NO_CHILD:
        return -ECHILD;
    case PROC_ERR_NO_SLOT:
        return -ENOMEM;         /* every waiter slot of proc_server taken */
    default:
        return -EINVAL;
    }
}

/* waitpid()'s and wait4()'s options as proc_wait() takes them, or -1 for a
 * bit Linux refuses.  The thread selectors are accepted and change nothing,
 * as every process here has one wait. */
static int
wait4_options(int options)
{
    const unsigned known = LX_WNOHANG | LX_WUNTRACED | LX_WCONTINUED |
                           LX__WNOTHREAD | LX__WALL | LX__WCLONE;

    if ((unsigned)options & ~known)
        return -1;
    return (options & (LX_WNOHANG | LX_WUNTRACED | LX_WCONTINUED)) |
           PROC_WAIT_EXITED;
}

/* The kernel's struct rusage from proc_server's: what it counts, and 0 for
 * what it does not (shared memory, swaps, blocks, signals, switches). */
static void
fill_rusage(void *rusage, const proc_rusage_t *u)
{
    long *k = rusage;

    memset(k, 0, LX_RUSAGE_LONGS * sizeof(long));
    k[0] = (long)u->utime_sec;
    k[1] = (long)u->utime_usec;
    k[2] = (long)u->stime_sec;
    k[3] = (long)u->stime_usec;
    k[4] = (long)u->maxrss_kb;          /* ru_maxrss, in KiB */
    k[8] = (long)u->minflt;             /* ru_minflt */
    k[9] = (long)u->majflt;             /* ru_majflt */
    k[13] = (long)u->msgsnd;            /* ru_msgsnd */
    k[14] = (long)u->msgrcv;            /* ru_msgrcv */
}

int
__uros_wait4(int pid, int *status_out, int options, void *rusage)
{
    proc_rusage_t usage;
    int status = 0, opts = wait4_options(options), r;

    if (opts < 0)
        return -EINVAL;
    r = wait_child(pid, opts, &status, &usage);
    if (r <= 0)
        return r;
    if (status_out)
        *status_out = status;
    if (rusage)
        fill_rusage(rusage, &usage);
    return r;
}

int
__uros_waitpid(int pid, int *status_out, int options)
{
    return __uros_wait4(pid, status_out, options, NULL);
}

/*
 * waitid(): the same wait, with what to wait for named by idtype and id and
 * the answer in a siginfo_t.  Under WNOHANG with nothing to report it
 * answers 0 with si_pid 0, as Linux does.
 */
int
__uros_waitid(int idtype, unsigned long id, void *info, int options,
              void *rusage)
{
    const unsigned known = LX_WNOHANG | LX_WUNTRACED | LX_WEXITED |
                           LX_WCONTINUED | LX_WNOWAIT | LX__WNOTHREAD |
                           LX__WALL | LX__WCLONE;
    struct lx_siginfo_chld *si = info;
    proc_rusage_t usage;
    int which, status = 0, r;

    if ((unsigned)options & ~known)
        return -EINVAL;
    if (!(options & (LX_WEXITED | LX_WUNTRACED | LX_WCONTINUED)))
        return -EINVAL;
    switch (idtype) {
    case LX_P_ALL:
        which = -1;
        break;
    case LX_P_PID:
        if (id == 0 || id > 0x7fffffffUL)
            return -EINVAL;
        which = (int)id;
        break;
    case LX_P_PGID:
        if (id > 0x7fffffffUL)
            return -EINVAL;
        which = id == 0 ? 0 : -(int)id;     /* 0: the caller's own group */
        break;
    default:
        return -EINVAL;
    }

    r = wait_child(which, options & (LX_WNOHANG | LX_WUNTRACED | LX_WEXITED |
                                     LX_WCONTINUED | LX_WNOWAIT),
                   &status, &usage);
    if (r < 0)
        return r;
    if (rusage != NULL) {
        if (r > 0)
            fill_rusage(rusage, &usage);
        else
            memset(rusage, 0, LX_RUSAGE_LONGS * sizeof(long));
    }
    if (si == NULL)
        return 0;
    memset(si, 0, LX_SIGINFO_SIZE);
    if (r == 0)
        return 0;               /* WNOHANG, nothing: si_pid stays 0 */
    si->si_signo = LX_SIGCHLD;
    si->si_pid = r;
    si->si_uid = 0;
    if ((status & 0x7f) == 0) {
        si->si_code = LX_CLD_EXITED;
        si->si_status = (status >> 8) & 0xff;
    } else if (status == 0xffff) {
        si->si_code = LX_CLD_CONTINUED;
        si->si_status = LX_SIGCONT;
    } else if ((status & 0xff) == 0x7f) {
        si->si_code = LX_CLD_STOPPED;
        si->si_status = (status >> 8) & 0xff;
    } else {
        si->si_code = LX_CLD_KILLED;
        si->si_status = status & 0x7f;
    }
    return 0;
}
