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
#include <mach/mach_syscalls.h>	/* syscall_thread_switch */
#include <mach/machine/thread_status.h>	/* #552: this machine's, not i386's */
#include <mach/machine/port_name.h>	/* #552: MACH_PORT_GEN_BITS is the target's */
#include <sa_mach.h>
#include <pthread.h>
#include <device/device.h>
#include <device/device_types.h>
#include "gpu_console.h"
#include <stdio.h>
#include <string.h>

#include "disk_bench.h"
#include "mem_bench.h"
#include "flipc2_bench.h"

/* MIG stub — generated from mach_port.defs */
extern kern_return_t mach_port_set_protected_payload(
	mach_port_t task, mach_port_t name, unsigned payload);

/* Raw syscall traps: declared in <mach/mach_traps.h>, included above (#426). */

/* ===================================================================
 * Configuration
 * =================================================================== */

#define WARMUP_ITERS		100
#define BENCH_ITERS		10000
#define CHILD_STACK_SIZE	(64 * 1024)	/* 64 KB */

/*
 * Well-known port names inserted into the child task's IPC space for the
 * inter-task benchmark.
 *
 * 🔴 BUILT FROM AN INDEX, NOT WRITTEN AS A NUMBER (#552).
 *
 * They used to be the literals 0x1503 and 0x1603.  A port name is an index and
 * a generation packed into one word, and how wide the generation is belongs to
 * the target: eight bits on i386, TEN on x86-64 (#413 widened it, because a name
 * and the entry bits that hold it must be the same width).  So those two
 * literals are indices 21 and 22 on i386 -- two entries -- and index 5 and index
 * 5 on x86-64, the SAME ENTRY.  The second insert answered KERN_NAME_EXISTS and
 * the whole inter-task suite printed nothing but an error, on every size.
 *
 * 🔑 The assertion below is the point, not the fix.  Deriving the names from
 * indices makes them right on both targets today; the _Static_assert makes a
 * collision IMPOSSIBLE to reintroduce on the next target, where the field may be
 * a third width.  A comment saying "keep these apart" would have been read and
 * believed, exactly like the one that said to revisit an array bound "if other
 * flavors are added" while a word size broke it.
 */
#define CHILD_RECV_INDEX	0x15
#define CHILD_SEND_INDEX	0x16
#define CHILD_RECV_NAME		((mach_port_t) MACH_PORT_MAKE(CHILD_RECV_INDEX, 0))
#define CHILD_SEND_NAME		((mach_port_t) MACH_PORT_MAKE(CHILD_SEND_INDEX, 0))

_Static_assert(MACH_PORT_INDEX(CHILD_RECV_NAME)
	    != MACH_PORT_INDEX(CHILD_SEND_NAME),
	"the two child port names land on one entry: this target's generation "
	"field is wide enough to swallow the difference between the indices");

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
 * SMP-correct echo-thread readiness handshake.
 *
 * The old code "synchronised" by calling thread_switch(YIELD/DEPRESS)
 * after pthread_create and assuming the echo thread had, by the time the
 * yield returned, started and blocked in mach_msg_receive.  That is a
 * uniprocessor assumption: on SMP the echo thread runs on another CPU
 * and the yield guarantees nothing.  Instead, the echo thread sends a
 * one-shot ping on a ready port as its very first action; spawn_echo()
 * blocks receiving it, so the benchmark loop only starts once the echo
 * thread is provably running.  Benches run sequentially, so a single
 * global ready port (live only across the handshake) suffices and lets
 * the four echo funcs keep their (void*)port argument unchanged.
 * =================================================================== */

static volatile mach_port_t g_echo_ready_port = MACH_PORT_NULL;

/* echo side: announce "I'm running" (call once at thread entry). */
static void
echo_signal_ready(void)
{
    mach_port_t		rp = g_echo_ready_port;
    mach_msg_header_t	m;

    if (rp == MACH_PORT_NULL)
	return;
    m.msgh_bits	       = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    m.msgh_size	       = sizeof(m);
    m.msgh_remote_port = rp;
    m.msgh_local_port  = MACH_PORT_NULL;
    m.msgh_id	       = 0;
    (void) mach_msg(&m, MACH_SEND_MSG, sizeof(m), 0,
		    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
}

/* spawner side: create the echo thread and block until it is running.
 * Replaces "pthread_create + pthread_detach + thread_switch(yield)".
 * The 5 s timeout turns a libpthreads lost-wakeup (echo never scheduled)
 * into an explicit log line instead of a silent wedge — a clean signal
 * to distinguish a benchmark sync bug from a real threading bug. */
static void
spawn_echo(const char *label, void *(*fn)(void *), mach_port_t echo_port)
{
    pthread_t		th;
    mach_port_t		ready_port;
    /* Receive buffer must hold the header AND the trailer the kernel
     * appends, plus slack — receiving into a bare header overflows the
     * stack (the stack protector trips: "stack smashing detected"). */
    struct {
	mach_msg_header_t  head;
	mach_msg_trailer_t trailer;
	char		   pad[32];
    } m;
    kern_return_t	kr;

    if (mach_port_allocate(mach_task_self(),
			   MACH_PORT_RIGHT_RECEIVE, &ready_port)) {
	/* Fall back to the old best-effort yield if we can't make a port. */
	pthread_create(&th, NULL, fn, (void *)(unsigned long)echo_port);
	pthread_detach(th);
	thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);
	return;
    }
    mach_port_insert_right(mach_task_self(), ready_port, ready_port,
			   MACH_MSG_TYPE_MAKE_SEND);
    g_echo_ready_port = ready_port;

    pthread_create(&th, NULL, fn, (void *)(unsigned long)echo_port);
    pthread_detach(th);

    kr = mach_msg(&m.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(m),
		  ready_port, 5000, MACH_PORT_NULL);
    if (kr == MACH_RCV_TIMED_OUT)
	printf("  %s: echo thread failed to start in 5s "
	       "(libpthreads lost wakeup?)\n", label);

    g_echo_ready_port = MACH_PORT_NULL;
    mach_port_destroy(mach_task_self(), ready_port);
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

    echo_signal_ready();

    for (;;) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,
		      sizeof(msg),
		      port,
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
	    /*
	     * Terminal errors mean the bench destroyed the receive port
	     * — exit instead of spinning on mach_msg(RCV) forever and
	     * polluting the next test's scheduling.
	     */
	    if (kr == MACH_RCV_PORT_DIED ||
		kr == MACH_RCV_PORT_CHANGED ||
		kr == MACH_RCV_INVALID_NAME ||
		kr == MACH_RCV_TIMED_OUT)
		break;
	    continue;
	}

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
    cache_snap_t	cs_before = { 0 }, cs_after = { 0 };
    int			cs_ok;
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

    /* Spawn echo thread and wait until it is provably running (SMP-safe). */
    spawn_echo(label, echo_thread_func, echo_port);

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
    cs_ok = cache_snap(&cs_before);
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
    cs_ok &= cache_snap(&cs_after);

    print_result(label, elapsed_ns(&t0, &t1), iters);
    /*
     * 🔴 ONLY IF BOTH SNAPSHOTS ANSWERED (#569).  cache_snap() returns 0 when
     * host_info refuses and leaves the struct untouched; the print was
     * commented out, so nobody had ever noticed that its input could be
     * whatever was on the stack.  Enabling the line is what made the compiler
     * say so.
     */
    if (cs_ok)
	print_cache_stats(label, &cs_before, &cs_after);

    /* Cleanup: destroy ports (kills echo thread's receive) */
    mach_port_destroy(mach_task_self(), echo_port);
    mach_port_destroy(mach_task_self(), reply_port);
}

/* ===================================================================
 * Combined-trap (SEND|RCV) echo thread  [#320]
 *
 * MIG-server-style loop: the first iteration only receives; thereafter
 * each iteration SENDS the previous reply AND RECEIVES the next request
 * in a single mach_msg() trap.  The server therefore parks on TH_WAIT
 * between requests, so both peers take the mach_msg combined-message
 * hotpath (the L4-style hand-off) rather than the separate-call DTS path.
 * =================================================================== */

static void *
combined_echo_thread_func(void *arg)
{
    mach_port_t		port = (mach_port_t)(unsigned long)arg;
    bench_recv_buf_t	buf;
    kern_return_t	kr;
    mach_msg_option_t	opt	= MACH_RCV_MSG;	/* first time: receive only */
    mach_msg_size_t	send_sz = 0;

    echo_signal_ready();

    for (;;) {
	kr = mach_msg(&buf.head, opt, send_sz, sizeof(buf), port,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
	    if (kr == MACH_RCV_PORT_DIED  || kr == MACH_RCV_PORT_CHANGED ||
		kr == MACH_RCV_INVALID_NAME || kr == MACH_RCV_TIMED_OUT  ||
		kr == MACH_SEND_INVALID_DEST)
		break;
	    opt = MACH_RCV_MSG; send_sz = 0;	/* resync to a clean receive */
	    continue;
	}

	/*
	 * buf holds a request; buf.head.msgh_remote_port is the client's
	 * send-once reply right.  Turn the buffer into a null reply to it,
	 * then SEND it and RCV the next request in one trap.
	 */
	buf.head.msgh_bits	 = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	buf.head.msgh_local_port = MACH_PORT_NULL;
	buf.head.msgh_size	 = sizeof(bench_null_msg_t);
	buf.head.msgh_id	+= 100;
	opt	= MACH_SEND_MSG | MACH_RCV_MSG;
	send_sz = sizeof(bench_null_msg_t);
    }
    return (void *)0;
}

/* ===================================================================
 * Combined-trap intra-task RPC benchmark  [#320]
 *
 * The client issues one mach_msg(MACH_SEND_MSG|MACH_RCV_MSG) per round
 * trip (send the request to the echo port, receive the reply on its own
 * reply port) -- exactly what a MIG simpleroutine does.  This is the path
 * that exercises the SMP-safe hotpath (#315); bench_intra_rpc, by using
 * separate send/recv calls, does not.
 * =================================================================== */

