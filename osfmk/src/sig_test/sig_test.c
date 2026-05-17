/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * sig_test — userspace exerciser for proc_server v0.2.0 signals (#238).
 *
 * Lives outside ipc_bench because it is a feature test, not a
 * benchmark — same pattern as kernel242_test (#242).  Exercises every
 * proc_kill dispatch class:
 *
 *   - catchable signal (SIGUSR1, SIGTERM)   → mach_msg on signal_port
 *   - PROC_ERR_BAD_SIGNO / PROC_ERR_NOT_FOUND error paths
 *   - SIGSTOP / SIGCONT                     → task_suspend / task_resume
 *   - SIGKILL                               → task_terminate + SIGCHLD
 *
 * SIGCHLD wiring is validated by spawning a bare child task, marking
 * it as our child via proc_register, then sending SIGKILL — the dead
 * child's pid must appear on OUR signal_port as PROC_SIGCHLD.
 *
 * sigaction / sigmask / hardware-trap exception ports are out of
 * scope: those live in libposix-uros (#218).  Here we receive the
 * raw proc_signal_msg_t directly.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/mach_interface.h>
#include <mach/thread_switch.h>
#include <servers/netname.h>
#include <proc_types.h>
#include <stdio.h>
#include <string.h>

/* MIG-generated user stubs from proc.defs (subsystem 3200). */
#include "proc.h"

extern kern_return_t bootstrap_ports(mach_port_t bootstrap,
                                     mach_port_t *host_port,
                                     mach_port_t *device_port,
                                     mach_port_t *root_ledger_wired,
                                     mach_port_t *root_ledger_paged,
                                     mach_port_t *security_port);
extern void printf_init(mach_port_t device_server_port);

/* ------------------------------------------------------------------ */

static unsigned int g_pass = 0;
static unsigned int g_fail = 0;

#define EXPECT(expr, msg) do {                                         \
    if (!(expr)) {                                                     \
        printf("  FAIL: %s (line %d)\n", msg, __LINE__);               \
        g_fail++;                                                      \
        return;                                                        \
    }                                                                  \
} while (0)

#define EXPECT_KR(expr, want, msg) do {                                \
    kern_return_t __kr = (expr);                                       \
    if (__kr != (want)) {                                              \
        printf("  FAIL: %s kr=%d want=%d (line %d)\n",                 \
               msg, __kr, (want), __LINE__);                           \
        g_fail++;                                                      \
        return;                                                        \
    }                                                                  \
} while (0)

#define EXPECT_RC(rc, want, msg) do {                                  \
    if ((rc) != (want)) {                                              \
        printf("  FAIL: %s rc=%d want=%d (line %d)\n",                 \
               msg, (rc), (want), __LINE__);                           \
        g_fail++;                                                      \
        return;                                                        \
    }                                                                  \
} while (0)

#define PASS() do { printf("  PASS\n"); g_pass++; } while (0)

#define BEGIN_TEST(name) do { printf("TEST: %s\n", (name)); } while (0)

/* ------------------------------------------------------------------ */

static mach_port_t proc_port    = MACH_PORT_NULL;
static proc_pid_t  my_pid       = 0;
static mach_port_t my_sig_recv  = MACH_PORT_NULL;

/*
 * Receive one proc_signal_msg_t with a millisecond timeout.
 * On success returns 0 and fills *out_signo / *out_sender; on
 * timeout or unexpected msg returns negative.
 */
static int
recv_signal(int timeout_ms, int *out_signo, proc_pid_t *out_sender)
{
    proc_signal_msg_t msg;
    kern_return_t kr;

    memset(&msg, 0, sizeof(msg));
    msg.head.msgh_size       = sizeof(msg);
    msg.head.msgh_local_port = my_sig_recv;

    kr = mach_msg(&msg.head,
                  MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                  0, sizeof(msg),
                  my_sig_recv,
                  (mach_msg_timeout_t)timeout_ms,
                  MACH_PORT_NULL);
    if (kr != KERN_SUCCESS)
        return -1;
    if (msg.head.msgh_id != PROC_SIGNAL_MSGID)
        return -2;
    if (out_signo)  *out_signo  = msg.signo;
    if (out_sender) *out_sender = msg.sender_pid;
    return 0;
}

