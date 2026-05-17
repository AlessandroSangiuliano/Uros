/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * sig_test — userspace exerciser for proc_server signals (#238) +
 * process groups / sessions (#239) + resource accounting (#240).
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

/* ==================================================================
 *  Resource accounting (v0.4.0 / #240)
 * ================================================================== */

static void
test_getrusage_self(void)
{
    proc_rusage_t r;
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("getrusage(self) returns non-zero virtual_kb");
    memset(&r, 0, sizeof(r));
    kr = proc_getrusage(proc_port, my_pid, &r, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_getrusage");
    EXPECT_RC(rc, PROC_OK, "proc_getrusage rc");
    /* A live task always has some virtual address space. */
    EXPECT(r.virtual_kb > 0, "virtual_kb should be > 0 for a live task");
    /* maxrss should also be non-zero for a running task (it has at
     * least its code + stack resident). */
    EXPECT(r.maxrss_kb > 0, "maxrss_kb should be > 0 for a live task");
    printf("  rusage(self): utime=%u.%06us stime=%u.%06us "
           "vsize=%u KiB rss=%u KiB minflt=%u majflt=%u msgs=%u/%u\n",
           r.utime_sec, r.utime_usec,
           r.stime_sec, r.stime_usec,
           r.virtual_kb, r.maxrss_kb,
           r.minflt, r.majflt, r.msgsnd, r.msgrcv);
    PASS();
}

static void
test_getrusage_bad_pid(void)
{
    proc_rusage_t r;
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("getrusage(bad pid) -> NOT_FOUND");
    memset(&r, 0, sizeof(r));
    kr = proc_getrusage(proc_port, 0xDEADBEEF, &r, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_getrusage kr");
    EXPECT_RC(rc, PROC_ERR_NOT_FOUND, "expected NOT_FOUND");
    PASS();
}

/* ==================================================================
 *  Process groups + sessions (v0.3.0 / #239)
 * ================================================================== */

/*
 * After setup_self() with parent_pid=0, we are pgrp leader and
 * session leader of pgrp=sid=my_pid.  Validate that proc_server
 * agrees, and that a child registered under us inherits both.
 */
static void
test_pgrp_inherit(void)
{
    mach_port_t child = MACH_PORT_NULL;
    proc_pid_t child_pid = 0;
    proc_pid_t got = 0;
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("pgrp/sid: self leader + child inherits");

    /* Self should be its own pgrp + sid leader (set at register). */
    rc = 0; got = 0;
    kr = proc_getpgid(proc_port, my_pid, &got, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "getpgid self");
    EXPECT_RC(rc, PROC_OK, "getpgid self rc");
    EXPECT(got == my_pid, "self.pgrp should be my_pid");

    rc = 0; got = 0;
    kr = proc_getsid(proc_port, my_pid, &got, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "getsid self");
    EXPECT_RC(rc, PROC_OK, "getsid self rc");
    EXPECT(got == my_pid, "self.sid should be my_pid");

    /* Child registered with parent_pid=my_pid must inherit both. */
    kr = task_create(mach_task_self(), (ledger_port_array_t)0, 0,
                     FALSE, &child);
    EXPECT_KR(kr, KERN_SUCCESS, "task_create");
    kr = proc_register(proc_port, my_pid, child, (char *)"inh_child",
                       &child_pid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_register child");
    EXPECT_RC(rc, PROC_OK, "proc_register child rc");

    rc = 0; got = 0;
    kr = proc_getpgid(proc_port, child_pid, &got, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "getpgid child");
    EXPECT(got == my_pid, "child.pgrp should inherit my_pid");

    rc = 0; got = 0;
    kr = proc_getsid(proc_port, child_pid, &got, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "getsid child");
    EXPECT(got == my_pid, "child.sid should inherit my_pid");

    /* Drain the SIGCHLD before next test. */
    (void)task_terminate(child);
    {
        int signo = 0; proc_pid_t s = 0;
        (void)recv_signal(2000, &signo, &s);
    }
    PASS();
}

/*
 * setsid on a pgrp leader must fail with PROC_ERR_PERM (POSIX EPERM).
 * We are leader from setup, so the call on self must error.  Also
 * verify the same on a freshly-created child (we make it leader by
 * setpgid then try setsid on it).
 */
static void
test_setsid_perm(void)
{
    int rc = 0;
    proc_pid_t new_sid = 0;
    kern_return_t kr;

    BEGIN_TEST("setsid on pgrp leader -> PERM");
    kr = proc_setsid(proc_port, my_pid, &new_sid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "setsid self");
    EXPECT_RC(rc, PROC_ERR_PERM, "self is leader, expected PERM");
    PASS();
}

/*
 * setpgid(child, child) moves a child out of the parent's pgrp into
 * its own — child becomes pgrp leader, still in the parent's session.
 */
static void
test_setpgid_new_pgrp(void)
{
    mach_port_t child = MACH_PORT_NULL;
    proc_pid_t child_pid = 0;
    proc_pid_t got = 0;
    int rc = 0;
    kern_return_t kr;

    BEGIN_TEST("setpgid(child, child) -> child is new pgrp leader");
    kr = task_create(mach_task_self(), (ledger_port_array_t)0, 0,
                     FALSE, &child);
    EXPECT_KR(kr, KERN_SUCCESS, "task_create");
    kr = proc_register(proc_port, my_pid, child, (char *)"pgleader",
                       &child_pid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "proc_register child");

    rc = 0;
    kr = proc_setpgid(proc_port, child_pid, child_pid, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "setpgid child");
    EXPECT_RC(rc, PROC_OK, "setpgid rc");

    rc = 0; got = 0;
    kr = proc_getpgid(proc_port, child_pid, &got, &rc);
    EXPECT(got == child_pid, "child.pgrp should be child_pid");

    /* Same session — child still belongs to ours. */
    rc = 0; got = 0;
    kr = proc_getsid(proc_port, child_pid, &got, &rc);
    EXPECT(got == my_pid, "child.sid should still be my_pid");

    (void)task_terminate(child);
    {
        int signo = 0; proc_pid_t s = 0;
        (void)recv_signal(2000, &signo, &s);
    }
    PASS();
}

/*
 * killpg(pgrp, SIGUSR1) on a pgrp containing two of our children
 * (which share our signal_port since they don't register their own
 * — for the test we move them into a fresh pgrp first and rely on
 * the parent's signal_port being checked only for OUR pid; killpg
 * dispatches to each pid's own signal_port, which our children
 * don't have, so n_sent should be 0).
 *
 * For a true catchable-killpg test we need children with their own
 * signal_ports.  Bare task_create children can't run code (no thread
 * trampoline), so simulate by registering FAKE child pids that point
 * to throwaway recv ports as task_port, and give each a real
 * signal_port we hold on the parent side.  proc_server doesn't
 * inspect task_port for anything other than dead-name notify, which
 * we don't care about firing here.
 */
static void
test_killpg_catchable(void)
{
    mach_port_t fake_taskA, fake_taskB;
    mach_port_t sigA_recv, sigB_recv;
    mach_port_t pgrp_set;       /* port_set covering both sig ports */
    proc_pid_t  pidA = 0, pidB = 0;
    proc_pid_t  pgrp;
    unsigned    n_sent = 0;
    int         rc = 0;
    int         got_A = 0, got_B = 0;
    kern_return_t kr;
    int i;

    BEGIN_TEST("killpg(pgrp, SIGUSR1) -> 2 recipients");

    /* Two fake task ports — only used as table keys / dead-name
     * subjects.  proc_server will install dead-name notify on them
     * but we won't trigger it during this test. */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &fake_taskA);
    EXPECT_KR(kr, KERN_SUCCESS, "alloc fake_taskA");
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &fake_taskB);
    EXPECT_KR(kr, KERN_SUCCESS, "alloc fake_taskB");
    (void)mach_port_insert_right(mach_task_self(), fake_taskA, fake_taskA,
                                 MACH_MSG_TYPE_MAKE_SEND);
    (void)mach_port_insert_right(mach_task_self(), fake_taskB, fake_taskB,
                                 MACH_MSG_TYPE_MAKE_SEND);

    kr = proc_register(proc_port, my_pid, fake_taskA, (char *)"kpgA",
                       &pidA, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "register A");
    kr = proc_register(proc_port, my_pid, fake_taskB, (char *)"kpgB",
                       &pidB, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "register B");

    /* Per-child signal ports + put them in a port_set so we can
     * receive from either in any order. */
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &sigA_recv);
    EXPECT_KR(kr, KERN_SUCCESS, "alloc sigA_recv");
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
                            &sigB_recv);
    EXPECT_KR(kr, KERN_SUCCESS, "alloc sigB_recv");
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET,
                            &pgrp_set);
    EXPECT_KR(kr, KERN_SUCCESS, "alloc port_set");
    (void)mach_port_move_member(mach_task_self(), sigA_recv, pgrp_set);
    (void)mach_port_move_member(mach_task_self(), sigB_recv, pgrp_set);
    (void)mach_port_insert_right(mach_task_self(), sigA_recv, sigA_recv,
                                 MACH_MSG_TYPE_MAKE_SEND);
    (void)mach_port_insert_right(mach_task_self(), sigB_recv, sigB_recv,
                                 MACH_MSG_TYPE_MAKE_SEND);

    rc = 0;
    kr = proc_set_signal_port(proc_port, pidA, sigA_recv, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "set_signal_port A");
    rc = 0;
    kr = proc_set_signal_port(proc_port, pidB, sigB_recv, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "set_signal_port B");

    /* Move both into a fresh pgrp.  Pick pidA as pgrp leader. */
    pgrp = pidA;
    rc = 0;
    kr = proc_setpgid(proc_port, pidA, pgrp, &rc);
    EXPECT_RC(rc, PROC_OK, "setpgid A");
    rc = 0;
    kr = proc_setpgid(proc_port, pidB, pgrp, &rc);
    EXPECT_RC(rc, PROC_OK, "setpgid B");

    rc = 0; n_sent = 0;
    kr = proc_killpg(proc_port, pgrp, PROC_SIGUSR1, &n_sent, &rc);
    EXPECT_KR(kr, KERN_SUCCESS, "killpg kr");
    EXPECT_RC(rc, PROC_OK, "killpg rc");
    EXPECT(n_sent == 2, "killpg should reach both A and B");

    /* Drain two messages from the port_set — order is unspecified, so
     * track each receipt by msgh_local_port. */
    for (i = 0; i < 2; i++) {
        proc_signal_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        msg.head.msgh_size       = sizeof(msg);
        msg.head.msgh_local_port = pgrp_set;
        kr = mach_msg(&msg.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT,
                      0, sizeof(msg), pgrp_set, 2000, MACH_PORT_NULL);
        EXPECT_KR(kr, KERN_SUCCESS, "recv killpg sig");
        EXPECT(msg.head.msgh_id == PROC_SIGNAL_MSGID, "wrong msgid");
        EXPECT(msg.signo == PROC_SIGUSR1, "wrong signo");
        if (msg.head.msgh_local_port == sigA_recv) got_A = 1;
        if (msg.head.msgh_local_port == sigB_recv) got_B = 1;
    }
    EXPECT(got_A && got_B, "both children should have received");

    /* Cleanup: terminate fakes to drain dead-name + SIGCHLDs. */
    (void)mach_port_destroy(mach_task_self(), fake_taskA);
    (void)mach_port_destroy(mach_task_self(), fake_taskB);
    for (i = 0; i < 2; i++) {
        int s = 0; proc_pid_t sender = 0;
        (void)recv_signal(2000, &s, &sender);
    }
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

    printf("\n=== sig_test (proc_server v0.2.0/v0.3.0/v0.4.0 / #238 + #239 + #240) ===\n");
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

        /* v0.3.0 / #239 — process groups + sessions */
        test_pgrp_inherit();
        test_setsid_perm();
        test_setpgid_new_pgrp();
        test_killpg_catchable();

        /* v0.4.0 / #240 — resource accounting */
        test_getrusage_self();
        test_getrusage_bad_pid();
    }

    printf("\n=== sig_test: %u PASS, %u FAIL ===\n", g_pass, g_fail);
    return (g_fail == 0) ? 0 : 1;
}
