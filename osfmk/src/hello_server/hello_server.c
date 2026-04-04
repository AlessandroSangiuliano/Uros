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
#include <sa_mach.h>
#include <pthread.h>
#include <device/device.h>
#include <device/device_types.h>
#include <stdio.h>

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
     * Step 2: Initialize console I/O.
     * This opens the "console" device via device_open() so that
     * printf() can write characters through Mach device_write_inband().
     */
    printf_init(device_port);

    /*
     * Step 3: Initialize panic handler.
     * Stashes the host_priv port so panic() can call host_reboot().
     */
    panic_init(host_port);

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
