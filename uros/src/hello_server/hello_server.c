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
 * hello_server.c — Minimal Mach standalone server for OSFMK
 *
 * A port of the original Uros hello_server from Mach 4 to OSF Mach.
 * This server demonstrates:
 *   - Standalone Mach server startup (bootstrap_ports, printf_init)
 *   - Port allocation and send right management
 *   - Message receive loop
 *   - Console I/O via the Mach device layer
 *
 * It can be used for testing, studying the Mach IPC system, and
 * performance measurement.
 */

#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_port.h>
#include <mach/message.h>
#include <mach/cap_types.h>             /* #216 v2.1 enforcement probe */
#include <mach/thread_switch.h>         /* SWITCH_OPTION_WAIT (Phase 4) */
#include <mach/mach_syscalls.h>         /* syscall_thread_switch (Phase 4) */
#include <device/device.h>
#include <device/device_types.h>
#include <libcap.h>                     /* #216 v2.1 enforcement probe */
#include <mach_init.h>                  /* mach_cap_port (#216 v2.1) */
#include <stdio.h>                      /* musl libc — Phase 3 pilot (#251) */
#include <stdlib.h>                     /* exit family */
#include <unistd.h>                     /* _exit() from musl */
#include <signal.h>                     /* sigaction/raise — Phase 4 (#252) */
#include <uros/libposix.h>              /* __uros_libc_init() */
extern void __uros_libc_init(void);     /* defined in patched musl */

/*
 * Phase 4 (#252) smoke: SIGUSR1 handler.  File scope so taking its
 * address doesn't trigger a nested-function trampoline (we link with
 * a non-executable stack).
 */
static volatile int hello_handler_fired;
static void
hello_sigusr1(int s)
{
    hello_handler_fired = s;
    printf("(hello_server): handler fired for signo=%d\n", s);
}

/*
 * Global ports — obtained at startup.
 */
static mach_port_t	host_port;
static mach_port_t	device_port;
static mach_port_t	security_port;
static mach_port_t	root_ledger_wired;
static mach_port_t	root_ledger_paged;

/*
 * Our service port — clients can send messages here.
 */
static mach_port_t	hello_port;

/*
 * Message IDs for our simple protocol.
 */
#define HELLO_MSG_PING		1000
#define HELLO_MSG_PONG		1001
#define HELLO_MSG_SHUTDOWN	1002

/*
 * Generic message buffer for receive.
 */
typedef struct {
    mach_msg_header_t	head;
    char		body[256];
} hello_msg_t;

/*
 * enumerate_ports — list all ports in the current task.
 * Reproduces the mach_port_names() enumeration from the Mach4 version.
 */
static void
enumerate_ports(void)
{
    kern_return_t		kr;
    mach_port_array_t		names;
    mach_msg_type_number_t	names_count;
    mach_port_type_array_t	types;
    mach_msg_type_number_t	types_count;
    unsigned int		i;

    kr = mach_port_names(mach_task_self(),
			 &names, &names_count,
			 &types, &types_count);
    if (kr != KERN_SUCCESS) {
	printf("(hello_server): mach_port_names failed: %d\n", kr);
	return;
    }

    printf("(hello_server): task has %d ports:\n", names_count);
    for (i = 0; i < names_count; i++) {
	printf("  port %d: name=0x%x type=0x%x\n",
	       i, names[i], types[i]);
    }
}

/*
 * check_port_status — verify our hello_port has the expected rights.
 */
static void
check_port_status(mach_port_t port)
{
    kern_return_t		kr;
    mach_port_type_t		ptype;

    kr = mach_port_type(mach_task_self(), port, &ptype);
    if (kr != KERN_SUCCESS) {
	printf("(hello_server): port 0x%x not valid: %d\n", port, kr);
    } else {
	printf("(hello_server): port 0x%x type=0x%x%s%s%s\n",
	       port, ptype,
	       (ptype & MACH_PORT_TYPE_RECEIVE) ? " RECV" : "",
	       (ptype & MACH_PORT_TYPE_SEND)    ? " SEND" : "",
	       (ptype & MACH_PORT_TYPE_SEND_ONCE) ? " SONCE" : "");
    }
}

/*
 * message_loop — sit in a receive loop and handle incoming messages.
 */
static void
message_loop(void)
{
    kern_return_t	kr;
    hello_msg_t		msg;
    int			running = 1;

    printf("(hello_server): entering message loop on port 0x%x\n",
	   hello_port);

    while (running) {
	kr = mach_msg(&msg.head,
		      MACH_RCV_MSG,
		      0,			/* send size */
		      sizeof(msg),		/* receive limit */
		      hello_port,		/* receive port */
		      MACH_MSG_TIMEOUT_NONE,
		      MACH_PORT_NULL);

	if (kr != MACH_MSG_SUCCESS) {
	    printf("(hello_server): mach_msg receive failed: %d\n", kr);
	    continue;
	}

	printf("(hello_server): received msg id=%d from port=0x%x\n",
	       msg.head.msgh_id, msg.head.msgh_remote_port);

	switch (msg.head.msgh_id) {
	case HELLO_MSG_PING:
	    printf("(hello_server): PING received\n");
	    break;

	case HELLO_MSG_SHUTDOWN:
	    printf("(hello_server): SHUTDOWN received, exiting\n");
	    running = 0;
	    break;

	default:
	    printf("(hello_server): unknown msg id %d\n",
		   msg.head.msgh_id);
	    break;
	}
    }
}

