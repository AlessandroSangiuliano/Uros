/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * ipc_bench.c — IPC Performance Benchmark for OSFMK
 *
 * Measures Mach IPC latency both intra-task (thread-to-thread) and
 * inter-task (task-to-task).  Inspired by the old OSF mach_perf/ipc
 * test suite but written as a self-contained standalone Mach server.
 *
 * Tests performed:
 *   1. Intra-task null RPC        (cthread echo, no payload)
 *   2. Intra-task inline RPC      (128 / 1024 / 4096 bytes)
 *   3. Inter-task null RPC        (child task echo, no payload)
 *   4. Inter-task inline RPC      (128 / 1024 / 4096 bytes)
 *   5. Port operations            (alloc+destroy, mach_port_names)
 *
 * Timing uses the Mach REALTIME_CLOCK via clock_get_time().
 */

#include <mach.h>
#include <mach/mach_host.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/clock.h>
#include <mach/clock_types.h>
#include <mach/thread_switch.h>
#include <mach/i386/thread_status.h>
#include <sa_mach.h>
#include <cthreads.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>

/* ===================================================================
 * Configuration
 * =================================================================== */

#define WARMUP_ITERS		100
#define BENCH_ITERS		10000
#define CHILD_STACK_SIZE	(64 * 1024)	/* 64 KB */

/*
 * Well-known port names inserted into the child task's IPC space
 * for the inter-task benchmark.
 */
#define CHILD_RECV_NAME		((mach_port_t) 0x1503)
#define CHILD_SEND_NAME		((mach_port_t) 0x1603)

/* ===================================================================
 * Global ports
 * =================================================================== */

static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;
static mach_port_t	clock_port;

/* ===================================================================
 * Message structures
 * =================================================================== */

/*
 * Null message — header only.
 */
typedef struct {
    mach_msg_header_t	head;
} bench_null_msg_t;

/*
 * Inline data messages for payload tests.
 */
typedef struct {
    mach_msg_header_t	head;
    char		data[128];
} bench_128_msg_t;

typedef struct {
    mach_msg_header_t	head;
    char		data[1024];
} bench_1024_msg_t;

typedef struct {
    mach_msg_header_t	head;
    char		data[4096];
} bench_4096_msg_t;

/*
 * Generic receive buffer — large enough for any message.
 */
typedef struct {
    mach_msg_header_t	head;
    char		data[4096 + 64];
} bench_recv_buf_t;

/* ===================================================================
 * kmsg cache statistics
 * =================================================================== */

typedef struct {
    unsigned int tries;
    unsigned int misses;
} cache_snap_t;

static int
cache_snap(cache_snap_t *s)
{
    host_ipc_cache_info_data_t	info;
    mach_msg_type_number_t		count = HOST_IPC_CACHE_INFO_COUNT;
    kern_return_t			kr;

    kr = host_info(host_port, HOST_IPC_CACHE_INFO,
		   (host_info_t)&info, &count);
    if (kr != KERN_SUCCESS)
	return 0;
    s->tries  = info.tries;
    s->misses = info.misses;
    return 1;
}

static void
print_cache_stats(const char *label,
		  const cache_snap_t *before, const cache_snap_t *after)
{
    unsigned int tries  = after->tries  - before->tries;
    unsigned int misses = after->misses - before->misses;
    unsigned int hits   = tries - misses;
    unsigned int pct    = tries ? (hits * 100) / tries : 0;

    printf("  [cache] %-30s tries=%-6u hits=%-6u misses=%-6u hit%%=%u%%\n",
	   label, tries, hits, misses, pct);
}

/* ===================================================================
 * Timing helpers
 * =================================================================== */

static void
get_time(tvalspec_t *tv)
{
    clock_get_time(clock_port, tv);
}

/*
 * Return elapsed nanoseconds between two tvalspec_t values.
 */
static unsigned long
elapsed_ns(const tvalspec_t *before, const tvalspec_t *after)
{
    unsigned long ns;
    ns  = (unsigned long)(after->tv_sec  - before->tv_sec)  * 1000000000UL;
    ns += (unsigned long)(after->tv_nsec - before->tv_nsec);
    return ns;
}

