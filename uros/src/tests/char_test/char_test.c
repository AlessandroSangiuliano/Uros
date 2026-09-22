/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * One character out and one character in, through char_server (#497).
 *
 * ── What this is for ──
 *
 * #497's second clause asks for a character each way "demonstrated rather
 * than assumed", and until this file there was nothing on x86-64 to ask with:
 * char_server publishes "char" and nobody looks it up.  The server starting
 * is not the claim.  A driver that attaches, reports a chip and moves no byte
 * is exactly the shape of a stub with a plausible return.
 *
 * So this goes the whole way: name server, cap_server, the device list,
 * the cap-checked open, and then the two directions through the MIG surface
 * a real client would use -- char_tty_write and char_tty_read.
 *
 * ── The third word, and why this test needs it (#563) ──
 *
 * 🔴 THE INPUT HALF DEPENDS ON SOMEBODY TYPING.  An unattended boot has no
 * operator and the harness may or may not send a byte, so "no character
 * arrived" is not a failure -- it is the question never having been put.  A
 * test that called that WRONG would fail every ordinary boot, and would teach
 * its reader to stop reading it, which is what #451 and #563 are both about.
 *
 * Hence three outcomes, and the input arm names them:
 *
 *	PASS		a byte was typed and came back through char_server
 *	NOT ASKED	nothing was typed; the path was never exercised
 *	WRONG		a byte was typed and what came back was not it
 *
 * ⚠️ And the OUTPUT arm has no third word, deliberately.  Nothing outside
 * this task is needed for it: the write either reached the wire or it did
 * not, so declining is not available to it and offering the word would only
 * give a failure somewhere to hide.
 *
 * ── What this test does NOT show ──
 *
 * ⚠️ That IRQ 4 is delivered.  uart.so drains the chip from its read path as
 * well (#382, defence against a lost edge), so a byte reaches this test
 * identically whether the interrupt arrived or the peek rescued it.  The
 * module counts the two apart and says which carried the first byte; that
 * line, and not this test, is the evidence about the interrupt.
 *
 * ⚠️ That COM1 is char_server's alone.  It is not -- the kernel writes it too,
 * and on this target reads it for the debugger door.  That is #497's third
 * clause and this file has nothing to say about it.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/port.h>
#include <mach/thread_switch.h>

#include <stdio.h>
#include <string.h>

#include <servers/netname.h>
#include <servers/netname_defs.h>

#include <mach/cap_types.h>
#include "libcap.h"

#include "char_server.h"		/* MIG: char_* client stubs */
#include <char/char_types.h>

extern mach_port_t	name_server_port;
extern void		printf_init(mach_port_t device_server_port);

/*
 * How long the input arm waits, and why it is a count of tries and not a
 * clock.  char_tty_read does not block -- it returns whatever the module's
 * ring holds, which is nothing until a byte arrives -- so the wait is a poll,
 * and thread_switch with a 50 ms hint is what every other waiter in this tree
 * uses.  Forty tries is about two seconds: long enough for a harness that
 * types after the banner, short enough that an unattended boot is not held up
 * by an arm that is going to decline.
 */
#define	RX_TRIES	40u
#define	RX_WAIT_MS	50

/* The byte the harness is asked to type.  Printable, not a control
 * character: ^C and ^Z are eaten by the line discipline (#397) and ^D is the
 * debugger break (#382), so any of those would test the wrong thing. */
#define	RX_EXPECT	'u'

static mach_port_t
wait_for_service(const char *name)
{
	mach_port_t	port = MACH_PORT_NULL;
	unsigned int	tries;

	for (tries = 0; tries < 200u; tries++) {
		if (netname_look_up(name_server_port, "", (char *)name,
				    &port) == KERN_SUCCESS)
			return port;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 50);
	}
	return MACH_PORT_NULL;
}

/*
 * Find the tty among whatever char_server has attached.
 *
 * ⚠️ ASKED, not assumed to be device 1.  The id is assigned in the order
 * modules attach, so bundling ps2.so ahead of uart.so would renumber it --
 * and a test that hard-coded the number would then exercise the keyboard and
 * report about the serial port.
 */
static int
find_tty(mach_port_t char_port, chr_u32_t *dev_id_out)
{
	struct char_device_info	*list;
	vm_offset_t		devices = 0;
	mach_msg_type_number_t	devices_count = 0;
	chr_u32_t		n_devices = 0;
	kern_return_t		kr;
	unsigned int		i;
	int			found = 0;

	kr = char_query_devices(char_port, &devices, &devices_count,
				&n_devices);
	if (kr != KERN_SUCCESS) {
		printf("char_test: query_devices failed (kr=%d)\n", (int)kr);
		return -1;
	}

	list = (struct char_device_info *)devices;
	for (i = 0; i < (unsigned int)n_devices; i++) {
		printf("char_test: device %u is class %u, module \"%s\"\n",
		       (unsigned)list[i].id, (unsigned)list[i].class,
		       list[i].module_name);
		if (!found && list[i].class == CHAR_CLASS_TTY) {
			*dev_id_out = (chr_u32_t)list[i].id;
			found = 1;
		}
	}

	(void)vm_deallocate(mach_task_self(), devices,
			    (vm_size_t)devices_count);
	return found ? 0 : -1;
}

int
main(int argc, char **argv)
{
	mach_port_t		char_port;
	struct uros_cap		tok;
	chr_u32_t		dev_id = 0;
	kern_return_t		kr;
	int			result = 0;
	int			passed = 0, arms = 0;
	char			out[64];
	char			in[CHR_BUF_MAX];
	mach_msg_type_number_t	in_count;
	unsigned int		tries;

	(void)argc;
	(void)argv;

	printf("char_test: started (#497)\n");

	if (name_server_port == MACH_PORT_NULL) {
		printf("char_test: WRONG — no name server port in this task's "
		       "registered ports, so char_server cannot be reached "
		       "from here at all\n");
		printf("char_test: 0 of 2 arms passed\n");
		return 1;
	}

	char_port = wait_for_service("char");
	if (char_port == MACH_PORT_NULL) {
		printf("char_test: WRONG — \"char\" never appeared in the "
		       "name server; char_server did not publish itself\n");
		printf("char_test: 0 of 2 arms passed\n");
		return 1;
	}

	if (wait_for_service("cap_server") == MACH_PORT_NULL) {
		printf("char_test: WRONG — cap_server never appeared, so no "
		       "capability can be issued for the tty\n");
		printf("char_test: 0 of 2 arms passed\n");
		return 1;
	}

	if (find_tty(char_port, &dev_id) < 0) {
		printf("char_test: WRONG — char_server attached no device of "
		       "class TTY, so there is nothing to move a byte "
		       "through\n");
		printf("char_test: 0 of 2 arms passed\n");
		return 1;
	}
	printf("char_test: the tty is device %u\n", (unsigned)dev_id);

	/*
	 * The capability.  RESOURCE_SERIAL names the kind, dev_id names the
	 * instance, and CHAR_CAP_TTY_RW is the pair of operations below --
	 * which is the whole of what this task is provisioned for by
	 * char_test.manifest.  A refusal here is a real failure and not a
	 * reason to carry on without one: char_server's read and write both
	 * verify the token, so a test that proceeded with an empty one would
	 * be testing the refusal path and calling it the tty.
	 */
	memset(&tok, 0, sizeof(tok));
	kr = cap_request(RESOURCE_SERIAL, (uint64_t)dev_id,
			 CHAR_CAP_TTY_RW, 0, &tok);
	if (kr != KERN_SUCCESS) {
		printf("char_test: WRONG — cap_request(RESOURCE_SERIAL, "
		       "dev=%u, TTY_RW) refused (kr=%d)\n",
		       (unsigned)dev_id, (int)kr);
		printf("char_test: 0 of 2 arms passed\n");
		return 1;
	}

	kr = char_device_open(char_port, (const char *)&tok,
			      (mach_msg_type_number_t)sizeof(tok), dev_id);
	if (kr != KERN_SUCCESS) {
		printf("char_test: WRONG — device_open(dev=%u) refused "
		       "(kr=%d)\n", (unsigned)dev_id, (int)kr);
		printf("char_test: 0 of 2 arms passed\n");
		return 1;
	}

	/*
	 * [1] OUT.  The line below leaves this task, crosses the MIG surface,
	 * reaches uart.so, and becomes bytes in a 16550's transmitter -- on
	 * x86-64 one device_io_port_write per byte, executed by the kernel
	 * because ring 3 may not.  Seeing it on the console IS the arm; there
	 * is no reply that could stand in for it, which is why the text says
	 * so on the wire itself.
	 */
	arms++;
	strcpy(out, "char_test: [1] this line left through char_server\r\n");
	result = 0;
	kr = char_tty_write(char_port, (const char *)&tok,
			    (mach_msg_type_number_t)sizeof(tok), dev_id, 0,
			    out, (mach_msg_type_number_t)strlen(out),
			    &result);
	if (kr == KERN_SUCCESS && result == CHR_OK) {
		printf("char_test: [1] tty_write accepted %u bytes — the line "
		       "above this one is what came out of the port\n",
		       (unsigned)strlen(out));
		passed++;
	} else {
		printf("char_test: [1] WRONG — tty_write kr=%d result=%d\n",
		       (int)kr, result);
	}

	/*
	 * [2] IN.  Poll until a byte shows up or the budget runs out, and
	 * say which of the three happened.
	 */
	arms++;
	in_count = 0;
	for (tries = 0; tries < RX_TRIES; tries++) {
		in_count = (mach_msg_type_number_t)sizeof(in);
		result = 0;
		kr = char_tty_read(char_port, (const char *)&tok,
				   (mach_msg_type_number_t)sizeof(tok),
				   dev_id, 0, (chr_u32_t)sizeof(in),
				   in, &in_count, &result);
		if (kr != KERN_SUCCESS) {
			printf("char_test: [2] WRONG — tty_read kr=%d\n",
			       (int)kr);
			in_count = 0;
			break;
		}
		if (result != CHR_OK) {
			printf("char_test: [2] WRONG — tty_read result=%d\n",
			       result);
			in_count = 0;
			break;
		}
		if (in_count > 0)
			break;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, RX_WAIT_MS);
	}

	if (kr != KERN_SUCCESS || result != CHR_OK) {
		/* already reported above */
	} else if (in_count == 0) {
		printf("char_test: [2] NOT ASKED — nothing was typed in %u "
		       "tries, so the input path was never exercised.  Send a "
		       "byte on the serial line to ask it (#563)\n",
		       RX_TRIES);
		arms--;
	} else if (in[0] == RX_EXPECT) {
		printf("char_test: [2] read back '%c' — a character went in "
		       "through char_server and the same one came out of "
		       "tty_read\n", in[0]);
		passed++;
	} else {
		printf("char_test: [2] WRONG — expected '%c', tty_read "
		       "returned '%c' (0x%02x) first of %u bytes\n",
		       RX_EXPECT, in[0], (unsigned)(unsigned char)in[0],
		       (unsigned)in_count);
	}

	printf("char_test: %d of %d arms passed\n", passed, arms);
	return (passed == arms) ? 0 : 1;
}