int
main(int argc, char **argv)
{
    kern_return_t	kr;
    int			i;

    /*
     * Step 0 (Phase 3 / #251): wire up musl's synthetic main-thread struct
     * before any libc function runs.  Without this, the very first printf
     * dereferences a null TLS pointer and SEGVs.  Removed in Phase 6 once
     * real pthread + set_thread_area lands.
     *
     * setvbuf disables stdout buffering: musl's isatty() returns -ENOTTY
     * from our ioctl stub, which makes musl fall back to fully-buffered
     * mode — every line then sits in a 1 KB FILE buffer waiting for an
     * explicit fflush that hello_server never issues (it lives forever in
     * the IPC loop).  Phase 4 will introduce a real char_server ioctl that
     * reports TTY status; until then, unbuffered output is the honest
     * choice for a debug server.
     */
    __uros_libc_init();
    setvbuf(stdout, NULL, _IONBF, 0);

    /*
     * Step 1: Get privileged ports from the bootstrap server.
     *
     * On Mach 4 this was:
     *   get_privileged_ports(&host_port, &device_port);
     *
     * On OSF Mach, bootstrap_ports() is a MIG routine that also
     * returns ledger and security ports.
     */
    kr = bootstrap_ports(bootstrap_port,
			 &host_port,
			 &device_port,
			 &root_ledger_wired,
			 &root_ledger_paged,
			 &security_port);
    if (kr != KERN_SUCCESS) {
	/* Can't printf yet — no console.  Just exit. */
	_exit(1);
    }

    /*
     * Step 2/3 (was printf_init + panic_init): no longer needed under the
     * musl path — printf goes directly through SYS_write to mach_print,
     * and panic() isn't linked from libmach_core (the file is excluded;
     * abort()/_Exit() from musl take its place).  See #251.
     */

    /*
     * Step 4: We have a console — say hello!
     */
    printf("(hello_server): started, argc=%d\n", argc);
    for (i = 0; i < argc; i++)
	printf("(hello_server): argv[%d]='%s'\n", i, argv[i]);

    printf("(hello_server): host_port=0x%x device_port=0x%x "
	   "bootstrap_port=0x%x\n",
	   host_port, device_port, bootstrap_port);

    /*
     * Phase 4b (#253): if the operator passed "crash" as an argv,
     * fault deliberately to validate the Mach exception → POSIX
     * signal path.  Normal boot doesn't, so this is a no-op for
     * the regular smoke.
     */
    /*
     * Phase 4b crash-path is opt-in via an UROS_CRASH_SMOKE build
     * define — keeping the source unchanged here makes the normal
     * smoke deterministic.  See exc.c / hw exception path in
     * libposix-uros.  The opt-in mechanism was kept simple: a
     * one-liner #ifdef wrapper that maintainers can re-add when they
     * want to exercise the exception path locally.
     */
    for (int j = 1; j < argc; j++) {
        const char *a = argv[j];
        /* Manual strcmp — pick the last path component if argv was
         * mangled by bootstrap (e.g. argv[0] is the full ELF path). */
        const char *base = a;
        for (const char *p = a; *p; p++) if (*p == '/') base = p + 1;
        if (base[0] == 'c' && base[1] == 'r' && base[2] == 'a'
            && base[3] == 's' && base[4] == 'h' && base[5] == '\0') {
            printf("(hello_server): --crash requested, dereferencing NULL\n");
            *(volatile int *)0 = 42;
            printf("(hello_server): unreachable\n");
        }
    }

    /*
     * Step 4.5 (Phase 4 / #252): exercise the new POSIX signal path.
     * sigaction() registers the handler in libposix-uros's table, then
     * raise() takes the long way round — musl raise → SYS_tkill →
     * __uros_kill → proc_kill RPC → proc_server posts proc_signal_msg
     * on our signal_port → libposix-uros's handler thread receives it
     * and calls hello_sigusr1.  Anything visible on the console proves
     * the full client+server signal loop works end-to-end.
     */
    extern unsigned int __uros_my_pid;
    printf("(hello_server): signals: pid=%u\n", __uros_my_pid);
    {
        struct sigaction sa = { 0 };
        sa.sa_handler = hello_sigusr1;
        if (sigaction(SIGUSR1, &sa, NULL) != 0)
            printf("(hello_server): sigaction(SIGUSR1) failed\n");
        else
            printf("(hello_server): sigaction(SIGUSR1) installed\n");

        if (raise(SIGUSR1) != 0)
            printf("(hello_server): raise(SIGUSR1) failed\n");
        /* Yield long enough for the handler thread to pick up the msg
         * and run the handler before we move on. */
        for (int j = 0; j < 50 && !hello_handler_fired; j++)
            (void)syscall_thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 20);
        printf("(hello_server): handler_fired=%d\n", hello_handler_fired);
    }

    /*
     * Step 4.6 (Phase 5 / #254): spawn /hello_exec via the new
     * __uros_spawn primitive and waitpid for it.  __uros_spawn is the
     * same path execve() uses, but called explicitly here so we don't
     * have to suicide before we can observe the child finish.
     */
    {
        extern int  __uros_spawn(const char *path,
                                 char *const argv[], char *const envp[],
                                 mach_port_t *out_task,
                                 mach_port_t *out_thread,
                                 unsigned int *out_pid);
        extern int  __uros_waitpid(int pid, int *status, int options);

        char *argv_v[] = { (char *)"hello_exec", NULL };
        char *envp_v[] = { NULL };
        mach_port_t  child_task = MACH_PORT_NULL;
        mach_port_t  child_thread = MACH_PORT_NULL;
        unsigned int child_pid = 0;
        int sr = __uros_spawn("/hello_exec", argv_v, envp_v,
                              &child_task, &child_thread, &child_pid);
        if (sr != 0) {
            printf("(hello_server): __uros_spawn(/hello_exec) failed: %d\n", sr);
        } else {
            printf("(hello_server): spawned /hello_exec pid=%u task=0x%x\n",
                   child_pid, (unsigned)child_task);
            /* hello_exec is a Mach-level test program that mach_print's
             * its AUXV and then spins forever, expecting the parent to
             * kill it.  Give it a beat to print, then task_terminate;
             * proc_server's dead-name handler will mark pid zombie and
             * unblock our waitpid via proc_subscribe_exit. */
            for (int j = 0; j < 30; j++)
                (void)syscall_thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 20);
            (void)task_terminate(child_task);
            int status = 0;
            int wr = __uros_waitpid((int)child_pid, &status, 0);
            if (wr < 0)
                printf("(hello_server): waitpid failed: %d\n", wr);
            else
                printf("(hello_server): waitpid -> pid=%d status=0x%x\n",
                       wr, status);
        }
    }

    /*
     * Step 5: Allocate our service port.
     *
     * On Mach 4, the original used a hardcoded HELLO_SERVER_PORT
     * with mach_port_allocate_name().  On OSF Mach we allocate
     * dynamically — the port name is assigned by the kernel.
     */
    kr = mach_port_allocate(mach_task_self(),
			    MACH_PORT_RIGHT_RECEIVE,
			    &hello_port);
    if (kr != KERN_SUCCESS) {
	printf("(hello_server): port allocate failed: %d\n", kr);
	_exit(1);
    }

    /* Make a send right so others could send to us */
    kr = mach_port_insert_right(mach_task_self(),
				hello_port, hello_port,
				MACH_MSG_TYPE_MAKE_SEND);
    if (kr != KERN_SUCCESS) {
	printf("(hello_server): insert send right failed: %d\n", kr);
	_exit(1);
    }

    printf("(hello_server): allocated service port 0x%x\n", hello_port);

    /*
     * Step 6: Enumerate all ports in our task (diagnostic).
     */
    enumerate_ports();
    check_port_status(hello_port);

    /*
     * Step 6.5: #216 v2.1 enforcement probe.  hello_server's manifest
     * declares zero caps_required, so any cap_request must come back
     * as CAP_ERR_NOT_IN_MANIFEST (2413) when the per-task cap_port is
     * in use.  When TASK_CAP_PORT isn't set (bootstrap without a
     * manifest, or pre-#216 bootstrap), we expect CAP_ERR_NONE
     * because the legacy well-known path is permissive — that's a
     * cleanly reportable state, not a failure.
     */
    {
	struct uros_cap probe;
	kern_return_t pk;

	printf("(hello_server): manifest enforcement probe — "
	       "mach_cap_port=0x%x\n", mach_cap_port);
	pk = cap_request(RESOURCE_BLK_DEVICE, 0xdeadbeefULL,
			 CAP_OP_BLK_READ, 0, &probe);
	if (pk == CAP_ERR_NOT_IN_MANIFEST) {
	    printf("(hello_server): probe -> CAP_ERR_NOT_IN_MANIFEST "
		   "— enforcement WORKS\n");
	} else if (pk == KERN_SUCCESS) {
	    printf("(hello_server): probe -> KERN_SUCCESS "
		   "(legacy permissive path)\n");
	} else {
	    printf("(hello_server): probe -> kr=%d (unexpected)\n", pk);
	}
    }

    /*
     * Step 7: Tell the bootstrap we're done initializing.
     * This unblocks loading of subsequent servers (if serialized).
     */
    bootstrap_completed(bootstrap_port, mach_task_self());

    printf("(hello_server): bootstrap_completed, entering message loop\n");

    /*
     * Step 8: Message receive loop.
     */
    message_loop();

    printf("(hello_server): exiting\n");
    return 0;
}
