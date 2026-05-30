/*
 * libposix-uros — POSIX waitpid / wait4 over proc_server (#254 / Phase 5).
 *
 * proc_server already does the heavy lifting: `proc_subscribe_exit`
 * (#238) takes a send-once notify port and sends a single
 * `proc_exit_msg_t {exit_code, pid}` to it when the pid becomes
 * zombie.  Already-zombie pids fire the notification immediately, so
 * the race-free wait is free.
 *
 * waitpid here is therefore: allocate a receive right, hand its
 * send-once to proc_server, mach_msg(RCV), unpack exit_code, return.
 * status is encoded the Linux way (low byte = signal, second-low =
 * exit value) so musl's WIFEXITED / WEXITSTATUS macros DTRT.
 *
 * Out of scope for Phase 5:
 *   - WNOHANG fast-path (we'd need proc_list + filter on caller pid)
 *   - WIFSTOPPED (SIGSTOP/SIGCONT tracking not surfaced by proc_server)
 *   - pid == -1 / pid == 0 / pid == -pgrp ranges; only pid > 0 works.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/mach_interface.h>
#include <mach/message.h>

#include "proc.h"
#include "proc_types.h"

extern mach_port_t __uros_proc_port;

/* ------------------------------------------------------------------ */
/* Linux wait-status encoding                                          */
/* ------------------------------------------------------------------ */

/*
 * The Phase 4a exit handler exits with `128 + signo` when a signal
 * terminates via SIG_DFL.  We keep that convention: exit_code values
 * 128..159 are "killed by signal N", anything else is a normal exit
 * status.  Encode them into the Linux `int status` layout musl
 * understands (W_EXITCODE in <sys/wait.h>).
 */
static int
encode_status(int exit_code)
{
    if (exit_code >= 128 && exit_code <= 191) {
        /* Killed by signal (exit_code - 128). */
        return (exit_code - 128) & 0x7f;
    }
    /* Normal exit: WIFEXITED expects status & 0x7f == 0,
     * WEXITSTATUS returns (status >> 8) & 0xff. */
    return (exit_code & 0xff) << 8;
}

/* ------------------------------------------------------------------ */
/* waitpid                                                            */
/* ------------------------------------------------------------------ */

int
__uros_waitpid(int pid, int *status_out, int options)
{
    (void)options;     /* WNOHANG/WUNTRACED not yet honoured */

    if (__uros_proc_port == MACH_PORT_NULL)
        return -ESRCH;
    if (pid <= 0)
        return -EINVAL;    /* Phase 5 supports specific-pid waits only */

    /* 1. Allocate the receive right proc_server will signal. */
    mach_port_t notify_port = MACH_PORT_NULL;
    kern_return_t kr = mach_port_allocate(mach_task_self(),
                                          MACH_PORT_RIGHT_RECEIVE,
                                          &notify_port);
    if (kr != KERN_SUCCESS)
        return -ENOMEM;

    /* 2. Insert a send-once and hand it to proc_server.  The MIG type
     *    polymorphism (mach_port_make_send_once_t) tells the kernel
     *    to consume a send right and turn it into the limited form. */
    kr = mach_port_insert_right(mach_task_self(), notify_port, notify_port,
                                MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
        (void)mach_port_destroy(mach_task_self(), notify_port);
        return -ENOMEM;
    }
    int rc = PROC_ERR_INVAL;
    kr = proc_subscribe_exit(__uros_proc_port,
                             (proc_pid_t)pid, notify_port, &rc);
    if (kr != KERN_SUCCESS) {
        (void)mach_port_destroy(mach_task_self(), notify_port);
        return -ECHILD;
    }
    if (rc == PROC_ERR_NOT_FOUND) {
        (void)mach_port_destroy(mach_task_self(), notify_port);
        return -ECHILD;
    }
    if (rc != PROC_OK) {
        (void)mach_port_destroy(mach_task_self(), notify_port);
        return -EINVAL;
    }

    /* 3. Block waiting for the proc_exit_msg_t.  Make the buffer big
     *    enough for the trailer that mach_msg always appends. */
    proc_exit_msg_t msg;
    kr = mach_msg(&msg.head,
                  MACH_RCV_MSG,
                  0,
                  sizeof msg,
                  notify_port,
                  MACH_MSG_TIMEOUT_NONE,
                  MACH_PORT_NULL);
    (void)mach_port_destroy(mach_task_self(), notify_port);
    if (kr != KERN_SUCCESS)
        return -EINTR;
    if (msg.head.msgh_id != PROC_EXIT_MSGID)
        return -EINTR;

    if (status_out)
        *status_out = encode_status(msg.exit_code);
    return (int)msg.pid;
}