static void
print_result(const char *label, unsigned long total_ns, int iters)
{
    unsigned long ns_per_op = total_ns / (unsigned long)iters;
    unsigned long us_whole  = ns_per_op / 1000;
    unsigned long us_frac   = (ns_per_op % 1000) / 10;   /* 2 decimal digits */
    unsigned long total_us  = total_ns / 1000;

    printf("  %-34s %5lu.%02lu us/op  (%d iters, %lu us total)\n",
	   label, us_whole, us_frac, iters, total_us);
}

/* ===================================================================
 * Intra-task echo thread (cthread)
 *
 * Receives a message on `port`, sends a reply back on the
 * remote_port embedded in the message header.
 * =================================================================== */

static void *
echo_thread_func(void *arg)
{
    mach_port_t		port = (mach_port_t)(unsigned long)arg;
    bench_recv_buf_t	msg;
    bench_null_msg_t	reply;
    kern_return_t	kr;

    for (;;) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,
		      sizeof(msg),
		      port,
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS)
	    continue;

	/* Send a minimal reply */
	reply.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	reply.head.msgh_size	    = sizeof(reply);
	reply.head.msgh_remote_port = msg.head.msgh_remote_port;
	reply.head.msgh_local_port  = MACH_PORT_NULL;
	reply.head.msgh_id	    = msg.head.msgh_id + 100;

	mach_msg(&reply.head,
		 MACH_SEND_MSG,
		 sizeof(reply),
		 0,
		 MACH_PORT_NULL,
		 MACH_MSG_TIMEOUT_NONE,
		 MACH_PORT_NULL);
    }

    return (void *)0;  /* not reached */
}

/* ===================================================================
 * Intra-task RPC benchmark
 *
 * Sends `iters` messages of `send_size` bytes to the echo cthread
 * and waits for the reply each time.
 * =================================================================== */