static void
bench_combined_rpc(const char *label, int send_size, int iters)
{
    mach_port_t		echo_port, reply_port;
    kern_return_t	kr;
    tvalspec_t		t0, t1;
    int			i;
    bench_recv_buf_t	buf;	/* one buffer: send from it, receive into it */

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &echo_port);
    if (kr) { printf("  %s: port alloc failed %d\n", label, kr); return; }
    kr = mach_port_insert_right(mach_task_self(), echo_port, echo_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert right failed %d\n", label, kr); return; }
    reply_port = mach_reply_port();

    /* Spawn echo thread and wait until it is provably running (SMP-safe). */
    spawn_echo(label, combined_echo_thread_func, echo_port);

    for (i = 0; i < WARMUP_ITERS; i++) {
	buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	buf.head.msgh_size	  = send_size;
	buf.head.msgh_remote_port = echo_port;
	buf.head.msgh_local_port  = reply_port;
	buf.head.msgh_id	  = 1;
	mach_msg(&buf.head, MACH_SEND_MSG | MACH_RCV_MSG, send_size,
		 sizeof(buf), reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    get_time(&t0);
    for (i = 0; i < iters; i++) {
	buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	buf.head.msgh_size	  = send_size;
	buf.head.msgh_remote_port = echo_port;
	buf.head.msgh_local_port  = reply_port;
	buf.head.msgh_id	  = 1;
	mach_msg(&buf.head, MACH_SEND_MSG | MACH_RCV_MSG, send_size,
		 sizeof(buf), reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);

    print_result(label, elapsed_ns(&t0, &t1), iters);

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
    cache_snap_t		cs_before = { 0 }, cs_after = { 0 };
    int			cs_ok;
    kern_return_t		kr;
    mach_port_t			child_task, child_thread;
    mach_port_t			child_recv_port;	/* child receives */
    mach_port_t			parent_recv_port;	/* parent receives replies */
    vm_offset_t			child_stack;
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
     * Step 5: Create thread in child and give it a pc and a stack.
     */
    kr = thread_create(child_task, &child_thread);
    if (kr) { printf("  %s: thread_create failed %d\n", label, kr); return; }

    kr = bench_child_thread_start(child_thread, child_echo_entry,
				  child_stack + CHILD_STACK_SIZE);
    if (kr) { printf("  %s: child thread start failed %d\n", label, kr); return; }

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
    cs_ok = cache_snap(&cs_before);
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
    cs_ok &= cache_snap(&cs_after);

    print_result(label, elapsed_ns(&t0, &t1), iters);
    /*
     * 🔴 ONLY IF BOTH SNAPSHOTS ANSWERED (#569).  cache_snap() returns 0 when
     * host_info refuses and leaves the struct untouched; the print was
     * commented out, so nobody had ever noticed that its input could be
     * whatever was on the stack.  Enabling the line is what made the compiler
     * say so.
     */
    if (cs_ok)
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
 * Inter-task RPC swept by size, repeated inside one boot (#446)
 *
 * THE QUESTION: is a message SIZE slow, or is the BOOT slow?
 *
 * The four-size inter table cannot answer it.  #446 measured eleven boots
 * of this path and found it bimodal, the fast mode appearing twice in the
 * eleven with a 27% gap to the median -- so "1024B read high on three boots
 * out of three" is three draws from a distribution whose slow mode is the
 * common one, and the same three boots would be unremarkable under a model
 * where the size means nothing at all.  Adding boots dilutes that slowly and
 * expensively: the lottery is decided once per boot, so a boot buys ONE
 * sample of it no matter how many iterations the row averages over.
 *
 * So the control goes INSIDE the boot.  Every size is measured REPS times in
 * the one boot, which holds the clock, the governor, the bundle and the
 * scheduler's history identical across the repeats by construction rather
 * than by assertion -- the four axes a cross-boot comparison has to name are
 * not even variables here.  Neighbours 768B and 1280B bracket the size under
 * suspicion, so a threshold has something to be a threshold BETWEEN.
 *
 * The two outcomes are different pictures, which is what makes this a
 * measurement and not another sample:
 *
 *   - a size that is intrinsically slow reads slow in EVERY repeat, and its
 *     neighbours read fast in every repeat -- a step that stays put;
 *   - a placement lottery scatters -- the same size reads fast in one repeat
 *     and slow in the next, and which size is high moves between repeats.
 *
 * ⚠️ Each measurement builds and tears down its own child task, so a repeat
 * is a fresh draw of whatever the placement decides, not a warm re-run of
 * the previous one.  That is deliberate: the thing under suspicion IS what a
 * fresh pair gets placed on.  It also means this suite creates one task per
 * row, which is why it is asked for by name and is not part of `all'.
 * =================================================================== */

#define	ISWEEP_REPS	3

static void
bench_inter_sweep(int iters)
{
    /*
     * Payload bytes, not message bytes: the label says what the caller
     * asked to send, and the header is added below the way every other
     * inter row adds it (sizeof(bench_1024_msg_t) is header + 1024).
     * 0 is the null RPC, the anchor the rest is read against.
     */
    static const struct {
	int		payload;
	const char	*label;
    } sizes[] = {
	{    0, "payload    0B (null)" },
	{   64, "payload   64B" },
	{  128, "payload  128B" },
	{  256, "payload  256B" },
	{  512, "payload  512B" },
	{  768, "payload  768B" },
	{ 1024, "payload 1024B" },
	{ 1280, "payload 1280B" },
	{ 1536, "payload 1536B" },
	{ 2048, "payload 2048B" },
	{ 3072, "payload 3072B" },
	{ 4096, "payload 4096B" },
    };
    const int	nsizes = (int)(sizeof(sizes) / sizeof(sizes[0]));
    int		rep, i;

    for (rep = 1; rep <= ISWEEP_REPS; rep++) {
	printf("  -- repeat %d of %d --\n", rep, ISWEEP_REPS);
	for (i = 0; i < nsizes; i++) {
	    bench_inter_rpc(sizes[i].label,
			    (int)sizeof(mach_msg_header_t) + sizes[i].payload,
			    iters);
	}
    }
}

/* ===================================================================
 * #558: task_create and task_terminate, concurrently, and nothing else
 *
 * THE DEFECT this exists to provoke: `pmap_destroy()' reaches `pv_remove()'
 * through its own page tables, so it rewrites the pv list of every page those
 * tables map -- pages belonging to arbitrary VM objects, none of them locked.
 * The readers of those same lists (`pmap_page_protect()' from the copy-on-write
 * path) hold the page's OBJECT lock. Two different locks, so no exclusion.
 * Underneath both, `pv_free_list' is a plain global push/pop with no lock and
 * no atomics, so two processors can be handed the same entry.
 *
 * ❌ WHY THE OTHER SUITES DO NOT PROVOKE IT, which is the point of this one.
 * `bench_inter_rpc()' terminates its child and creates the next from ONE
 * thread, in sequence -- and sequential code does not race with itself. Its
 * only overlap is accidental: the dying child's last reference dropped on
 * another processor while this thread is already inside the next fork. Forty
 * boots at `-smp 4', twenty under TCG and twenty under KVM, with `isweep'
 * doing 36 create/destroy pairs per boot instead of four, produced **nothing**.
 * That is not a rate for the defect; it is the shape of the workload.
 *
 * 🔑 So here the overlap IS the test. Every thread is a forker and a destroyer
 * at once, and they all fork from THIS task, so all of them walk the pv lists
 * of the same pages at the same time. No IPC, no child threads, no ports
 * beyond the task port each iteration must release: the shortest path from
 * `task_create()' to `vm_map_fork()' to `pmap_page_protect()', which is
 * exactly the backtrace in the reperto.
 *
 * ⚠️ THE REGION IS NOT DECORATION. An address space with nothing resident in
 * it is forked without `vm_map_fork()' ever reaching a page that has pv
 * entries, and then this provokes nothing while looking busy. The pages are
 * touched so they are resident and indexed.
 * =================================================================== */

/*
 * 🔑 THE WATCHDOG, NOT THE WORK, IS WHAT A BOOT COSTS. Measured on the first
 * run: 1000 concurrent fork-and-destroy pairs take 161 ms, or 161 us each, in
 * a boot whose watchdog is two minutes. Twenty boots of the old workload bought
 * 20,000 forks and seventy minutes; one boot buys 100,000 in sixteen seconds.
 * So the pressure goes here rather than into the boot count -- boots still have
 * their own value, since each one re-rolls the initial conditions a race may
 * depend on, but they are the expensive axis and no longer the only one.
 *
 * ⚠️ MORE THREADS THAN PROCESSORS ON PURPOSE. Eight workers on four CPUs get
 * preempted in the middle of a task_create, which widens the windows this is
 * hunting instead of narrowing them; four would let each worker run its
 * operation to completion far more often.
 */
#ifndef	FORKRACE_THREADS
#define FORKRACE_THREADS	8
#endif
#ifndef	FORKRACE_ITERS
#define FORKRACE_ITERS		12500
#endif
/*
 * 🔥 CHILDREN THAT RUN, AND HOW MANY ARE ALIVE AT ONCE (#558).
 *
 * Zero is the suite above: fork, terminate, release, and the child never runs.
 * That shape forks hard and provokes ALMOST NO pv traffic, which took eight
 * boots with the index's locks ablated to notice: a task that never runs never
 * faults, so its pmap holds no mappings, so `pmap_destroy()' has nothing to
 * take out of any pv list.  The only list long enough to walk was this task's
 * own, and the concurrent REMOVER the defect needs was not there at all.
 *
 * With a count, each worker keeps that many children ALIVE, each with a thread
 * reading the shared region.  Every page of the region then carries one pv
 * entry per living child -- lists of tens rather than of one -- and retiring a
 * child drives pmap_destroy() through all of them while the other children are
 * still faulting them in and every worker's next task_create() is walking the
 * same lists to write-protect them.  Reader, writer and allocator on the same
 * lists at the same time, which is the reperto's shape.
 *
 * ⚠️ A child costs far more than a bare fork (a stack, a thread, its faults),
 * so the iteration count belongs lower when this is on -- both are cache
 * variables for that reason.
 */
#ifndef	FORKRACE_LIVE
#define FORKRACE_LIVE		0
#endif
#define FORKRACE_REGION		(1024 * 1024)
#define FORKRACE_PAGE		4096

typedef struct {
    int		iters;
    unsigned	made;
    unsigned	refused;
    pthread_t	th;
} forkrace_worker_t;

#if	FORKRACE_LIVE > 0
/*
 * The region the children read.  Written once before any of them exists, and
 * inherited by every child at the same address -- task_create(inherit_memory)
 * copies the address space, so a global holds for the child what it held for
 * the parent, which is the same property child_echo_entry() relies on.
 */
static vm_offset_t	forkrace_region_addr;

/*
 * READ, never write.  A write would copy the page and END the sharing this is
 * building: the point is many pmaps pointing at the SAME physical page, which
 * is what makes a pv list long.
 */
/*
 * 🔴 A CHILD ENDS BY ITSELF, and the first version did not.
 *
 * It read the region for ever and waited to be retired.  The run then reached
 * "Benchmark complete" and the machine stayed busy until the watchdog closed
 * it -- census passes pinned, resets climbing -- so the boot never reached an
 * end the harness recognises.  Whatever the reason a child outlived its
 * retirement, a workload that cannot finish cannot be measured, and a backstop
 * that costs a counter is cheaper than a campaign of runs nobody can classify.
 *
 * The parent still retires them; this is what happens when that is not enough.
 */
#define FORKRACE_CHILD_PASSES	200

static void
forkrace_child_entry(void)
{
    volatile const char	*p = (volatile const char *) forkrace_region_addr;
    unsigned long	 off;
    int			 pass;

    for (pass = 0; pass < FORKRACE_CHILD_PASSES; pass++) {
	for (off = 0; off < FORKRACE_REGION; off += FORKRACE_PAGE)
	    (void) p[off];

	/*
	 * Sleep between passes rather than spin: thirty-odd children spinning
	 * on four processors would starve the forkers, and it is the forkers
	 * that drive the walks this is hunting.
	 */
	(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 20);
    }

    (void) task_terminate(mach_task_self());
    for (;;)
	(void) thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 1000);
}

/* One live child: a task, a stack, a thread reading the region.  Answers
 * whether it was made, and leaves only the TASK right held. */
static int
forkrace_spawn_live(mach_port_t *out)
{
    mach_port_t		task, thread;
    vm_offset_t		stack = 0;
    kern_return_t	kr;

    kr = task_create(mach_task_self(), (ledger_port_array_t)0, 0, TRUE, &task);
    if (kr != KERN_SUCCESS)
	return 0;

    kr = vm_allocate(task, &stack, CHILD_STACK_SIZE, TRUE);
    if (kr != KERN_SUCCESS)
	goto undo_task;

    kr = thread_create(task, &thread);
    if (kr != KERN_SUCCESS)
	goto undo_task;

    kr = bench_child_thread_start(thread, forkrace_child_entry,
				  stack + CHILD_STACK_SIZE);
    if (kr != KERN_SUCCESS)
	goto undo_thread;

    kr = thread_resume(thread);
    if (kr != KERN_SUCCESS)
	goto undo_thread;

    /* 🔴 The thread right is not needed once it is running, and a right this
     * loop keeps thousands of times is a leak that stops the next task. */
    (void) mach_port_deallocate(mach_task_self(), thread);
    *out = task;
    return 1;

undo_thread:
    (void) mach_port_deallocate(mach_task_self(), thread);
undo_task:
    (void) task_terminate(task);
    (void) mach_port_deallocate(mach_task_self(), task);
    return 0;
}

static void
forkrace_retire(mach_port_t task)
{
    (void) task_terminate(task);
    (void) mach_port_deallocate(mach_task_self(), task);
}
#endif	/* FORKRACE_LIVE > 0 */

static void *
forkrace_worker_func(void *arg)
{
    forkrace_worker_t	*w = (forkrace_worker_t *)arg;
    int			 i;

#if	FORKRACE_LIVE > 0
    mach_port_t		live[FORKRACE_LIVE];
    int			held = 0, oldest = 0;

    for (i = 0; i < w->iters; i++) {
	mach_port_t	child;

	if (!forkrace_spawn_live(&child)) {
	    w->refused++;
	    continue;
	}
	w->made++;

	if (held < FORKRACE_LIVE) {
	    live[held++] = child;
	    continue;
	}

	/*
	 * The ring is full: the oldest child dies while this one is starting
	 * to fault the same pages in, and every other worker is walking those
	 * lists too.
	 */
	{
	    mach_port_t	retire = live[oldest];

	    live[oldest] = child;
	    oldest = (oldest + 1) % FORKRACE_LIVE;
	    forkrace_retire(retire);
	}
    }

    while (held-- > 0)
	forkrace_retire(live[held]);

    return (void *) 0;
#else
    for (i = 0; i < w->iters; i++) {
	mach_port_t	child;
	kern_return_t	kr;

	kr = task_create(mach_task_self(), (ledger_port_array_t)0, 0,
			 TRUE, &child);
	if (kr != KERN_SUCCESS) {
	    w->refused++;
	    continue;
	}
	w->made++;

	(void) task_terminate(child);

	/*
	 * 🔴 The right is released, and it is not housekeeping here: the LAST
	 * reference is what carries the map to vm_map_deallocate() and so to
	 * pmap_destroy(). Leaking it would leave every child alive and this
	 * suite would fork a thousand tasks and destroy none of them.
	 */
	(void) mach_port_deallocate(mach_task_self(), child);
    }
    return (void *) 0;
#endif	/* FORKRACE_LIVE > 0 */
}

static void
bench_forkrace(void)
{
    forkrace_worker_t	w[FORKRACE_THREADS];
    vm_offset_t		region = 0;
    kern_return_t	kr;
    tvalspec_t		t0, t1;
    unsigned		made = 0, refused = 0;
    unsigned long	off;
    int			i;

    kr = vm_allocate(mach_task_self(), &region, FORKRACE_REGION, TRUE);
    if (kr != KERN_SUCCESS) {
	printf("  forkrace: region alloc failed %d — not run\n", kr);
	return;
    }
    for (off = 0; off < FORKRACE_REGION; off += FORKRACE_PAGE)
	((volatile char *) region)[off] = 1;

#if	FORKRACE_LIVE > 0
    /* Before any child exists, so every one of them inherits it. */
    forkrace_region_addr = region;
#endif

    for (i = 0; i < FORKRACE_THREADS; i++) {
	w[i].iters = FORKRACE_ITERS;
	w[i].made = 0;
	w[i].refused = 0;
    }

    get_time(&t0);
    for (i = 0; i < FORKRACE_THREADS; i++)
	pthread_create(&w[i].th, NULL, forkrace_worker_func, &w[i]);

    /*
     * 🔴 A HEARTBEAT, BECAUSE "IT HUNG" IS NOT A RESULT.
     *
     * The totals are printed after the join, so a run that wedges prints
     * nothing at all -- and then "wedged" carries no number, which makes
     * "a resource ran out at a fixed count" indistinguishable from "a race
     * caught it somewhere random".  Those want different fixes.
     *
     * The counters are read without synchronisation on purpose: they are
     * aligned words this only ever reads, a stale value costs a heartbeat's
     * accuracy and nothing else, and taking a lock here would serialise the
     * very concurrency the suite exists to create.
     *
     * Printing only on change keeps a wedge from filling the log, and five
     * unchanged rounds is reported once, with the count: that line is the
     * whole diagnostic value of a run that never finishes.
     */
    {
	unsigned	last = 0, still = 0;
	int		running = 1;

	while (running) {
	    unsigned	n = 0;
	    int		k;

	    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 1000);

	    running = 0;
	    for (k = 0; k < FORKRACE_THREADS; k++) {
		n += w[k].made + w[k].refused;
		if ((int)(w[k].made + w[k].refused) < w[k].iters)
		    running = 1;
	    }

	    if (n != last) {
		printf("  ... %u of %d\n", n, FORKRACE_THREADS * FORKRACE_ITERS);
		last = n;
		still = 0;
	    } else if (running && ++still == 5) {
		printf("  !!! STALLED at %u of %d — no progress for five "
		       "rounds; the totals below will not be printed\n",
		       n, FORKRACE_THREADS * FORKRACE_ITERS);
		break;
	    }
	}
    }

    for (i = 0; i < FORKRACE_THREADS; i++)
	pthread_join(w[i].th, NULL);
    get_time(&t1);

    for (i = 0; i < FORKRACE_THREADS; i++) {
	made += w[i].made;
	refused += w[i].refused;
    }

    /*
     * The count is a PRESENCE control: a suite that forked nothing because
     * task_create refused every time would otherwise read exactly like a
     * suite that forked a thousand times and found no defect.
     */
    /*
     * ⚠️ The live count is part of the line because it changes what the run
     * MEANS: with zero the children never run and the pv lists stay one entry
     * long, which is a different experiment wearing the same name.
     */
    printf("  %d threads x %d iters over %d KB, %d live children each: "
	   "%u forked and destroyed, %u refused, %lu us\n",
	   FORKRACE_THREADS, FORKRACE_ITERS, FORKRACE_REGION / 1024,
	   FORKRACE_LIVE, made, refused, elapsed_ns(&t0, &t1) / 1000);

    (void) vm_deallocate(mach_task_self(), region, FORKRACE_REGION);
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

    echo_signal_ready();

    for (;;) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,
		      sizeof(msg),
		      port,
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
	    /*
	     * Terminal errors mean the bench destroyed the receive port
	     * — exit instead of spinning on mach_msg(RCV) forever and
	     * polluting the next test's scheduling.
	     */
	    if (kr == MACH_RCV_PORT_DIED ||
		kr == MACH_RCV_PORT_CHANGED ||
		kr == MACH_RCV_INVALID_NAME ||
		kr == MACH_RCV_TIMED_OUT)
		break;
	    continue;
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

    /* Spawn echo thread and wait until it is provably running (SMP-safe). */
    spawn_echo(label, slow_echo_thread_func, echo_port);

    /* Warmup.  (The old per-iteration thread_switch(YIELD) here assumed UP
     * "echo is now blocked" semantics; on SMP it is unachievable and only
     * added a busy-yield spin, so it is gone — Mach queues the message and
     * the echo thread services it regardless of where it is.) */
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
    get_time(&t0);
    for (i = 0; i < iters; i++) {
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
 * Protected payload test
 *
 * Verifies that mach_port_set_protected_payload() causes the kernel
 * to deliver the payload in msgh_local_port with the
 * MACH_MSGH_BITS_PROTECTED_PAYLOAD bit set.
 * =================================================================== */

#define PP_MAGIC_PAYLOAD	0xDEAD0042

static void
test_protected_payload(void)
{
    mach_port_t		recv_port, send_port;
    kern_return_t	kr;
    int			pass = 0, fail = 0;

    /* Allocate a receive right and make a send right */
    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &recv_port);
    if (kr != KERN_SUCCESS) {
	printf("  pp: port_allocate failed: %d\n", kr);
	return;
    }

    kr = mach_port_insert_right(mach_task_self(), recv_port,
				recv_port, MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
	printf("  pp: insert_right failed: %d\n", kr);
	mach_port_destroy(mach_task_self(), recv_port);
	return;
    }
    send_port = recv_port;

    /* --- Test 1: without payload, msgh_local_port = port name --- */
    {
	bench_null_msg_t send_msg;
	mach_msg_empty_rcv_t recv_msg;

	send_msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	send_msg.head.msgh_size = sizeof(send_msg);
	send_msg.head.msgh_remote_port = send_port;
	send_msg.head.msgh_local_port = MACH_PORT_NULL;
	send_msg.head.msgh_id = 999;

	kr = mach_msg(&send_msg.head, MACH_SEND_MSG,
		      sizeof(send_msg), 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
	    printf("  pp: send failed: %d\n", kr);
	    fail++;
	} else {
	    kr = mach_msg(&recv_msg.header, MACH_RCV_MSG,
			  0, sizeof(recv_msg), recv_port,
			  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	    if (kr != KERN_SUCCESS) {
		printf("  pp: recv failed: %d\n", kr);
		fail++;
	    } else if (recv_msg.header.msgh_bits &
		       MACH_MSGH_BITS_PROTECTED_PAYLOAD) {
		printf("  pp: FAIL no-payload msg has PP bit set\n");
		fail++;
	    } else {
		pass++;
	    }
	}
    }

    /* --- Test 2: with payload, msgh_local_port = payload value --- */
    kr = mach_port_set_protected_payload(mach_task_self(),
					 recv_port, PP_MAGIC_PAYLOAD);
    if (kr != KERN_SUCCESS) {
	printf("  pp: set_protected_payload failed: %d\n", kr);
	fail++;
    } else {
	bench_null_msg_t send_msg;
	mach_msg_empty_rcv_t recv_msg;

	send_msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	send_msg.head.msgh_size = sizeof(send_msg);
	send_msg.head.msgh_remote_port = send_port;
	send_msg.head.msgh_local_port = MACH_PORT_NULL;
	send_msg.head.msgh_id = 1000;

	kr = mach_msg(&send_msg.head, MACH_SEND_MSG,
		      sizeof(send_msg), 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
	    printf("  pp: send w/payload failed: %d\n", kr);
	    fail++;
	} else {
	    kr = mach_msg(&recv_msg.header, MACH_RCV_MSG,
			  0, sizeof(recv_msg), recv_port,
			  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	    if (kr != KERN_SUCCESS) {
		printf("  pp: recv w/payload failed: %d\n", kr);
		fail++;
	    } else if (!(recv_msg.header.msgh_bits &
			 MACH_MSGH_BITS_PROTECTED_PAYLOAD)) {
		printf("  pp: FAIL payload msg missing PP bit\n");
		fail++;
	    } else if ((unsigned)recv_msg.header.msgh_local_port !=
		       PP_MAGIC_PAYLOAD) {
		printf("  pp: FAIL payload=0x%x expected 0x%x\n",
		       (unsigned)recv_msg.header.msgh_local_port,
		       PP_MAGIC_PAYLOAD);
		fail++;
	    } else {
		pass++;
	    }
	}
    }

    /* --- Test 3: clear payload (set to 0), reverts to port name --- */
    kr = mach_port_set_protected_payload(mach_task_self(),
					 recv_port, 0);
    if (kr != KERN_SUCCESS) {
	printf("  pp: clear payload failed: %d\n", kr);
	fail++;
    } else {
	bench_null_msg_t send_msg;
	mach_msg_empty_rcv_t recv_msg;

	send_msg.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	send_msg.head.msgh_size = sizeof(send_msg);
	send_msg.head.msgh_remote_port = send_port;
	send_msg.head.msgh_local_port = MACH_PORT_NULL;
	send_msg.head.msgh_id = 1001;

	kr = mach_msg(&send_msg.head, MACH_SEND_MSG,
		      sizeof(send_msg), 0, MACH_PORT_NULL,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != KERN_SUCCESS) {
	    printf("  pp: send after clear failed: %d\n", kr);
	    fail++;
	} else {
	    kr = mach_msg(&recv_msg.header, MACH_RCV_MSG,
			  0, sizeof(recv_msg), recv_port,
			  MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	    if (kr != KERN_SUCCESS) {
		printf("  pp: recv after clear failed: %d\n", kr);
		fail++;
	    } else if (recv_msg.header.msgh_bits &
		       MACH_MSGH_BITS_PROTECTED_PAYLOAD) {
		printf("  pp: FAIL cleared msg still has PP bit\n");
		fail++;
	    } else {
		pass++;
	    }
	}
    }

    mach_port_destroy(mach_task_self(), recv_port);

    printf("  protected payload: %d PASS, %d FAIL\n", pass, fail);
}

/* ===================================================================
 * Protected payload performance benchmark (intra-task)
 *
 * Measures RPC latency with the echo thread's receive port
 * having a protected payload set vs. not set.  Accepts send_size
 * to test with different inline data sizes.
 * =================================================================== */

static void
bench_pp_intra(const char *label, int send_size, int use_pp, int iters)
{
    mach_port_t		echo_port, reply_port;
    kern_return_t	kr;
    tvalspec_t		t0, t1;
    int			i;
    static bench_recv_buf_t	send_buf;
    static bench_recv_buf_t	recv_buf;

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &echo_port);
    if (kr) { printf("  pp bench: port alloc failed %d\n", kr); return; }

    kr = mach_port_insert_right(mach_task_self(),
				echo_port, echo_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  pp bench: insert right failed %d\n", kr); return; }

    if (use_pp) {
	kr = mach_port_set_protected_payload(mach_task_self(),
					     echo_port, PP_MAGIC_PAYLOAD);
	if (kr) { printf("  pp bench: set_payload failed %d\n", kr); return; }
    }

    reply_port = mach_reply_port();

    /* Spawn echo thread and wait until it is provably running (SMP-safe). */
    spawn_echo(label, echo_thread_func, echo_port);

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

    print_result(label, elapsed_ns(&t0, &t1), iters);

    mach_port_destroy(mach_task_self(), echo_port);
    mach_port_destroy(mach_task_self(), reply_port);
}

/* ===================================================================
 * Protected payload performance benchmark (inter-task)
 *
 * Like bench_inter_rpc but runs twice per size: once without payload,
 * once with payload set on the child's receive port before moving it.
 * =================================================================== */

static void
bench_pp_inter(const char *label, int send_size, int use_pp, int iters)
{
    kern_return_t		kr;
    mach_port_t			child_task, child_thread;
    mach_port_t			child_recv_port;
    mach_port_t			parent_recv_port;
    vm_offset_t			child_stack;
    tvalspec_t			t0, t1;
    int				i;
    static bench_recv_buf_t	send_buf;
    static bench_recv_buf_t	recv_buf;

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE,
			    &child_recv_port);
    if (kr) { printf("  %s: alloc child_recv failed %d\n", label, kr); return; }

    kr = mach_port_insert_right(mach_task_self(),
				child_recv_port, child_recv_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert send right failed %d\n", label, kr); return; }

    if (use_pp) {
	kr = mach_port_set_protected_payload(mach_task_self(),
					     child_recv_port,
					     PP_MAGIC_PAYLOAD);
	if (kr) { printf("  %s: set_payload failed %d\n", label, kr); return; }
    }

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE,
			    &parent_recv_port);
    if (kr) { printf("  %s: alloc parent_recv failed %d\n", label, kr); return; }

    kr = mach_port_insert_right(mach_task_self(),
				parent_recv_port, parent_recv_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert parent send failed %d\n", label, kr); return; }

    kr = task_create(mach_task_self(),
		     (ledger_port_array_t)0, 0,
		     TRUE, &child_task);
    if (kr) { printf("  %s: task_create failed %d\n", label, kr); return; }

    child_stack = 0;
    kr = vm_allocate(child_task, &child_stack, CHILD_STACK_SIZE, TRUE);
    if (kr) { printf("  %s: child stack alloc failed %d\n", label, kr); return; }

    kr = mach_port_insert_right(child_task,
				CHILD_RECV_NAME,
				child_recv_port,
				MACH_MSG_TYPE_MOVE_RECEIVE);
    if (kr) { printf("  %s: insert child recv failed %d\n", label, kr); return; }

    kr = mach_port_insert_right(child_task,
				CHILD_SEND_NAME,
				parent_recv_port,
				MACH_MSG_TYPE_COPY_SEND);
    if (kr) { printf("  %s: insert child send failed %d\n", label, kr); return; }

    kr = thread_create(child_task, &child_thread);
    if (kr) { printf("  %s: thread_create failed %d\n", label, kr); return; }

    kr = bench_child_thread_start(child_thread, child_echo_entry,
				  child_stack + CHILD_STACK_SIZE);
    if (kr) { printf("  %s: child thread start failed %d\n", label, kr); return; }

    kr = thread_resume(child_thread);
    if (kr) { printf("  %s: thread_resume failed %d\n", label, kr); return; }

    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

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

    print_result(label, elapsed_ns(&t0, &t1), iters);

    task_terminate(child_task);
    mach_port_deallocate(mach_task_self(), child_thread);
    mach_port_deallocate(mach_task_self(), child_task);
    mach_port_destroy(mach_task_self(), child_recv_port);
    mach_port_destroy(mach_task_self(), parent_recv_port);
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

    echo_signal_ready();

    for (;;) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,
		      sizeof(msg),
		      port,
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
	    /*
	     * Terminal errors mean the bench destroyed the receive port
	     * — exit instead of spinning on mach_msg(RCV) forever and
	     * polluting the next test's scheduling.
	     */
	    if (kr == MACH_RCV_PORT_DIED ||
		kr == MACH_RCV_PORT_CHANGED ||
		kr == MACH_RCV_INVALID_NAME ||
		kr == MACH_RCV_TIMED_OUT)
		break;
	    continue;
	}

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

    /* Spawn echo thread and wait until it is provably running (SMP-safe). */
    spawn_echo(label, ool_echo_thread_func, echo_port);

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
 * Out-of-line PORT arrays, swept by port count (#415)
 *
 * The kernel keeps ipc_port_t pointers in message fields that are the
 * width of a port name.  On i386 those are the same four bytes; on
 * x86-64 they are not, and the candidate designs cost differently
 * depending on how many ports a message carries.  Counting live traffic
 * said 99.94% of messages carry one or two and nothing in three million
 * carried more than six -- but that is what this workload does, not an
 * upper bound.  task_threads() returns one port per thread, so a large
 * task asks for as many as it has.
 *
 * This measures the part that need not be guessed: what a message with
 * N ports costs, as N grows.  The ns/port column is the one to read.
 * Flat means the cost is proportional, and a design that pays per port
 * pays fairly.  A rise means something in the path is worse than linear
 * and the tail matters more than the average suggests.
 *
 * Each port travels as MAKE_SEND from a receive right this task holds,
 * so the same set can be sent every iteration; the echo side drops the
 * send right it was handed and frees the array, leaving the port count
 * where it started.
 * =================================================================== */

typedef struct {
    mach_msg_header_t			head;
    mach_msg_body_t			body;
    mach_msg_ool_ports_descriptor_t	ports;
} bench_oolp_send_msg_t;

typedef struct {
    mach_msg_header_t			head;
    mach_msg_body_t			body;
    mach_msg_ool_ports_descriptor_t	ports;
    mach_msg_trailer_t			trailer;
} bench_oolp_recv_msg_t;

/*
 * Echo thread for out-of-line port arrays.
 *
 * Releasing the rights matters here in a way it does not for OOL data:
 * every message hands this side one send right per port, and a
 * benchmark that leaked them would be measuring a port table growing
 * underneath it rather than the cost of carrying ports.
 */
static void *
oolp_echo_thread_func(void *arg)
{
    mach_port_t			port = (mach_port_t)(unsigned long)arg;
    bench_oolp_recv_msg_t	msg;
    bench_null_msg_t		reply;
    kern_return_t		kr;

    echo_signal_ready();

    for (;;) {
	kr = mach_msg(&msg.head, MACH_RCV_MSG, 0, sizeof(msg),
		      port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
	    if (kr == MACH_RCV_PORT_DIED ||
		kr == MACH_RCV_PORT_CHANGED ||
		kr == MACH_RCV_INVALID_NAME ||
		kr == MACH_RCV_TIMED_OUT)
		break;
	    continue;
	}

	if ((msg.head.msgh_bits & MACH_MSGH_BITS_COMPLEX) &&
	    msg.body.msgh_descriptor_count == 1 &&
	    msg.ports.address != 0 && msg.ports.count > 0) {
	    mach_port_t	   *names = (mach_port_t *) msg.ports.address;
	    unsigned int    n = msg.ports.count;
	    unsigned int    j;

	    for (j = 0; j < n; j++)
		if (names[j] != MACH_PORT_NULL)
		    mach_port_deallocate(mach_task_self(), names[j]);

	    vm_deallocate(mach_task_self(),
			  (vm_offset_t) msg.ports.address,
			  (vm_size_t) n * sizeof(mach_port_t));
	}

	reply.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
	reply.head.msgh_size	    = sizeof(reply);
	reply.head.msgh_remote_port = msg.head.msgh_remote_port;
	reply.head.msgh_local_port  = MACH_PORT_NULL;
	reply.head.msgh_id	    = msg.head.msgh_id + 100;
	mach_msg(&reply.head, MACH_SEND_MSG, sizeof(reply), 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return (void *) 0;
}

/*
 * Constant work per point rather than constant iterations: a sweep
 * whose points take wildly different wall time measures the slow end on
 * fewest samples, which is where the noise is worst.  Bounded at both
 * ends so a small count still gets a decent sample and a large one
 * still finishes.
 */
static int
oolp_iters_for(unsigned int nports)
{
    unsigned int iters = 40000u / nports;

    if (iters > 2000u)
	iters = 2000u;
    if (iters < 30u)
	iters = 30u;
    return (int) iters;
}

/*
 * Returns FALSE when the port count could not be built, so the sweep
 * stops rather than going on to report numbers for arrays it did not
 * manage to make.
 */
static boolean_t
bench_ool_ports(unsigned int nports)
{
    mach_port_t			echo_port, reply_port;
    mach_port_t		       *names = 0;
    bench_oolp_send_msg_t	send_buf;
    bench_oolp_recv_msg_t	recv_buf;
    kern_return_t		kr;
    tvalspec_t			t0, t1;
    unsigned long		ns, per_op;
    int				iters = oolp_iters_for(nports);
    unsigned int		i, made;

    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &echo_port);
    if (kr) {
	printf("  %5u ports: echo port alloc failed %d\n", nports, kr);
	return FALSE;
    }
    kr = mach_port_insert_right(mach_task_self(), echo_port, echo_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) {
	printf("  %5u ports: insert right failed %d\n", nports, kr);
	return FALSE;
    }
    reply_port = mach_reply_port();
    spawn_echo("oolp-echo", oolp_echo_thread_func, echo_port);

    /*
     * The ports to carry, made once and outside the timing: what is
     * being measured is carrying them, not creating them.
     */
    kr = vm_allocate(mach_task_self(), (vm_offset_t *) &names,
		     (vm_size_t) nports * sizeof(mach_port_t), TRUE);
    if (kr) {
	printf("  %5u ports: array alloc failed %d\n", nports, kr);
	return FALSE;
    }

    for (made = 0; made < nports; made++) {
	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &names[made]);
	if (kr != KERN_SUCCESS) {
	    printf("  %5u ports: only %u could be allocated (%d)"
		   " -- sweep stops here\n", nports, made, kr);
	    for (i = 0; i < made; i++)
		mach_port_destroy(mach_task_self(), names[i]);
	    return FALSE;
	}
    }

#define OOLP_FILL()							\
    do {								\
	send_buf.head.msgh_bits =					\
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,			\
			   MACH_MSG_TYPE_MAKE_SEND_ONCE) |		\
	    MACH_MSGH_BITS_COMPLEX;					\
	send_buf.head.msgh_size		= sizeof(send_buf);		\
	send_buf.head.msgh_remote_port	= echo_port;			\
	send_buf.head.msgh_local_port	= reply_port;			\
	send_buf.head.msgh_id		= 1;				\
	send_buf.body.msgh_descriptor_count = 1;			\
	send_buf.ports.address		= (void *) names;		\
	send_buf.ports.count		= nports;			\
	send_buf.ports.deallocate	= FALSE;			\
	send_buf.ports.copy		= MACH_MSG_PHYSICAL_COPY;	\
	send_buf.ports.disposition	= MACH_MSG_TYPE_MAKE_SEND;	\
	send_buf.ports.type		= MACH_MSG_OOL_PORTS_DESCRIPTOR;\
    } while (0)

    for (i = 0; i < 20u; i++) {
	OOLP_FILL();
	mach_msg(&send_buf.head, MACH_SEND_MSG, sizeof(send_buf), 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    get_time(&t0);
    for (i = 0; i < (unsigned int) iters; i++) {
	OOLP_FILL();
	mach_msg(&send_buf.head, MACH_SEND_MSG, sizeof(send_buf), 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);
#undef OOLP_FILL

    ns = elapsed_ns(&t0, &t1);
    per_op = ns / (unsigned long) iters;
    printf("  %5u %-11s %5lu.%02lu us/op  %6lu ns/port  (%d iters)\n",
	   nports, nports == 1 ? "port" : "ports",
	   per_op / 1000, (per_op % 1000) / 10,
	   per_op / (unsigned long) nports, iters);

    for (i = 0; i < nports; i++)
	mach_port_destroy(mach_task_self(), names[i]);
    vm_deallocate(mach_task_self(), (vm_offset_t) names,
		  (vm_size_t) nports * sizeof(mach_port_t));
    mach_port_destroy(mach_task_self(), echo_port);
    return TRUE;
}

static void
bench_ool_ports_sweep(void)
{
    /*
     * The 1-port point is measured twice, first and last, and the two
     * do not agree: 173 us against 7.81, same count and same code.
     * Whatever the first measurement in a sweep pays for -- the echo
     * thread's first run, the port table growing, the kernel's zones
     * touched for the first time -- it pays once, and the first point
     * is where it lands.
     *
     * Kept rather than worked around.  A sweep that quietly discarded
     * its first point would be hiding the one number that says how
     * much of what follows is warm.  Read the repeat, not the opener.
     */
    static const unsigned int counts[] = {
	1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024, 1
    };
    unsigned int i;

    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++)
	if (!bench_ool_ports(counts[i]))
	    return;
}

/* ===================================================================
 * OOL inter-task benchmark
 *
 * Creates a child task (like bench_inter_rpc), but the parent sends
 * OOL data and the child deallocates it before replying.  This
 * measures the real cross-address-space COW cost that every server
 * pays when receiving data from a client.
 * =================================================================== */

/*
 * Child echo entry for OOL messages.
 * Receives OOL data, deallocates it, sends null reply.
 * Runs in the child task — only raw mach_msg(), no MIG.
 */
/*
 * Child echo entry for OOL messages.
 * Receives OOL data and sends a null reply.
 *
 * We intentionally skip vm_deallocate() on the received OOL data:
 * the child task has no cthread context (mig_get_reply_port crashes),
 * and the child is terminated when the benchmark ends, so all its
 * memory is reclaimed.  Only raw mach_msg() (direct SYSENTER) is safe.
 */
static void __attribute__((noreturn, used))
child_ool_echo_entry(void)
{
    bench_ool_recv_msg_t	msg;
    bench_null_msg_t		reply;

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

static void
bench_ool_inter_rpc(const char *label, vm_size_t ool_size, int iters)
{
    kern_return_t		kr;
    mach_port_t			child_task, child_thread;
    mach_port_t			child_recv_port;
    mach_port_t			parent_recv_port;
    vm_offset_t			child_stack;
    tvalspec_t			t0, t1;
    int				i;
    bench_ool_send_msg_t	send_buf;
    bench_null_msg_t		recv_buf;
    vm_offset_t			data_buf;

    /* Allocate OOL data buffer */
    kr = vm_allocate(mach_task_self(), &data_buf, ool_size, TRUE);
    if (kr) { printf("  %s: vm_allocate failed %d\n", label, kr); return; }

    /* Allocate ports */
    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &child_recv_port);
    if (kr) { printf("  %s: alloc child_recv failed %d\n", label, kr); return; }
    kr = mach_port_insert_right(mach_task_self(),
				child_recv_port, child_recv_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert send right failed %d\n", label, kr); return; }
    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE, &parent_recv_port);
    if (kr) { printf("  %s: alloc parent_recv failed %d\n", label, kr); return; }
    kr = mach_port_insert_right(mach_task_self(),
				parent_recv_port, parent_recv_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr) { printf("  %s: insert parent send right failed %d\n", label, kr); return; }

    /* Create child task */
    kr = task_create(mach_task_self(),
		     (ledger_port_array_t)0, 0, TRUE, &child_task);
    if (kr) { printf("  %s: task_create failed %d\n", label, kr); return; }

    child_stack = 0;
    kr = vm_allocate(child_task, &child_stack, CHILD_STACK_SIZE, TRUE);
    if (kr) { printf("  %s: child stack alloc failed %d\n", label, kr); return; }

    /* Insert port rights into child */
    kr = mach_port_insert_right(child_task, CHILD_RECV_NAME,
				child_recv_port, MACH_MSG_TYPE_MOVE_RECEIVE);
    if (kr) { printf("  %s: insert child recv failed %d\n", label, kr); return; }
    kr = mach_port_insert_right(child_task, CHILD_SEND_NAME,
				parent_recv_port, MACH_MSG_TYPE_COPY_SEND);
    if (kr) { printf("  %s: insert child send failed %d\n", label, kr); return; }

    /* Create and start child thread */
    kr = thread_create(child_task, &child_thread);
    if (kr) { printf("  %s: thread_create failed %d\n", label, kr); return; }

    if (bench_child_thread_start(child_thread, child_ool_echo_entry,
				 child_stack + CHILD_STACK_SIZE)) {
	printf("  %s: child thread start failed\n", label);
	return;
    }
    thread_resume(child_thread);
    thread_switch(MACH_PORT_NULL, SWITCH_OPTION_DEPRESS, 10);

    /* Warmup */
    for (i = 0; i < WARMUP_ITERS; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	send_buf.head.msgh_size	      = sizeof(send_buf);
	send_buf.head.msgh_remote_port = child_recv_port;
	send_buf.head.msgh_local_port  = MACH_PORT_NULL;
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
		 parent_recv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    /* Timed run */
    get_time(&t0);
    for (i = 0; i < iters; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	send_buf.head.msgh_size	      = sizeof(send_buf);
	send_buf.head.msgh_remote_port = child_recv_port;
	send_buf.head.msgh_local_port  = MACH_PORT_NULL;
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
		 parent_recv_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    get_time(&t1);

    print_result(label, elapsed_ns(&t0, &t1), iters);

    /* Cleanup */
    task_terminate(child_task);
    mach_port_deallocate(mach_task_self(), child_thread);
    mach_port_deallocate(mach_task_self(), child_task);
    mach_port_destroy(mach_task_self(), child_recv_port);
    mach_port_destroy(mach_task_self(), parent_recv_port);
    vm_deallocate(mach_task_self(), data_buf, ool_size);
}

/* ===================================================================
 * Raw syscall benchmarks (mach_null / mach_print)
 *
 * These measure the pure SYSENTER/SYSEXIT round-trip cost without
 * any IPC overhead.  Comparable to GNU Mach's mach_print benchmark.
 * =================================================================== */

#define SYSCALL_BENCH_ITERS	(1 << 18)	/* ~256K iterations */
#define SYSCALL_PRINT_ITERS	(1 << 14)	/* ~16K — printf is slow */

static void
bench_mach_null(int iters)
{
    tvalspec_t	t0, t1;
    int		i;

    /* Warmup */
    for (i = 0; i < WARMUP_ITERS; i++)
	mach_null();

    get_time(&t0);
    for (i = 0; i < iters; i++)
	mach_null();
    get_time(&t1);

    print_result("mach_null (noop trap)", elapsed_ns(&t0, &t1), iters);
}

/*
 * A round trip to the KERNEL, not to another task (#443).
 *
 * The MIG argument checks live in the generated kernel server stubs, so
 * they are paid by calls like this one and by nothing else: an intra-task
 * null RPC never touches a server stub, and mach_null is a trap that never
 * reaches MIG at all.  Measuring either of those to decide what TypeCheck
 * costs would return zero by construction -- a zero that means the
 * instrument was pointed the wrong way, not that the cost is absent.
 *
 * mach_port_type is chosen for doing almost nothing: one name in, one
 * word out.  The less work the routine does, the larger the share of the
 * measurement that IS the path being asked about.
 */
static void
bench_kernel_rpc(int iters)
{
    tvalspec_t		t0, t1;
    mach_port_t		name;
    mach_port_type_t	type;
    kern_return_t	kr;
    int			i;

    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &name);
    if (kr != KERN_SUCCESS) {
	printf("  krpc: port allocate failed %d\n", kr);
	return;
    }

    for (i = 0; i < WARMUP_ITERS; i++)
	(void) mach_port_type(mach_task_self(), name, &type);

    get_time(&t0);
    for (i = 0; i < iters; i++)
	(void) mach_port_type(mach_task_self(), name, &type);
    get_time(&t1);

    print_result("mach_port_type (kernel RPC)", elapsed_ns(&t0, &t1), iters);

    (void) mach_port_destroy(mach_task_self(), name);
}

static void
bench_mach_print(int iters)
{
    tvalspec_t	t0, t1;
    int		i;

    /* Warmup */
    for (i = 0; i < 10; i++)
	mach_print("");

    get_time(&t0);
    for (i = 0; i < iters; i++)
	mach_print("");
    get_time(&t1);

    print_result("mach_print(\"\") trap", elapsed_ns(&t0, &t1), iters);
}

/* ===================================================================
 * Test suite selection
 *
 * Without arguments: run all suites.
 * With arguments: run only the named suites.
 *
 * Valid suite names (argv):
 *   syscall  — mach_null / mach_print trap
 *   intra    — intra-task Mach IPC RPC
 *   slow     — slow-path receive (continuation)
 *   inter    — inter-task Mach IPC RPC
 *   isweep   — inter-task RPC swept by size, repeated within one boot (#446)
 *   forkrace — concurrent task_create/task_terminate, no IPC (#558)
 *   port     — port alloc/destroy, mach_port_names
 *   pp       — protected payload (test + intra + inter)
 *   ool      — out-of-line data (intra + inter)
 *   disk     — disk I/O (ahci + ext2)
 *   mem      — memory bandwidth
 *   flipc2   — all FLIPC v2 benchmarks
 *   ports    — out-of-line port arrays, swept by port count (#415)
 *   all      — everything (same as no arguments)
 * =================================================================== */

#define SUITE_SYSCALL	(1u <<  0)
#define SUITE_INTRA	(1u <<  1)
#define SUITE_SLOW	(1u <<  2)
#define SUITE_INTER	(1u <<  3)
#define SUITE_PORT	(1u <<  4)
#define SUITE_PP	(1u <<  5)
#define SUITE_OOL	(1u <<  6)
#define SUITE_DISK	(1u <<  7)
#define SUITE_MEM	(1u <<  8)
#define SUITE_FLIPC2	(1u <<  9)
#define SUITE_COMB	(1u << 10)
#define SUITE_CC	(1u << 11)	/* concurrent same-space (#327) */
#define SUITE_FAULT	(1u << 12)	/* concurrent same-space faults (#338) */
#define SUITE_SCALE	(1u << 13)	/* concurrency sweep, clean numbers (#319) */
#define SUITE_PORTS	(1u << 14)	/* out-of-line port arrays, by count (#415) */
#define SUITE_KRPC	(1u << 15)	/* MIG kernel RPC, where TypeCheck lives (#443) */
#define SUITE_ISWEEP	(1u << 16)	/* inter-task RPC by size, repeated (#446) */
#define SUITE_FORKRACE	(1u << 17)	/* concurrent task_create/terminate (#558) */

/*
 *	`isweep' is a diagnostic and has to be ASKED FOR BY NAME.
 *
 *	It answers one question, it creates a child task per measurement, and
 *	it prints a block that is read as a picture rather than as a row in
 *	the baseline table -- so folding it into `all' would change what every
 *	unargumented run of this server produces, and the 20/06 tables are
 *	compared against those runs.  Naming it in `suites NOT run' is the
 *	honest form: it says the suite exists and did not run.
 */
#define SUITE_ALL	(0xFFFFFFFFu & ~SUITE_ISWEEP & ~SUITE_FORKRACE)

static int
streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static unsigned int
parse_suites(int argc, char **argv)
{
    unsigned int mask = 0;
    int i;

    for (i = 1; i < argc; i++) {
	if (streq(argv[i], "all"))	    mask |= SUITE_ALL;
	else if (streq(argv[i], "syscall")) mask |= SUITE_SYSCALL;
	else if (streq(argv[i], "intra"))   mask |= SUITE_INTRA;
	else if (streq(argv[i], "slow"))    mask |= SUITE_SLOW;
	else if (streq(argv[i], "inter"))   mask |= SUITE_INTER;
	else if (streq(argv[i], "isweep"))  mask |= SUITE_ISWEEP;
	else if (streq(argv[i], "forkrace")) mask |= SUITE_FORKRACE;
	else if (streq(argv[i], "port"))    mask |= SUITE_PORT;
	else if (streq(argv[i], "pp"))	    mask |= SUITE_PP;
	else if (streq(argv[i], "ool"))	    mask |= SUITE_OOL;
	else if (streq(argv[i], "disk"))    mask |= SUITE_DISK;
	else if (streq(argv[i], "mem"))	    mask |= SUITE_MEM;
	else if (streq(argv[i], "flipc2"))  mask |= SUITE_FLIPC2;
	else if (streq(argv[i], "ports"))   mask |= SUITE_PORTS;
	else if (streq(argv[i], "krpc"))    mask |= SUITE_KRPC;
	else if (streq(argv[i], "comb"))    mask |= SUITE_COMB;
	else if (streq(argv[i], "cc"))	    mask |= SUITE_CC;
	else if (streq(argv[i], "fault"))   mask |= SUITE_FAULT;
	else if (streq(argv[i], "scale"))   mask |= SUITE_SCALE;
    }
    return mask ? mask : SUITE_ALL;
}

/* ===================================================================
 * Concurrent same-space RPC benchmark (#327)
 *
 * Spawns `nthreads` worker threads in THIS task, each driving its own
 * echo thread over a private port pair.  All of them share one ipc_space,
 * so every mach_msg they issue contends on the space lock.  With the old
 * mutex (is_read_lock == is_write_lock) the workers serialize on it; with
 * the #327 reader/writer lock the receive-side lookups run concurrently
 * while the send-side copyin still takes the write side.  We report the
 * aggregate ns/RPC so the scaling from x1 -> xN exposes the contention:
 * a flat number means full serialization, a dropping one means the read
 * side now overlaps.  Compare the same numbers against a mutex kernel
 * (A/B) to isolate the lock from raw CPU parallelism.
 * =================================================================== */

#define MAX_CC_THREADS	32	/* #319: oversubscription sweep up to 32 */
#define SCALE_ITERS	2000	/* per thread per run in the scale sweep */
#define SCALE_RUNS	5	/* runs per thread-count -> median (kill noise) */

typedef struct {
    mach_port_t	echo_port;
    mach_port_t	reply_port;
    int		iters;
    int		send_size;
    pthread_t	th;
} cc_worker_t;

static volatile int g_cc_go;	/* start barrier for the timed region */

static void *
cc_worker_func(void *arg)
{
    cc_worker_t		*w = (cc_worker_t *)arg;
    bench_recv_buf_t	send_buf;
    /*
     * #374: receive into a full-size buffer, not a header-only bench_null_msg_t.
     * A Mach receive writes the reply plus the kernel trailer, so a 24-byte
     * (header-only) stack buffer has zero slack -- any byte past it lands on this
     * frame's saved ebp / return address, the flaky scaling-sweep stack smash
     * (eip=0x0/0x18; 0x18 = sizeof(mach_msg_header_t)).  Every other receiver in
     * this file already uses bench_recv_buf_t; match them.
     */
    bench_recv_buf_t	recv_buf;
    int			i;

    /*
     * All workers begin together -- but YIELD while waiting, do not spin.
     *
     * ⚠️ This loop used to be a naked spin, and that is a livelock: the
     * thread that sets g_cc_go is the one still inside the pthread_create
     * loop above, so every worker already created is burning a processor
     * while the releaser needs one.  At eight workers on four processors it
     * wedges -- measured at 13% of runs on the current tree and 50% on the
     * tree before it, always at the tail of the #319 scaling sweep, never
     * earlier, because that is where the spinners first outnumber the
     * processors by enough to starve the creator.
     *
     * A barrier whose waiters can starve its releaser is not a barrier.
     *
     * ⚠️ thread_switch and not swtch_pri: swtch_pri depresses the caller's
     * priority until it next blocks, which would still be in force during
     * the timed region immediately below -- a fix that silently changes the
     * number the benchmark exists to produce.  This one gives up the
     * processor and nothing else.
     */
    while (!g_cc_go)
	(void) syscall_thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);

    for (i = 0; i < w->iters; i++) {
	send_buf.head.msgh_bits =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
			   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	send_buf.head.msgh_size	       = w->send_size;
	send_buf.head.msgh_remote_port = w->echo_port;
	send_buf.head.msgh_local_port  = w->reply_port;
	send_buf.head.msgh_id	       = 1;

	mach_msg(&send_buf.head, MACH_SEND_MSG, w->send_size, 0,
		 MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	mach_msg(&recv_buf.head, MACH_RCV_MSG, 0, sizeof(recv_buf),
		 w->reply_port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return (void *)0;
}

/*
 * Run the concurrent same-space RPC workload ONCE and return the aggregate
 * wall-clock ns (no print).  Shared by the cc suite (#327) and the scale sweep
 * (#319).  nthreads workers each do iters RPCs through a private echo thread,
 * all released together by the g_cc_go barrier.
 */
static unsigned long
cc_run_once(int nthreads, int send_size, int iters_per_thread)
{
    cc_worker_t		w[MAX_CC_THREADS];
    tvalspec_t		t0, t1;
    kern_return_t	kr;
    unsigned long	total_ns;
    int			i;

    if (nthreads > MAX_CC_THREADS)
	nthreads = MAX_CC_THREADS;

    g_cc_go = 0;

    for (i = 0; i < nthreads; i++) {
	kr = mach_port_allocate(mach_task_self(),
				MACH_PORT_RIGHT_RECEIVE, &w[i].echo_port);
	if (kr) { printf("  cc[%d]: echo alloc failed %d\n", i, kr); return 0; }
	mach_port_insert_right(mach_task_self(), w[i].echo_port,
			       w[i].echo_port, MACH_MSG_TYPE_MAKE_SEND);
	w[i].reply_port = mach_reply_port();
	w[i].iters	= iters_per_thread;
	w[i].send_size	= send_size;
	spawn_echo("cc-echo", echo_thread_func, w[i].echo_port);
    }

    for (i = 0; i < nthreads; i++)
	pthread_create(&w[i].th, NULL, cc_worker_func, &w[i]);

    get_time(&t0);
    g_cc_go = 1;
    for (i = 0; i < nthreads; i++)
	pthread_join(w[i].th, NULL);
    get_time(&t1);
    total_ns = elapsed_ns(&t0, &t1);

    for (i = 0; i < nthreads; i++) {
	mach_port_destroy(mach_task_self(), w[i].echo_port);
	mach_port_destroy(mach_task_self(), w[i].reply_port);
    }
    return total_ns;
}

static void
bench_concurrent_samespace(int nthreads, int send_size, int iters_per_thread)
{
    unsigned long	total_ns;
    long		total_rpc;

    if (nthreads > MAX_CC_THREADS)
	nthreads = MAX_CC_THREADS;
    total_ns  = cc_run_once(nthreads, send_size, iters_per_thread);
    total_rpc = (long)nthreads * iters_per_thread;
    printf("  concurrent same-space x%d   %ld RPCs in %lu ns  "
	   "(%lu ns/RPC aggregate)\n",
	   nthreads, total_rpc, total_ns,
	   total_ns / (total_rpc ? total_rpc : 1));
}

/*
 * #319: concurrency sweep with median-of-N for CLEAN scaling numbers.  At the
 * current CPU count (set at boot, -cN), run the concurrent same-space RPC
 * workload at increasing thread counts; for each, take the median over
 * SCALE_RUNS runs (a warmup run is discarded) to kill the single-run variance.
 * Report aggregate ns/RPC and speedup vs 1 thread.  This stresses the
 * scheduler/runq + IPC wakeup (the #319 target): perfect scaling keeps ns/RPC
 * flat (speedup = T); a global-runq bottleneck makes ns/RPC rise (speedup < T).
 * One boot = one clean curve for that CPU count; reboot with -cN for the rest.
 */
static void
bench_scale(void)
{
    static const int	tc[] = { 1, 2, 4, 6, 8, 12, 16, 24, 32 };
    int			ntc = (int)(sizeof(tc) / sizeof(tc[0]));
    int			send = (int)sizeof(bench_null_msg_t);
    unsigned long	base_nspr = 0;
    int			i, r, a, b;

    printf("    thr   med ns/RPC(aggr)   speedup vs 1\n");
    for (i = 0; i < ntc; i++) {
	int		T = tc[i];
	unsigned long	s[SCALE_RUNS], med, nspr, sx100;

	if (T > MAX_CC_THREADS)
	    break;

	(void) cc_run_once(T, send, SCALE_ITERS / 4);	/* warmup, discarded */
	for (r = 0; r < SCALE_RUNS; r++)
	    s[r] = cc_run_once(T, send, SCALE_ITERS);
	/* insertion sort of SCALE_RUNS samples -> median (no qsort/stdlib) */
	for (a = 1; a < SCALE_RUNS; a++) {
	    unsigned long key = s[a];
	    for (b = a - 1; b >= 0 && s[b] > key; b--)
		s[b + 1] = s[b];
	    s[b + 1] = key;
	}
	med  = s[SCALE_RUNS / 2];
	nspr = med / ((unsigned long)T * SCALE_ITERS);
	if (nspr == 0)
	    nspr = 1;
	if (i == 0)
	    base_nspr = nspr;
	sx100 = (unsigned long)T * base_nspr * 100 / nspr;	/* speedup x100 */
	printf("    %-4d  %-15lu   %lu.%02lux\n",
	       T, nspr, sx100 / 100, sx100 % 100);
    }
}

/* ===================================================================
 * pmap concurrent-fault stress (#338: split page-table locks)
 *
 * N worker threads (oversubscribed vs CPUs) each loop: mmap a private
 * anonymous chunk, write every page (faults it in -> pmap_enter on this
 * address space), read every page back checking the value (a racing
 * pmap_remove/enter or a missed TLB shootdown would corrupt it), then
 * munmap (-> pmap_remove).  All in ONE address space across all CPUs,
 * so it hammers the per-PT-page pt-locks, the pmd_same recheck, the
 * enter<->activate TLB barrier and the protocol-2/3 ordering -- the
 * paths KVM (TSO) cannot exercise.  Completion = no deadlock; the value
 * check = no corruption.
 * =================================================================== */

#define MAX_FAULT_THREADS	32

static volatile int	g_fault_go;
static volatile int	g_fault_err;

typedef struct {
    int			id;
    int			iters;
    int			chunk_kb;
    pthread_t		th;
    unsigned long	checksum;
} fault_worker_t;

static void *
fault_worker_func(void *arg)
{
    fault_worker_t	*w = (fault_worker_t *)arg;
    vm_size_t		sz = (vm_size_t)w->chunk_kb * 1024;
    unsigned long	sum = 0;
    int			it;
    vm_size_t		off;

    /* Same barrier, same defect, same fix -- see cc_worker_func above. */
    while (!g_fault_go)
	(void) syscall_thread_switch(MACH_PORT_NULL, SWITCH_OPTION_NONE, 0);

    for (it = 0; it < w->iters; it++) {
	vm_address_t	addr = 0;
	volatile char	*p;

	/* Mach-native anonymous map (== mmap MAP_ANON under the hood); the
	 * sa_mach header shadow makes <sys/mman.h> clash on off_t. */
	if (vm_allocate(mach_task_self(), &addr, sz, TRUE) != KERN_SUCCESS) {
	    g_fault_err = 1;
	    continue;
	}
	p = (volatile char *)addr;
	/* fault every page in (pmap_enter) */
	for (off = 0; off < sz; off += 4096)
	    p[off] = (char)((off >> 12) + it + w->id);
	/* read back: a racing remove/enter or stale TLB shows up here */
	for (off = 0; off < sz; off += 4096) {
	    if (p[off] != (char)((off >> 12) + it + w->id))
		g_fault_err = 1;
	    sum += (unsigned char)p[off];
	}
	vm_deallocate(mach_task_self(), addr, sz);	/* pmap_remove */
    }
    w->checksum = sum;
    return (void *)0;
}

static void
bench_fault_stress(int nthreads, int chunk_kb, int iters_per_thread)
{
    fault_worker_t	w[MAX_FAULT_THREADS];
    tvalspec_t		t0, t1;
    int			i;

    if (nthreads > MAX_FAULT_THREADS)
	nthreads = MAX_FAULT_THREADS;

    g_fault_go = 0;
    g_fault_err = 0;

    for (i = 0; i < nthreads; i++) {
	w[i].id		= i;
	w[i].iters	= iters_per_thread;
	w[i].chunk_kb	= chunk_kb;
	w[i].checksum	= 0;
	pthread_create(&w[i].th, NULL, fault_worker_func, &w[i]);
    }

    get_time(&t0);
    g_fault_go = 1;
    for (i = 0; i < nthreads; i++)
	pthread_join(w[i].th, NULL);
    get_time(&t1);

    {
	unsigned long	total_ns = elapsed_ns(&t0, &t1);
	long		pages	 = (long)nthreads * iters_per_thread *
				   ((long)chunk_kb / 4);
	printf("  fault-stress x%-2d  %dKB x %d iters  "
	       "%ld page-faults in %lu ns  ->  %s\n",
	       nthreads, chunk_kb, iters_per_thread, pages, total_ns,
	       g_fault_err ? "**CORRUPTION/ERROR**" : "ok");
    }
}

/* ===================================================================
 * Radix overflow capability-table test (#331)
 *
 * Exercises the sparse-name code paths that the normal benchmark does
 * not: allocate_name at a high (sparse) index goes through the radix
 * overflow, a colliding index must be rejected with KERN_NAME_EXISTS,
 * and destroy must free the radix entry so the name can be re-used.
 * =================================================================== */

static void
test_radix_overflow(void)
{
    kern_return_t	kr;
    mach_port_t		self = mach_task_self();
    unsigned int	sparse = 0x100000;	/* 1M: well above the table */
    mach_port_t		name_a = (mach_port_t)((sparse << 8) | 0x11);
    mach_port_t		name_b = (mach_port_t)((sparse << 8) | 0x22); /* same idx, diff gen */
    mach_port_type_t	type;
    int			ok = 1;
    int			i;
    mach_port_t		names[8];

    /* 1) sparse allocate -> radix insert */
    kr = mach_port_allocate_name(self, MACH_PORT_RIGHT_RECEIVE, name_a);
    if (kr != KERN_SUCCESS) { printf("  radix: alloc sparse failed %d\n", kr); ok = 0; }

    /* 2) it must be found -> radix lookup */
    kr = mach_port_type(self, name_a, &type);
    if (kr != KERN_SUCCESS) { printf("  radix: type(sparse) failed %d\n", kr); ok = 0; }

    /* 3) colliding name (same index, different gen) -> KERN_NAME_EXISTS */
    kr = mach_port_allocate_name(self, MACH_PORT_RIGHT_RECEIVE, name_b);
    if (kr != KERN_NAME_EXISTS) {
	printf("  radix: collision expected KERN_NAME_EXISTS, got %d\n", kr);
	ok = 0;
    }

    /* 4) destroy -> radix delete */
    kr = mach_port_destroy(self, name_a);
    if (kr != KERN_SUCCESS) { printf("  radix: destroy(sparse) failed %d\n", kr); ok = 0; }

    /* 5) re-allocate the same name -> radix re-insert after delete */
    kr = mach_port_allocate_name(self, MACH_PORT_RIGHT_RECEIVE, name_a);
    if (kr != KERN_SUCCESS) { printf("  radix: re-alloc failed %d\n", kr); ok = 0; }
    (void) mach_port_destroy(self, name_a);

    /* 6) several sparse indices in different radix subtrees, then verify */
    for (i = 0; i < 8; i++) {
	names[i] = (mach_port_t)(((sparse + (i * 0x40000)) << 8) | 0x33);
	kr = mach_port_allocate_name(self, MACH_PORT_RIGHT_RECEIVE, names[i]);
	if (kr != KERN_SUCCESS) { printf("  radix: multi alloc[%d] failed %d\n", i, kr); ok = 0; }
    }
    for (i = 0; i < 8; i++) {
	kr = mach_port_type(self, names[i], &type);
	if (kr != KERN_SUCCESS) { printf("  radix: multi type[%d] failed %d\n", i, kr); ok = 0; }
    }
    for (i = 0; i < 8; i++)
	(void) mach_port_destroy(self, names[i]);

    printf("  radix overflow test: %s\n", ok ? "PASS" : "FAIL");
}

/* ===================================================================
 * Main
 * =================================================================== */

int
main(int argc, char **argv)
{
    kern_return_t	kr;
    unsigned int	suites;

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
    /*
     * 🔴 AND GIVEN BACK (#511).  That port is bus authority -- configuration
     * space, MMIO, DMA, interrupts -- and this task wanted it to open a
     * console.  See the note above printf_init() in libmach/printf.c.
     */
    (void) mach_port_deallocate(mach_task_self(), device_port);
    panic_init(host_port);

    /* Mirror printf onto gpu_server's text plane (#222 follow-up).
     * Async because servers launch in parallel and gpu_server may not
     * be netname-registered yet when this main() runs. */
    (void)gpu_console_init_async("bench", 100u, 50u);

    suites = parse_suites(argc, argv);

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
     * Raw syscall benchmarks — measure SYSENTER/SYSEXIT cost
     * --------------------------------------------------------- */
    if (suites & SUITE_KRPC) {
	printf("--- Kernel RPC (where the MIG checks are) ---\n");
	bench_kernel_rpc(SYSCALL_BENCH_ITERS);
	printf("\n");
    }

    if (suites & SUITE_SYSCALL) {
	printf("--- Raw syscall (no IPC) ---\n");

	bench_mach_null(SYSCALL_BENCH_ITERS);
	bench_mach_print(SYSCALL_PRINT_ITERS);

	printf("\n");
    }

    /*
     * 🔑 WHAT WILL AND WILL NOT RUN, SAID BEFORE ANY OF IT DOES (#552).
     *
     * «Benchmark complete» on its own is the sentence a reader takes for "all
     * of it ran", and the selector mask means that is usually false.  A suite
     * that prints nothing cannot be told from a suite that passed.
     *
     * 🔴 EARLY, BECAUSE A CUT RUN IS WHEN IT IS NEEDED -- but not FIRST, and
     * where it goes was measured rather than chosen.
     *
     * Version one sat above «Benchmark complete», which a cut run never
     * reaches: with ipc_bench in the default x86-64 bundle the harness ends the
     * run at cow_test's verdict (run-x86_64.sh: "cow_test ends the run, because
     * it is the LAST ENTRY IN THE BUNDLE") and the benchmark died after 18 of
     * its 67 lines.
     *
     * 🔥 Version two sat at the very top, and NEVER PRINTED.  Four boots to
     * establish what it is not: not the length (a 74-character line vanished,
     * and two 40-character ones vanished too), not the assembly across calls
     * (one printf per line vanished the same way), not the format (two CONSTANT
     * strings with no arguments vanished as well).  What survives is only the
     * LAST call before the next output, and the number lost grows with the
     * number added -- the accumulated text is discarded rather than written,
     * which is the mechanism #510 describes and had no confirmed specimen of.
     * Reported there.
     *
     * ⚠️ So it goes after the first two suites, where this task's output
     * demonstrably arrives: early enough for a cut run, late enough to exist.
     * Not a workaround for #510 -- that is a defect in printf -- but a
     * statement about where a line can be relied on until it is fixed.
     */
    {
	static const struct { unsigned bit; const char *name; } all_suites[] = {
	    { SUITE_KRPC,    "krpc" },    { SUITE_SYSCALL, "syscall" },
	    { SUITE_INTRA,   "intra" },   { SUITE_SLOW,    "slow" },
	    { SUITE_INTER,   "inter" },   { SUITE_COMB,    "comb" },
	    { SUITE_PORT,    "port" },    { SUITE_PORTS,   "ports" },
	    { SUITE_PP,      "pp" },      { SUITE_OOL,     "ool" },
	    { SUITE_MEM,     "mem" },     { SUITE_DISK,    "disk" },
	    { SUITE_FLIPC2,  "flipc2" },  { SUITE_CC,      "cc" },
	    { SUITE_FAULT,   "fault" },   { SUITE_SCALE,   "scale" },
	    { SUITE_ISWEEP,  "isweep" }, { SUITE_FORKRACE, "forkrace" },
	};
	char		yes[160], no[160];	/* yes[] feeds the count only (see below) */
	unsigned	i, ny = 0, nn = 0;

	/*
	 * 🔴 ONE printf PER LINE, ASSEMBLED HERE FIRST.
	 *
	 * The first version built each line from a dozen printf calls -- an
	 * opening fragment, one per name, a closing newline -- and the
	 * "asked for" line NEVER ARRIVED: the log went straight from the kmsg
	 * cache line to "suites NOT asked for".  Measured, not guessed.
	 *
	 * 🔑 A line this console shows is a line one printf wrote.  Three
	 * times today a bench figure came out with another task's text spliced
	 * into it -- «ush$ null RPC», «ush: bound ctty ... null RPC» -- and
	 * #510 is open on printf dropping output on a device error while
	 * reporting the length as if it had not.  Anything assembled across
	 * calls is exposed to both.
	 */
	yes[0] = no[0] = '\0';
	for (i = 0; i < sizeof(all_suites)/sizeof(all_suites[0]); i++) {
	    char       *dst = (suites & all_suites[i].bit) ? yes : no;
	    unsigned   *n   = (suites & all_suites[i].bit) ? &ny : &nn;
	    const char *src = all_suites[i].name;
	    unsigned    len = 0;

	    while (dst[len] != '\0')
		len++;
	    if (len + 12 >= sizeof(yes))	/* both buffers are one size */
		continue;
	    if ((*n)++)
		dst[len++] = ' ';
	    while (*src != '\0')
		dst[len++] = *src++;
	    dst[len] = '\0';
	}

	/*
	 * ⚠️ DISCRIMINATORE #510: la riga lunga stampata in DUE pezzi corti.
	 * Se compaiono entrambi, la causa e' la LUNGHEZZA; se ne manca uno,
	 * non lo e'.  La riga da 74 caratteri e' scomparsa due volte su due
	 * mentre quella da 48 accanto e' sempre arrivata.
	 */
	/*
	 * 🔴 ONE LINE, AND IT IS THE ONE THAT ARRIVES.
	 *
	 * There were two -- "asked for" and "NOT asked for" -- and the first
	 * NEVER PRINTED, wherever it was put.  Five boots to characterise it:
	 * not the length, not the assembly across calls, not the format, and
	 * not the position at the top of main.  Of the pair, only the LAST
	 * survives, and adding a third loses two.  Reported on #510, which
	 * describes that mechanism and had no confirmed specimen.
	 *
	 * 🔑 Dropping it costs nothing, which is why this is not a concession:
	 * the suites that DID run print their own results, so listing them adds
	 * nothing a reader cannot see.  What cannot be seen is what is missing,
	 * and that is the line kept.  A second line that silently does not
	 * arrive would be worse than no second line -- it would read as
	 * coverage.
	 */
	printf("--- suites NOT run: %s\n",
	       nn ? no : "(none, every suite was asked for)");

	/*
	 * ⚠️ `scale' gets a line of its own, because it is not merely one of
	 * sixteen: on one processor it stops making progress at thr=24 (#556).
	 * A reader who asks for it and gets a wedge should find the issue
	 * number here rather than in a CMakeLists.
	 */
	if (suites & SUITE_SCALE)
	    printf("--- note: scale wedges at thr=24 on a uniprocessor (#556)\n");
    }

    /* ---------------------------------------------------------
     * Intra-task benchmarks (thread-to-thread, same address space)
     * --------------------------------------------------------- */
    if (suites & SUITE_INTRA) {
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
    }

    /* ---------------------------------------------------------
     * Slow-path receive: receiver always blocked before send.
     * --------------------------------------------------------- */
    if (suites & SUITE_SLOW) {
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
    }

    /* ---------------------------------------------------------
     * Inter-task benchmarks (task-to-task, separate address spaces)
     * --------------------------------------------------------- */
    if (suites & SUITE_INTER) {
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
    }

    /* ---------------------------------------------------------
     * Concurrent task_create/task_terminate (#558) -- a provoker,
     * not a benchmark: it prints a count so that "it forked nothing"
     * cannot be read as "it forked and found nothing".
     * --------------------------------------------------------- */
    if (suites & SUITE_FORKRACE) {
	printf("--- Concurrent fork/destroy (#558) ---\n");

	bench_forkrace();

	printf("\n");
    }

    /* ---------------------------------------------------------
     * Inter-task swept by size, repeated within this boot (#446)
     * --------------------------------------------------------- */
    if (suites & SUITE_ISWEEP) {
	printf("--- Inter-task size sweep (%d repeats in this boot) ---\n",
	       ISWEEP_REPS);

	bench_inter_sweep(BENCH_ITERS);

	printf("\n");
    }

    /* ---------------------------------------------------------
     * Combined-trap (SEND|RCV) RPC -- exercises the mach_msg hotpath [#320]
     * --------------------------------------------------------- */
    if (suites & SUITE_COMB) {
	printf("--- Combined SEND|RCV intra-task (hotpath) ---\n");

	bench_combined_rpc("null RPC",
			   (int)sizeof(bench_null_msg_t), BENCH_ITERS);
	bench_combined_rpc("128B inline RPC",
			   (int)sizeof(bench_128_msg_t), BENCH_ITERS);
	bench_combined_rpc("1024B inline RPC",
			   (int)sizeof(bench_1024_msg_t), BENCH_ITERS);
	bench_combined_rpc("4096B inline RPC",
			   (int)sizeof(bench_4096_msg_t), BENCH_ITERS);

	printf("\n");
    }

    /* ---------------------------------------------------------
     * Concurrent same-space RPC -- N threads, one ipc_space [#327]
     * Exposes the ipc_space read/write lock contention; watch the
     * aggregate ns/RPC as the thread count scales x1 -> x4.
     * --------------------------------------------------------- */
    if (suites & SUITE_CC) {
	printf("--- Concurrent same-space RPC (ipc_space lock, #327) ---\n");

	bench_concurrent_samespace(1, (int)sizeof(bench_null_msg_t),
				   BENCH_ITERS);
	bench_concurrent_samespace(2, (int)sizeof(bench_null_msg_t),
				   BENCH_ITERS);
	bench_concurrent_samespace(4, (int)sizeof(bench_null_msg_t),
				   BENCH_ITERS);

	printf("\n");
    }

    if (suites & SUITE_FAULT) {
	printf("--- pmap fault stress "
	       "(#338: concurrent same-space page faults) ---\n");
	printf("    completion = no deadlock;  'ok' = no corruption\n");
	bench_fault_stress(16, 2048, 256);	/* 16 thr, 2 MB chunks */
	bench_fault_stress(32, 1024, 256);	/* 32 thr (4x oversub), 1 MB */
	printf("\n");
    }

    if (suites & SUITE_SCALE) {
	printf("--- scaling sweep (#319: concurrent same-space RPC, "
	       "median of %d) ---\n", SCALE_RUNS);
	printf("    curve for the CURRENT cpu count (boot -cN); "
	       "speedup<T => runq/scheduler contention\n");
	bench_scale();
	printf("\n");
    }

    /* ---------------------------------------------------------
     * Port operation benchmarks
     * --------------------------------------------------------- */
    if (suites & SUITE_PORT) {
	printf("--- Port operations ---\n");

	bench_port_alloc_destroy(BENCH_ITERS);
	bench_port_names(BENCH_ITERS);
	test_radix_overflow();		/* #331 sparse/collision paths */
    }

    /* ---------------------------------------------------------
     * Protected payload test + benchmarks
     * --------------------------------------------------------- */
    if (suites & SUITE_PP) {
	printf("--- Protected payload ---\n");
	test_protected_payload();

	printf("--- PP intra-task ---\n");
	bench_pp_intra("null (no PP)",
		       (int)sizeof(bench_null_msg_t), 0, BENCH_ITERS);
	bench_pp_intra("null (w/ PP)",
		       (int)sizeof(bench_null_msg_t), 1, BENCH_ITERS);
	bench_pp_intra("128B (no PP)",
		       (int)sizeof(bench_128_msg_t), 0, BENCH_ITERS);
	bench_pp_intra("128B (w/ PP)",
		       (int)sizeof(bench_128_msg_t), 1, BENCH_ITERS);
	bench_pp_intra("1024B (no PP)",
		       (int)sizeof(bench_1024_msg_t), 0, BENCH_ITERS);
	bench_pp_intra("1024B (w/ PP)",
		       (int)sizeof(bench_1024_msg_t), 1, BENCH_ITERS);
	bench_pp_intra("4096B (no PP)",
		       (int)sizeof(bench_4096_msg_t), 0, BENCH_ITERS);
	bench_pp_intra("4096B (w/ PP)",
		       (int)sizeof(bench_4096_msg_t), 1, BENCH_ITERS);

	printf("--- PP inter-task ---\n");
	bench_pp_inter("null (no PP)",
		       (int)sizeof(bench_null_msg_t), 0, BENCH_ITERS);
	bench_pp_inter("null (w/ PP)",
		       (int)sizeof(bench_null_msg_t), 1, BENCH_ITERS);
	bench_pp_inter("128B (no PP)",
		       (int)sizeof(bench_128_msg_t), 0, BENCH_ITERS);
	bench_pp_inter("128B (w/ PP)",
		       (int)sizeof(bench_128_msg_t), 1, BENCH_ITERS);
	bench_pp_inter("1024B (no PP)",
		       (int)sizeof(bench_1024_msg_t), 0, BENCH_ITERS);
	bench_pp_inter("1024B (w/ PP)",
		       (int)sizeof(bench_1024_msg_t), 1, BENCH_ITERS);
	bench_pp_inter("4096B (no PP)",
		       (int)sizeof(bench_4096_msg_t), 0, BENCH_ITERS);
	bench_pp_inter("4096B (w/ PP)",
		       (int)sizeof(bench_4096_msg_t), 1, BENCH_ITERS);
    }

    /* ---------------------------------------------------------
     * OOL (out-of-line) data benchmarks
     * --------------------------------------------------------- */
    /* -----------------------------------------------------------
     * Out-of-line port arrays, by count (#415)
     * ----------------------------------------------------------- */
    if (suites & SUITE_PORTS) {
	printf("--- Out-of-line port arrays (intra-task) ---\n");
	bench_ool_ports_sweep();
	printf("\n");
    }

    if (suites & SUITE_OOL) {
	printf("--- OOL data (intra-task, PHYSICAL_COPY) ---\n");

	bench_ool_intra_rpc("4 KB OOL",   4096, OOL_BENCH_ITERS);
	bench_ool_intra_rpc("16 KB OOL", 16384, OOL_BENCH_ITERS);
	bench_ool_intra_rpc("64 KB OOL", 65536, OOL_BENCH_ITERS);

	printf("--- OOL data (inter-task, PHYSICAL_COPY) ---\n");

	bench_ool_inter_rpc("4 KB OOL inter",   4096, OOL_BENCH_ITERS);
	bench_ool_inter_rpc("16 KB OOL inter", 16384, OOL_BENCH_ITERS);
	bench_ool_inter_rpc("64 KB OOL inter", 65536, OOL_BENCH_ITERS);
    }

    /* ---------------------------------------------------------
     * Disk I/O benchmarks
     * --------------------------------------------------------- */
    if (suites & SUITE_DISK) {
	bench_disk_run(host_port, clock_port);
	/* #232 FLIPC v2 read/write A/B moved to the standalone flipc_bench
	 * binary (#272), launched from the shell on a quiescent system. */
    }

    /* ---------------------------------------------------------
     * Memory bandwidth benchmarks
     * --------------------------------------------------------- */
    if (suites & SUITE_MEM)
	bench_mem_run(clock_port);

    /* ---------------------------------------------------------
     * FLIPC v2 shared-memory channel benchmarks
     * --------------------------------------------------------- */
    if (suites & SUITE_FLIPC2)
	bench_flipc2_run(clock_port, host_port);

    /* libvfs smoke test (#220 v0.1) — hangs off the FLIPC suite gate
     * for now; pure correctness, not a perf bench. */
    if (suites & SUITE_FLIPC2) {
	/*
	 * #552: libvfs does not build for x86-64 yet -- migcom sums field
	 * sizes without alignment padding and fs_stat's reply comes out four
	 * bytes short at -m64 (#553).  ⚠️ SAID rather than skipped: a suite
	 * that prints nothing cannot be told from one that passed.
	 */
#if defined(__x86_64__)
	printf("\n--- libvfs smoke: SKIPPED, libvfs is not built for x86-64 "
	       "(#553) ---\n");
#else
	extern void bench_libvfs_smoke(void);
	bench_libvfs_smoke();
#endif
    }

    /* exec_server smoke test (#228 v0.1.0) — same FLIPC gate. */
    if (suites & SUITE_FLIPC2) {
	extern void bench_exec_smoke(void);
	bench_exec_smoke();
    }

    /* proc_server smoke test (#237 v0.1.0) — same FLIPC gate. */
    if (suites & SUITE_FLIPC2) {
#if defined(__x86_64__)
	printf("\n--- proc smoke: SKIPPED, it links libvfs, which is not built "
	       "for x86-64 (#553) ---\n");
#else
	extern void bench_proc_smoke(void);
	bench_proc_smoke();
#endif
    }

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
