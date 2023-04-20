#include "hello.h"
#include <device/device.h>
#include <mach.h>

void console_init();
kern_return_t get_privileged_ports(mach_port_t *host_ports, mach_port_t *device_port);

mach_port_t	host_port;
mach_port_t	device_port;
mach_port_t	console_port;

int main (int argc, char **argv)
{
	int err = 0;
    get_privileged_ports (&host_port, &device_port);

    console_init();

    err = printf("Hello from Uros. I was born 'from' here!\n");

	printf("Ciao Iolanda il valore di ritorno è: %d\n,", err);

    return 0;
}

void console_init()
{
    kern_return_t		rc;

	/*
	 * Open the console
	 */
	rc = device_open (device_port, D_READ | D_WRITE, "console", &console_port);
	/*
	 * Call `printf_init' to enable `printf'
	 */
	printf_init (device_port);
}

kern_return_t get_privileged_ports(mach_port_t *host_port, mach_port_t *device_port)
{
    mach_port_t bootstrap_port;
	mach_port_t reply_port;
	kern_return_t kr;

	struct msg {
		mach_msg_header_t hdr;
		mach_msg_type_t port_desc_1;
		mach_port_t port_1;
		mach_msg_type_t port_desc_2;
		mach_port_t port_2;
	} msg;

	/*
	 * Get our bootstrap port.
	 */

	kr = task_get_bootstrap_port(mach_task_self(), &bootstrap_port);
	if (kr != KERN_SUCCESS)
		return kr;

	/*
	 * Allocate a reply port.
	 */

	reply_port = mig_get_reply_port();
	if (!MACH_PORT_VALID(reply_port))
		return KERN_FAILURE;

	/*
	 * Ask for the host and device ports.
	 */

	msg.hdr.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
					   MACH_MSG_TYPE_MAKE_SEND_ONCE);
	msg.hdr.msgh_remote_port = bootstrap_port;
	msg.hdr.msgh_local_port = reply_port;
	msg.hdr.msgh_id = 999999;

	kr = mach_msg(&msg.hdr, MACH_SEND_MSG|MACH_RCV_MSG,
		      sizeof msg.hdr, sizeof msg, reply_port,
		      MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS)
		return kr;

	*host_port = msg.port_1;
	*device_port = msg.port_2;
	return KERN_SUCCESS;
}