static void
bench_intra_rpc(const char *label, int send_size, int iters)
{
    cache_snap_t	cs_before, cs_after;
    mach_port_t		echo_port, reply_port;
    kern_return_t	kr;
    tvalspec_t		t0, t1;
    int			i;
    bench_recv_buf_t	send_buf;
    bench_null_msg_t	recv_buf;

    /* Create echo port and reply port */
    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &echo_port);
    if (kr) { printf("  %s: port alloc failed %d\n", label, kr); return; }

    kr = mach_port_insert_right(mach_task_self(),
				echo_port, echo_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert right failed %d\n", label, kr); return; }

    reply_port = mach_reply_port();

    /* Spawn echo cthread */
    cthread_detach(cthread_fork(echo_thread_func,
				(void *)(unsigned long)echo_port));

    /* Let echo thread start up */
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /* Warmup */
    for (i = 0; i < WARMUP_ITERS; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	send_buf.head.msgh_size	      = send_size;
	send_buf.head.msgh_remote_port = echo_port;
	send_buf.head.msgh_local_port  = reply_port;
	send_buf.head.msgh_id	      = 1;

	mach_msg(&send_buf.head, MACH_SEND_MSG, send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    /* Timed run */
    cache_snap(&cs_before);
    get_time(&t0);
    for (i = 0; i < iters; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	send_buf.head.msgh_size	      = send_size;
	send_buf.head.msgh_remote_port = echo_port;
	send_buf.head.msgh_local_port  = reply_port;
	send_buf.head.msgh_id	      = 1;

	mach_msg(&send_buf.head, MACH_SEND_MSG, send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);
    cache_snap(&cs_after);

    print_result(label, elapsed_ns(&t0, &t1), iters);
    print_cache_stats(label, &cs_before, &cs_after);

    /* Cleanup: destroy ports (kills echo thread's receive) */
    mach_port_destroy(mach_task_self(), echo_port);
    mach_port_destroy(mach_task_self(), reply_port);
}

/* ===================================================================
 * Inter-task: child echo entry point
 *
 * This function runs in the CHILD task.  It must not call any
 * MIG routine (bootstrap_ports, clock_get_time, etc.) — only raw
 * mach_msg() which is a direct syscall wrapper.
 *
 * Port names CHILD_RECV_NAME and CHILD_SEND_NAME are inserted
 * into the child's IPC space by the parent before thread resume.
 * =================================================================== */

static void __attribute__((noreturn, used))
child_echo_entry(void)
{
    bench_recv_buf_t	msg;
    bench_null_msg_t	reply;

    for (;;) {
	mach_msg(&msg.head,
		 MACH_RCV_MSG,
		 0,
		 sizeof(msg),
		 CHILD_RECV_NAME,
		 MACH_MSG_TIMEOUT_NONE,
		 MACH_PORT_NULL);

	reply.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	reply.head.msgh_size	    = sizeof(reply);
	reply.head.msgh_remote_port = CHILD_SEND_NAME;
	reply.head.msgh_local_port  = MACH_PORT_NULL;
	reply.head.msgh_id	    = msg.head.msgh_id + 100;

	mach_msg(&reply.head,
		 MACH_SEND_MSG,
		 sizeof(reply),
		 0,
		 MACH_PORT_NULL,
		 MACH_MSG_TIMEOUT_NONE,
		 MACH_PORT_NULL);
    }
}

/* ===================================================================
 * Inter-task RPC benchmark
 *
 * Creates a child task via task_create(inherit_memory=TRUE),
 * inserts ports into the child's IPC space, creates a thread
 * in the child pointing at child_echo_entry(), and measures
 * round-trip IPC latency.
 * =================================================================== */

static void
bench_inter_rpc(const char *label, int send_size, int iters)
{
    cache_snap_t		cs_before, cs_after;
    kern_return_t		kr;
    mach_port_t			child_task, child_thread;
    mach_port_t			child_recv_port;	/* child receives */
    mach_port_t			parent_recv_port;	/* parent receives replies */
    vm_offset_t			child_stack;
    struct i386_thread_state	state;
    mach_msg_type_number_t	state_count;
    tvalspec_t			t0, t1;
    int				i;
    bench_recv_buf_t		send_buf;
    bench_null_msg_t		recv_buf;

    /*
     * Step 1: Allocate ports.
     * child_recv_port: parent will move the receive right to child.
     * parent_recv_port: parent keeps the receive right for replies.
     */
    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE,
			    &child_recv_port);
    if (kr) { printf("  %s: alloc child_recv failed %d\n", label, kr); return; }

    /* Create a send right for parent to keep */
    kr = mach_port_insert_right(mach_task_self(),
				child_recv_port, child_recv_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert send right failed %d\n", label, kr); return; }

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE,
			    &parent_recv_port);
    if (kr) { printf("  %s: alloc parent_recv failed %d\n", label, kr); return; }

    /* Send right for parent_recv_port (will give to child) */
    kr = mach_port_insert_right(mach_task_self(),
				parent_recv_port, parent_recv_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert parent send right failed %d\n", label, kr); return; }

    /*
     * Step 2: Create child task.
     * inherit_memory=TRUE: child gets a COW copy of our address space,
     * so child_echo_entry() code is available at the same address.
     */
    kr = task_create(mach_task_self(),
		     (ledger_port_array_t)0, 0,
		     TRUE,  /* inherit memory */
		     &child_task);
    if (kr) { printf("  %s: task_create failed %d\n", label, kr); return; }

    /*
     * Step 3: Allocate stack in the child task's address space.
     */
    child_stack = 0;
    kr = vm_allocate(child_task, &child_stack, CHILD_STACK_SIZE, TRUE);
    if (kr) { printf("  %s: child stack alloc failed %d\n", label, kr); return; }

    /*
     * Step 4: Insert port rights into child's IPC space.
     *
     * CHILD_RECV_NAME: receive right — child receives benchmark
     *                  messages on this port.
     * CHILD_SEND_NAME: send right — child sends replies to parent
     *                  on this port.
     */
    kr = mach_port_insert_right(child_task,
				CHILD_RECV_NAME,
				child_recv_port,
				MACH_MSG_TYPE_MOVE_RECEIVE);
    if (kr) { printf("  %s: insert child recv failed %d\n", label, kr); return; }
    /* Parent lost the receive right; still has send right */

    kr = mach_port_insert_right(child_task,
				CHILD_SEND_NAME,
				parent_recv_port,
				MACH_MSG_TYPE_COPY_SEND);
    if (kr) { printf("  %s: insert child send failed %d\n", label, kr); return; }

    /*
     * Step 5: Create thread in child and set i386 registers.
     */
    kr = thread_create(child_task, &child_thread);
    if (kr) { printf("  %s: thread_create failed %d\n", label, kr); return; }

    state_count = i386_THREAD_STATE_COUNT;
    kr = thread_get_state(child_thread, i386_THREAD_STATE,
			  (thread_state_t)&state, &state_count);
    if (kr) { printf("  %s: get_state failed %d\n", label, kr); return; }

    state.eip  = (unsigned int)child_echo_entry;
    state.uesp = (unsigned int)(child_stack + CHILD_STACK_SIZE);

    kr = thread_set_state(child_thread, i386_THREAD_STATE,
			  (thread_state_t)&state, i386_THREAD_STATE_COUNT);
    if (kr) { printf("  %s: set_state failed %d\n", label, kr); return; }

    /*
     * Step 6: Start child thread.
     */
    kr = thread_resume(child_thread);
    if (kr) { printf("  %s: thread_resume failed %d\n", label, kr); return; }

    /* Let child settle */
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /*
     * Step 7: Warmup.
     */
    for (i = 0; i < WARMUP_ITERS; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	send_buf.head.msgh_size	      = send_size;
	send_buf.head.msgh_remote_port = child_recv_port;
	send_buf.head.msgh_local_port  = MACH_PORT_NULL;
	send_buf.head.msgh_id	      = 1;

	mach_msg(&send_buf.head, MACH_SEND_MSG, send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 parent_recv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    /*
     * Step 8: Timed run.
     */
    cache_snap(&cs_before);
    get_time(&t0);
    for (i = 0; i < iters; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	send_buf.head.msgh_size	      = send_size;
	send_buf.head.msgh_remote_port = child_recv_port;
	send_buf.head.msgh_local_port  = MACH_PORT_NULL;
	send_buf.head.msgh_id	      = 1;

	mach_msg(&send_buf.head, MACH_SEND_MSG, send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 parent_recv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);
    cache_snap(&cs_after);

    print_result(label, elapsed_ns(&t0, &t1), iters);
    print_cache_stats(label, &cs_before, &cs_after);

    /*
     * Step 9: Cleanup — destroy the child task.
     */
    task_terminate(child_task);
    mach_port_deallocate(mach_task_self(), child_thread);
    mach_port_deallocate(mach_task_self(), child_task);
    mach_port_destroy(mach_task_self(), child_recv_port);
    mach_port_destroy(mach_task_self(), parent_recv_port);
}

/* ===================================================================
 * Slow-path receive benchmark
 *
 * Guarantees the continuation path is exercised on every iteration.
 *
 * On NCPUS=1, a thread_switch(YIELD) before each send forces the echo
 * thread to run first, call mach_msg_receive, and block (no message
 * pending).  When the sender then sends, the kernel must wake up the
 * blocked receiver — this is the exact path where mach_msg_receive_
 * continue fires instead of restoring callee-saved registers.
 *
 * Comparing this number against bench_intra_rpc (where the receiver
 * may find the message already queued) isolates the slow-path cost.
 * The difference between builds with and without continuations shows
 * the continuation benefit directly.
 * =================================================================== */

/*
 * Echo thread used by the slow-path benchmark.
 * Identical to echo_thread_func but name distinguishes it for clarity.
 */
static void *
slow_echo_thread_func(void *arg)
{
    mach_port_t		port = (mach_port_t)(unsigned long)arg;
    bench_recv_buf_t	msg;
    bench_null_msg_t	reply;
    kern_return_t	kr;

    for (;;) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,
		      sizeof(msg),
		      port,
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS)
	    continue;

	reply.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	reply.head.msgh_size	    = sizeof(reply);
	reply.head.msgh_remote_port = msg.head.msgh_remote_port;
	reply.head.msgh_local_port  = MACH_PORT_NULL;
	reply.head.msgh_id	    = msg.head.msgh_id + 100;

	mach_msg(&reply.head,
		 MACH_SEND_MSG,
		 sizeof(reply),
		 0,
		 MACH_PORT_NULL,
		 MACH_MSG_TIMEOUT_NONE,
		 MACH_PORT_NULL);
    }
    return (void *)0;
}

/*
 * bench_slow_receive:
 *
 * Measures wakeup-from-blocked latency.  Before each send, yields the
 * CPU so the echo thread runs and enters mach_msg_receive (blocking).
 * The measured time is: yield + send + wakeup-via-continuation +
 * echo-reply + receive-reply.
 *
 * The yield overhead is small (~0.1 us) and constant across builds.
 * The wakeup cost is what changes: with continuations, the receiver
 * resumes without restoring callee-saved registers (~50-100 cycles
 * faster than without).
 */
static void
bench_slow_receive(const char *label, int send_size, int iters)
{
    mach_port_t		echo_port, reply_port;
    kern_return_t	kr;
    tvalspec_t		t0, t1;
    int			i;
    bench_recv_buf_t	send_buf;
    bench_recv_buf_t	recv_buf;

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &echo_port);
    if (kr) { printf("  %s: port alloc failed %d\n", label, kr); return; }

    kr = mach_port_insert_right(mach_task_self(),
				echo_port, echo_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert right failed %d\n", label, kr); return; }

    reply_port = mach_reply_port();

    cthread_detach(cthread_fork(slow_echo_thread_func,
				(void *)(unsigned long)echo_port));

    /* Let echo thread start and block for the first time */
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);

    /* Warmup — same yield-send-recv pattern */
    for (i = 0; i < WARMUP_ITERS; i++) {
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);

	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	send_buf.head.msgh_size	      = send_size;
	send_buf.head.msgh_remote_port = echo_port;
	send_buf.head.msgh_local_port  = reply_port;
	send_buf.head.msgh_id	      = 1;

	mach_msg(&send_buf.head, MACH_SEND_MSG, send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    /* Timed run */
    get_time(&t0);
    for (i = 0; i < iters; i++) {
	/*
	 * Yield CPU: echo thread runs, calls mach_msg_receive, blocks.
	 * On return here the echo thread is guaranteed to be sleeping.
	 */
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);

	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	send_buf.head.msgh_size	      = send_size;
	send_buf.head.msgh_remote_port = echo_port;
	send_buf.head.msgh_local_port  = reply_port;
	send_buf.head.msgh_id	      = 1;

	/* Send wakes the blocked echo thread — continuation fires here */
	mach_msg(&send_buf.head, MACH_SEND_MSG, send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);

    print_result(label, elapsed_ns(&t0, &t1), iters);

    mach_port_destroy(mach_task_self(), echo_port);
    mach_port_destroy(mach_task_self(), reply_port);
}

/* ===================================================================
 * Port-operation benchmarks
 * =================================================================== */

static void
bench_port_alloc_destroy(int iters)
{
    tvalspec_t		t0, t1;
    mach_port_t		port;
    int			i;

    /* Warmup */
    for (i = 0; i < WARMUP_ITERS; i++) {
	mach_port_allocate(mach_task_self(),
			   MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_destroy(mach_task_self(), port);
    }

    get_time(&t0);
    for (i = 0; i < iters; i++) {
	mach_port_allocate(mach_task_self(),
			   MACH_PORT_RIGHT_RECEIVE, &port);
	mach_port_destroy(mach_task_self(), port);
    }
    get_time(&t1);

    print_result("port alloc + destroy", elapsed_ns(&t0, &t1), iters);
}

static void
bench_port_names(int iters)
{
    tvalspec_t			t0, t1;
    mach_port_array_t		names;
    mach_msg_type_number_t	names_count;
    mach_port_type_array_t	types;
    mach_msg_type_number_t	types_count;
    int				i;

    /* Warmup */
    for (i = 0; i < WARMUP_ITERS; i++) {
	mach_port_names(mach_task_self(),
			&names, &names_count,
			&types, &types_count);
	vm_deallocate(mach_task_self(), (vm_offset_t)names,
		      names_count * sizeof(*names));
	vm_deallocate(mach_task_self(), (vm_offset_t)types,
		      types_count * sizeof(*types));
    }

    get_time(&t0);
    for (i = 0; i < iters; i++) {
	mach_port_names(mach_task_self(),
			&names, &names_count,
			&types, &types_count);
	vm_deallocate(mach_task_self(), (vm_offset_t)names,
		      names_count * sizeof(*names));
	vm_deallocate(mach_task_self(), (vm_offset_t)types,
		      types_count * sizeof(*types));
    }
    get_time(&t1);

    print_result("mach_port_names()", elapsed_ns(&t0, &t1), iters);
}

/* ===================================================================
 * OOL (out-of-line) data benchmarks
 *
 * Sends a message with one OOL descriptor of the given size using
 * MACH_MSG_PHYSICAL_COPY.  The echo thread receives the OOL data
 * (kernel maps it into the receiver's address space), deallocates
 * it, and sends a null reply.
 *
 * This exercises the copyin path in ipc_kmsg_copyin_body() and the
 * copyout path in ipc_kmsg_copyout_body().  With the zero-copy
 * optimisation, large OOL regions use vm_map_copyin() with COW
 * instead of a physical copyin + vm_map_copyin(kernel_copy_map).
 * =================================================================== */

/*
 * OOL message — one out-of-line region.
 */
typedef struct {
    mach_msg_header_t		head;
    mach_msg_body_t		body;
    mach_msg_ool_descriptor_t	ool;
} bench_ool_send_msg_t;

typedef struct {
    mach_msg_header_t		head;
    mach_msg_body_t		body;
    mach_msg_ool_descriptor_t	ool;
    mach_msg_trailer_t		trailer;
} bench_ool_recv_msg_t;

/*
 * Echo thread for OOL messages: receives OOL data, deallocates it,
 * sends a null reply.
 */
static void *
ool_echo_thread_func(void *arg)
{
    mach_port_t		port = (mach_port_t)(unsigned long)arg;
    bench_ool_recv_msg_t msg;
    bench_null_msg_t	reply;
    kern_return_t	kr;

    for (;;) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,
		      sizeof(msg),
		      port,
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS)
	    continue;

	/* Deallocate received OOL data to avoid leaking memory */
	if ((msg.head.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
	    msg.body.msgh_descriptor_count == 1 &&
	    msg.ool.address != 0 && msg.ool.size > 0) {
	    vm_deallocate(mach_task_self(),
			  (vm_offset_t)msg.ool.address,
			  msg.ool.size);
	}

	reply.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	reply.head.msgh_size	    = sizeof(reply);
	reply.head.msgh_remote_port = msg.head.msgh_remote_port;
	reply.head.msgh_local_port  = MACH_PORT_NULL;
	reply.head.msgh_id	    = msg.head.msgh_id + 100;

	mach_msg(&reply.head,
		 MACH_SEND_MSG,
		 sizeof(reply),
		 0,
		 MACH_PORT_NULL,
		 MACH_MSG_TIMEOUT_NONE,
		 MACH_PORT_NULL);
    }
    return (void *)0;
}

#define OOL_BENCH_ITERS		5000

static void
bench_ool_intra_rpc(const char *label, vm_size_t ool_size, int iters)
{
    mach_port_t		echo_port, reply_port;
    kern_return_t	kr;
    tvalspec_t		t0, t1;
    int			i;
    bench_ool_send_msg_t send_buf;
    bench_null_msg_t	recv_buf;
    vm_offset_t		data_buf;

    /* Allocate OOL data buffer */
    kr = vm_allocate(mach_task_self(), &data_buf, ool_size, TRUE);
    if (kr) { printf("  %s: vm_allocate failed %d\n", label, kr); return; }

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &echo_port);
    if (kr) { printf("  %s: port alloc failed %d\n", label, kr); return; }
    kr = mach_port_insert_right(mach_task_self(),
				echo_port, echo_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert right failed %d\n", label, kr); return; }
    reply_port = mach_reply_port();

    cthread_detach(cthread_fork(ool_echo_thread_func,
				(void *)(unsigned long)echo_port));
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /* Warmup */
    for (i = 0; i < WARMUP_ITERS; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE) |
	    MACH_MSGH_BITS_COMPLEX;
	send_buf.head.msgh_size	      = sizeof(send_buf);
	send_buf.head.msgh_remote_port = echo_port;
	send_buf.head.msgh_local_port  = reply_port;
	send_buf.head.msgh_id	      = 1;
	send_buf.body.msgh_descriptor_count = 1;
	send_buf.ool.address     = (void *)data_buf;
	send_buf.ool.size        = ool_size;
	send_buf.ool.deallocate  = FALSE;
	send_buf.ool.copy        = MACH_MSG_PHYSICAL_COPY;
	send_buf.ool.type        = MACH_MSG_OOL_DESCRIPTOR;

	mach_msg(&send_buf.head, MACH_SEND_MSG, sizeof(send_buf), 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    /* Timed run */
    get_time(&t0);
    for (i = 0; i < iters; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE) |
	    MACH_MSGH_BITS_COMPLEX;
	send_buf.head.msgh_size	      = sizeof(send_buf);
	send_buf.head.msgh_remote_port = echo_port;
	send_buf.head.msgh_local_port  = reply_port;
	send_buf.head.msgh_id	      = 1;
	send_buf.body.msgh_descriptor_count = 1;
	send_buf.ool.address     = (void *)data_buf;
	send_buf.ool.size        = ool_size;
	send_buf.ool.deallocate  = FALSE;
	send_buf.ool.copy        = MACH_MSG_PHYSICAL_COPY;
	send_buf.ool.type        = MACH_MSG_OOL_DESCRIPTOR;

	mach_msg(&send_buf.head, MACH_SEND_MSG, sizeof(send_buf), 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);

    print_result(label, elapsed_ns(&t0, &t1), iters);

    vm_deallocate(mach_task_self(), data_buf, ool_size);
    mach_port_destroy(mach_task_self(), echo_port);
    mach_port_destroy(mach_task_self(), reply_port);
}

/* ===================================================================
 * Main
 * =================================================================== */

int
main(int argc, char **argv)
{
    kern_return_t	kr;
    int			i;

    /*
     * Step 1: Get privileged ports.
     */
    kr = bootstrap_ports(bootstrap_port,
			 &host_port,
			 &device_port,
			 &root_ledger_wired,
			 &root_ledger_paged,
			 &security_port);
    if (kr != KERN_SUCCESS)
	_exit(1);

    /*
     * Step 2: Initialize console + panic.
     */
    printf_init(device_port);
    panic_init(host_port);

    printf("\n");
    printf("=== OSFMK IPC Performance Benchmark ===\n");
    printf("    %d iterations per test, %d warmup\n", BENCH_ITERS, WARMUP_ITERS);

    {
	host_ipc_cache_info_data_t	cinfo;
	mach_msg_type_number_t		cc = HOST_IPC_CACHE_INFO_COUNT;
	if (host_info(host_port, HOST_IPC_CACHE_INFO,
		      (host_info_t)&cinfo, &cc) == KERN_SUCCESS)
	    printf("    kmsg cache: %u slots/CPU, max msg size %u bytes\n\n",
		   cinfo.stash, cinfo.saved_size);
	else
	    printf("    kmsg cache: stats unavailable\n\n");
    }

    /*
     * Step 3: Get clock for timing.
     */
    kr = host_get_clock_service(host_port, REALTIME_CLOCK, &clock_port);
    if (kr != KERN_SUCCESS) {
	printf("(ipc_bench): clock_get failed: %d\n", kr);
	_exit(1);
    }

    /* ---------------------------------------------------------
     * Intra-task benchmarks (thread-to-thread, same address space)
     * --------------------------------------------------------- */
    printf("--- Intra-task (thread-to-thread) ---\n");

    bench_intra_rpc("null RPC",
		    (int)sizeof(bench_null_msg_t), BENCH_ITERS);
    bench_intra_rpc("128B inline RPC",
		    (int)sizeof(bench_128_msg_t), BENCH_ITERS);
    bench_intra_rpc("1024B inline RPC",
		    (int)sizeof(bench_1024_msg_t), BENCH_ITERS);
    bench_intra_rpc("4096B inline RPC",
		    (int)sizeof(bench_4096_msg_t), BENCH_ITERS);

    printf("\n");

    /* ---------------------------------------------------------
     * Slow-path receive: receiver always blocked before send.
     * This is the path exercised by mach_msg_receive_continue.
     * A thread_switch(YIELD) before each send guarantees the echo
     * thread is sleeping in mach_msg_receive when the send fires.
     * --------------------------------------------------------- */
    printf("--- Slow-path receive (continuation path) ---\n");

    bench_slow_receive("null RPC (receiver blocked)",
		       (int)sizeof(bench_null_msg_t), BENCH_ITERS);
    bench_slow_receive("128B inline RPC (receiver blocked)",
		       (int)sizeof(bench_128_msg_t), BENCH_ITERS);
    bench_slow_receive("1024B inline RPC (receiver blocked)",
		       (int)sizeof(bench_1024_msg_t), BENCH_ITERS);
    bench_slow_receive("4096B inline RPC (receiver blocked)",
		       (int)sizeof(bench_4096_msg_t), BENCH_ITERS);

    printf("\n");

    /* ---------------------------------------------------------
     * Inter-task benchmarks (task-to-task, separate address spaces)
     * --------------------------------------------------------- */
    printf("--- Inter-task (task-to-task) ---\n");

    bench_inter_rpc("null RPC",
		    (int)sizeof(bench_null_msg_t), BENCH_ITERS);
    bench_inter_rpc("128B inline RPC",
		    (int)sizeof(bench_128_msg_t), BENCH_ITERS);
    bench_inter_rpc("1024B inline RPC",
		    (int)sizeof(bench_1024_msg_t), BENCH_ITERS);
    bench_inter_rpc("4096B inline RPC",
		    (int)sizeof(bench_4096_msg_t), BENCH_ITERS);

    printf("\n");

    /* ---------------------------------------------------------
     * Port operation benchmarks
     * --------------------------------------------------------- */
    printf("--- Port operations ---\n");

    bench_port_alloc_destroy(BENCH_ITERS);
    bench_port_names(BENCH_ITERS);

    /* ---------------------------------------------------------
     * OOL (out-of-line) data benchmarks
     * Measures OOL copyin/copyout cost for large data regions.
     * Uses MACH_MSG_PHYSICAL_COPY — exercises the zero-copy COW path
     * for regions >= MSG_OOL_SIZE_SMALL (2049 bytes).
     * --------------------------------------------------------- */
    printf("--- OOL data (intra-task, PHYSICAL_COPY) ---\n");

    bench_ool_intra_rpc("4 KB OOL",   4096, OOL_BENCH_ITERS);
    bench_ool_intra_rpc("16 KB OOL", 16384, OOL_BENCH_ITERS);
    bench_ool_intra_rpc("64 KB OOL", 65536, OOL_BENCH_ITERS);

    printf("\n");
    printf("=== Benchmark complete ===\n");

    /*
     * Tell bootstrap we're done.
     */
    bootstrap_completed(bootstrap_port, mach_task_self());

    /*
     * Idle — don't exit, just sleep.
     */
    for (;;)
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1000);

    return 0;
}