/* ------------------------------------------------------------------ */

static void
setup_self(void)
{
    mach_port_t sig_send;
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("setup: self-register + signal_port");

    /* Register ourselves with proc_server.  parent_pid=0 so we don't
     * receive SIGCHLD for our own death. */
    kr = proc_register(proc_port, PROC_PID_NONE, mach_task_self(),
                       (char *)"sig_test", &my_pid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_register");
    EXPECT_RC(rc, PROC_OK, "proc_register rc");
    EXPECT(my_pid != 0, "pid != 0");

    /* Allocate signal_port; pass a send right to proc_server. */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &my_sig_recv);
    EXPECT_KR(kr, KERN_SUCCESS, "port_allocate sig_recv");
    kr = mach_port_insert_right(mach_task_self(), my_sig_recv, my_sig_recv,
                                MACH_MSG_TYPE_MAKE_SEND);
    EXPECT_KR(kr, KERN_SUCCESS, "insert_right sig_send");
    sig_send = my_sig_recv;     /* same name, send right now held */

    rc = 0;
    kr = proc_set_signal_port(proc_port, my_pid, sig_send, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_set_signal_port");
    EXPECT_RC(rc, PROC_OK, "proc_set_signal_port rc");

    printf("  setup: my_pid=%u sig_port=0x%x\n",
           my_pid, (unsigned)my_sig_recv);
    PASS();
}

static void
test_self_sigusr1(void)
{
    int rc = 0;
    int signo = 0;
    proc_pid_t sender = 0;
    kern_return_t kr;

    BEGIN_TEST("proc_kill(self, SIGUSR1) -> recv");
    kr = proc_kill(proc_port, my_pid, PROC_SIGUSR1, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill");
    EXPECT_RC(rc, PROC_OK, "proc_kill rc");
    EXPECT(recv_signal(500, &signo, &sender) == 0,
           "recv_signal timed out");
    EXPECT(signo == PROC_SIGUSR1, "wrong signo");
    PASS();
}

static void
test_self_sigterm(void)
{
    int rc = 0;
    int signo = 0;
    proc_pid_t sender = 0;
    kern_return_t kr;

    BEGIN_TEST("proc_kill(self, SIGTERM) -> recv");
    kr = proc_kill(proc_port, my_pid, PROC_SIGTERM, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill");
    EXPECT_RC(rc, PROC_OK, "proc_kill rc");
    EXPECT(recv_signal(500, &signo, &sender) == 0,
           "recv_signal timed out");
    EXPECT(signo == PROC_SIGTERM, "wrong signo");
    PASS();
}

static void
test_bad_signo(void)
{
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("proc_kill(self, bad signo) -> BAD_SIGNO");
    kr = proc_kill(proc_port, my_pid, 99, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill kr");
    EXPECT_RC(rc, PROC_ERR_BAD_SIGNO, "expected BAD_SIGNO");
    PASS();
}

static void
test_bad_pid(void)
{
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("proc_kill(missing pid) -> NOT_FOUND");
    kr = proc_kill(proc_port, 0xDEADBEEF, PROC_SIGTERM, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill kr");
    EXPECT_RC(rc, PROC_ERR_NOT_FOUND, "expected NOT_FOUND");
    PASS();
}

/*
 * SIGSTOP + SIGCONT on a freshly-created child task.  The child has
 * no threads so it never runs, but task_suspend/task_resume must
 * return KERN_SUCCESS — that's the success criterion for the kernel
 * dispatch.
 */
static void
test_child_stop_cont(void)
{
    mach_port_t child = MACH_PORT_NULL;
    proc_pid_t child_pid = 0;
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("SIGSTOP + SIGCONT on bare child");
    kr = task_create(mach_task_self(), (ledger_port_array_t)0, 0,
                     FALSE, &child);
    EXPECT_KR(kr, KERN_SUCCESS, "task_create");

    kr = proc_register(proc_port, my_pid, child, (char *)"stopchild",
                       &child_pid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_register child");
    EXPECT_RC(rc, PROC_OK, "proc_register child rc");

    rc = 0;
    kr = proc_kill(proc_port, child_pid, PROC_SIGSTOP, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill SIGSTOP kr");
    EXPECT_RC(rc, PROC_OK, "SIGSTOP rc");

    rc = 0;
    kr = proc_kill(proc_port, child_pid, PROC_SIGCONT, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill SIGCONT kr");
    EXPECT_RC(rc, PROC_OK, "SIGCONT rc");

    /* Tear the child down.  Because we registered it as ours,
     * proc_server will emit a SIGCHLD on OUR signal_port — drain it
     * so the next test sees only its own SIGCHLD. */
    (void)task_terminate(child);
    {
        int signo = 0;
        proc_pid_t sender = 0;
        EXPECT(recv_signal(2000, &signo, &sender) == 0,
               "SIGCHLD for stopchild never arrived");
        EXPECT(signo == PROC_SIGCHLD, "stopchild SIGCHLD wrong signo");
        EXPECT(sender == child_pid,
               "stopchild SIGCHLD.sender_pid mismatch");
    }
    PASS();
}

/*
 * SIGKILL on a child → task_terminate fires → dead_name notification
 * → SIGCHLD lands on our signal_port carrying child_pid as sender.
 * This is the end-to-end POSIX waitpid-style fence v0.2.0 ships.
 */
static void
test_child_kill_sigchld(void)
{
    mach_port_t child = MACH_PORT_NULL;
    proc_pid_t child_pid = 0;
    int rc = 0;
    int signo = 0;
    proc_pid_t sender = 0;
    kern_return_t kr;

    BEGIN_TEST("SIGKILL child -> SIGCHLD to parent");
    kr = task_create(mach_task_self(), (ledger_port_array_t)0, 0,
                     FALSE, &child);
    EXPECT_KR(kr, KERN_SUCCESS, "task_create");

    kr = proc_register(proc_port, my_pid, child, (char *)"killchild",
                       &child_pid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_register child");
    EXPECT_RC(rc, PROC_OK, "proc_register child rc");

    rc = 0;
    kr = proc_kill(proc_port, child_pid, PROC_SIGKILL, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_kill SIGKILL kr");
    EXPECT_RC(rc, PROC_OK, "SIGKILL rc");

    /* The dead-name notification is asynchronous — give proc_server
     * generous slack (it has to process the notify msg and then
     * mach_msg us back). */
    EXPECT(recv_signal(2000, &signo, &sender) == 0,
           "no SIGCHLD arrived");
    EXPECT(signo == PROC_SIGCHLD, "wrong signo (want SIGCHLD)");
    EXPECT(sender == child_pid,
           "SIGCHLD.sender_pid should be the dead child");

    /* Re-killing the zombie must report NOT_FOUND. */
    rc = 0;
    kr = proc_kill(proc_port, child_pid, PROC_SIGTERM, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "post-mortem proc_kill kr");
    EXPECT_RC(rc, PROC_ERR_NOT_FOUND, "zombie should be NOT_FOUND");

    PASS();
}

/* ------------------------------------------------------------------ */

static void
wait_for_proc_server(void)
{
    int spins;
    for (spins = 0; spins < 100; spins++) {
        if (netname_look_up(name_server_port, "",
                            (char *)"proc_server", &proc_port)
            == NETNAME_SUCCESS)
            return;
        (void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 200);
    }
}

int
main(int argc, char **argv)
{
    mach_port_t host, device, lw, lp, sec;
    (void)argc; (void)argv;

    if (bootstrap_ports(bootstrap_port, &host, &device, &lw, &lp, &sec)
        != KERN_SUCCESS)
        return 1;
    printf_init(device);

    printf("\n=== sig_test (proc_server v0.2.0 / #238) ===\n");
    wait_for_proc_server();
    if (proc_port == MACH_PORT_NULL) {
        printf("  sig_test: proc_server never appeared -- SKIP\n");
        return 0;
    }

    setup_self();
    /* If setup failed everything downstream would FAIL too — skip and
     * report just the setup failure. */
    if (g_fail == 0) {
        test_self_sigusr1();
        test_self_sigterm();
        test_bad_signo();
        test_bad_pid();
        test_child_stop_cont();
        test_child_kill_sigchld();
    }

    printf("\n=== sig_test: %u PASS, %u FAIL ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
