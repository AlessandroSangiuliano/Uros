/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * char_server/klog_forward.c — the kernel's own output, out through the one
 * writer the serial port now has (#497).
 *
 * ── Why this has to exist the moment the port is claimed ──
 *
 * 🔴 A DRIVER THAT TAKES THE PORT AND FORWARDS NOTHING HAS SILENCED THE
 * MACHINE.  uart_attach() claims COM1 and the kernel's console steps back, so
 * from that instant every line the kernel prints -- and every printf from
 * every other task, because those reach the wire through the kernel's console
 * device -- arrives in the klog ring and on a framebuffer and nowhere else.
 * On a box with a screen that is an inconvenience.  On a headless one (#373)
 * it is a machine that has stopped answering, and an automated run that reads
 * a serial log would report the boot as cut short.
 *
 * 🔑 klog.h has said since #200 that the ring is there to be drained by a
 * userspace forwarder, and one already exists -- bootstrap's, which feeds
 * gpu_server.  That is not this one: gpu_server has not crossed to x86-64, and
 * the destination that matters here is the serial line the kernel just gave
 * up.  Same ring, same RPC, different mouth.
 *
 * ── What it buys besides not going quiet ──
 *
 * One writer touches the chip.  The kernel's bytes and a client's bytes now
 * arrive at the same uart_tty_write(), which serialises them with the flag it
 * has had since #207 -- so the interleaving that costs i386 a quarter of its
 * acceptance smoke (#544) is impossible by construction here rather than
 * unlikely.  That is the whole argument for the handover, and this file is the
 * half that makes it a gain rather than a loss.
 *
 * ⚠️ IT DOES NOT MAKE ORDER PERFECT, AND SAYING SO IS THE POINT.  A line
 * printed by the kernel reaches the wire when this thread next runs, which is
 * after a line a client wrote directly, even if the kernel said it first.
 * Both lines arrive whole -- that is what the single writer guarantees -- but
 * two SOURCES can be transposed.  A byte-accurate order would need the clients
 * to go through the ring as well, and a timestamp is the honest fix if it ever
 * matters; what is fixed is the thing that made #544 a bug, which was lines
 * woven together inside a word, not lines in an unexpected order.
 *
 * ⚠️ And the cursor can be outrun.  klog is 64 KiB and drops rather than
 * blocking printf -- design doc §11.3 rule 2 -- so a burst larger than that
 * between two runs of this thread is lost, and the kernel snaps the cursor
 * forward silently.  That is a property of the ring and not of this file, and
 * it is the same one bootstrap's forwarder has lived with since #200.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_host.h>
#include <mach/host_info.h>
#include <mach/mach_klog.h>
#include <mach/thread_switch.h>

#include <pthread.h>
#include <stdio.h>

#include "char_server.h"

/*
 * How often the ring is looked at.
 *
 * ⚠️ A POLL AND NOT A WAKE-UP, because klog has no way to say "there is
 * something".  Ten milliseconds is bootstrap's number for the same ring, and
 * the cost of being wrong in the cheap direction is a thread that wakes a
 * hundred times a second to find nothing; the cost of being wrong in the
 * other is a 64 KiB ring wrapping between two looks.
 */
#define	KLOG_POLL_MS	10

static pthread_t	klog_tid;
static int		klog_started;

/*
 * 🔴 THIS FILE MAY NOT USE printf, AND THE REASON IS THE FILE'S SUBJECT.
 *
 * printf reaches the wire through the kernel's console device, and by the
 * time anything here runs the kernel has given that wire away -- to us.  So a
 * diagnostic printed the ordinary way lands in the ring this thread is
 * supposed to be draining, and if the drain is what failed, the message
 * saying so is lost by the same failure it reports.
 *
 * A message about the forwarder goes out the way the forwarder does.
 */
static void
say(const char *s)
{
	size_t n = 0;

	while (s[n] != '\0')
		n++;
	(void)char_core_tty_write_raw(s, n);
}

static void *
klog_forward_thread(void *arg)
{
	natural_t	cursor = 0;
	mach_port_t	host = mach_host_self();
	klog_data_t	buf;
	int		complained = 0;

	(void)arg;

	/*
	 * 🔥 SKIP WHAT IS ALREADY OUT, AND THIS WAS NOT OBVIOUS UNTIL IT WAS
	 * SEEN.  A cursor of 0 means "the oldest byte still in the ring", and
	 * the ring holds 64 KiB -- an entire boot's output, every line of
	 * which has ALREADY left through the port while the kernel still owned
	 * it.  So the first thing this thread did was send the whole boot down
	 * the wire a second time, at eighty microseconds a byte, burying the
	 * lines it exists to deliver under five seconds of replay.
	 *
	 * The drain to the tip is the fix, and it has to be a LOOP: one call
	 * returns at most 4 KiB, so a single call would skip a sixteenth of the
	 * backlog and dutifully repeat the rest.
	 */
	for (;;) {
		mach_msg_type_number_t	cnt = sizeof(buf);
		natural_t		next;

		if (host_get_log(host, cursor, buf, &cnt, &next)
		    != KERN_SUCCESS)
			break;
		if (cnt == 0)
			break;
		cursor = next;
	}

	for (;;) {
		mach_msg_type_number_t	cnt = sizeof(buf);
		natural_t		next;
		kern_return_t		kr;

		kr = host_get_log(host, cursor, buf, &cnt, &next);
		if (kr != KERN_SUCCESS && !complained) {
			complained = 1;
			say("char_server: host_get_log refused; the kernel's "
			    "output stops here (#497)\r\n");
		}
		if (kr == KERN_SUCCESS && cnt > 0) {
			/*
			 * ⚠️ The cursor advances whether or not the bytes
			 * reached the wire.  A tty that refuses -- a write in
			 * flight, a port that went away -- must not make this
			 * thread re-read the same bytes for ever, which would
			 * turn one lost line into a loop that never catches
			 * up and never lets go of the ring.
			 */
			(void)char_core_tty_write_raw(buf, (size_t)cnt);
			cursor = next;
		}
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT,
			      KLOG_POLL_MS);
	}
	return NULL;
}

/*
 * Start the drain.  Called once, after discovery, and only when a tty is
 * attached -- there is nowhere to forward to otherwise, and a thread spinning
 * on a ring nobody reads from is worse than no thread.
 */
void
char_klog_forward_start(void)
{
	int err;

	if (klog_started)
		return;

	err = pthread_create(&klog_tid, NULL, klog_forward_thread, NULL);
	if (err != 0) {
		say("char_server: klog forwarder failed to start — the "
		    "kernel's output will not reach this line while this "
		    "server holds it (#497)\r\n");
		return;
	}

	klog_started = 1;
	say("char_server: forwarding the kernel's klog to the tty it now "
	    "owns (#497)\r\n");
}
