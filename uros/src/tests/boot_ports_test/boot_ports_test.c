/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Who bootstrap gives the master ports to (#620).
 *
 * bootstrap_ports() used to hand the master host port, the master device
 * port, both root ledgers and the host security port to any task that asked:
 * every task held the same bootstrap port, so bootstrap could not tell them
 * apart, and host_priv alone reaches every task's port through the privileged
 * processor sets.  Now each server has a bootstrap port of its own, and its
 * line in bootstrap.conf says what it gets: -H the master host port, -D the
 * master device port, and a ledger or the security port never.
 *
 * Started three times, its argument naming the line:
 *
 *	boot_ports_test boot_ports_test none
 *	-D boot_ports_test boot_ports_test device
 *	-H -D boot_ports_test boot_ports_test both
 *
 * 🔑 Three, because one alone proves nothing: a bootstrap that refused every
 * task would pass the first, one that gave every task everything the last,
 * and one that read only the first flag of a line would pass both and fail
 * the middle one.
 *
 *	[1] what bootstrap_ports() answered is what the line gives: the host port
 *	    with -H, the device port with -D, never a ledger or the security
 *	    port.
 *	[2] the host port this task holds reaches the privileged processor sets
 *	    only with -H.  Asked of the port bootstrap gave, or, where it gave
 *	    none, of the host port every task has, mach_host_self().
 *	[3] bootstrap_console() gives the console to each of the three: a line
 *	    is written on the port it returns.
 *	[4] the device port, where there is one, opens "console" only with -D.
 *	    Without -D and without a port there is nothing to ask, and the arm
 *	    says so.
 *	[5] printf on a console that refuses (#510) says so in the kernel's
 *	    log, with the size of what it refused, sends the line on through
 *	    mach_print, and returns a negative value.  It is here because the
 *	    console [3] and [4] ask about is the one printf writes to.
 *
 * ⚠️ [2] and [4] ask the port the task holds, not the flag: a bootstrap that
 * gave the master host port to "none" fails [2] as well as [1], by the port
 * doing what it should not.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/mach_host.h>
#include <mach/mach_klog.h>	/* host_get_log: the kernel's log, for [5] */
#include <mach/host_info.h>	/* klog_data_t */
#include <mach/bootstrap.h>
#include <device/device.h>

#include <stdio.h>
#include <string.h>

#include <sa_mach.h>		/* printf_init */

static const char *
given(mach_port_t port)
{
	return port != MACH_PORT_NULL ? "given" : "null";
}

/*
 * The first line, through mach_print, which needs no port: run-x86_64.sh
 * waits on it, so a task whose console never came is a start with no end in
 * the log rather than no line at all -- which is what a printf with no
 * console would leave, and what no list can tell from a task never started.
 */
static void
say_asking(const char *line)
{
	static const char	pre[] = "boot_ports_test: asking bootstrap (";
	char			buf[sizeof (pre) + 16];
	size_t			n = 0, i;

	for (i = 0; pre[i] != 0; i++)
		buf[n++] = pre[i];
	for (i = 0; line[i] != 0 && n < sizeof (buf) - 3; i++)
		buf[n++] = line[i];
	buf[n++] = ')';
	buf[n++] = '\n';
	buf[n] = 0;
	mach_print(buf);
}

/*
 * The kernel's log, for [5]: where it ends now, and what it says from a
 * given point on.
 *
 * host_get_log() takes any host port and a cursor and hands back the bytes
 * from there, a buffer at a time; a cursor the ring has already overwritten
 * is moved by the kernel to the oldest byte it still has.  So reading from 0
 * until nothing comes back is always allowed, and stops at the end.
 */
static klog_data_t	klog_buf;
static char		klog_since[4 * KLOG_DATA_MAX];

static natural_t
klog_end(mach_port_t host)
{
	natural_t		cursor = 0, next;
	mach_msg_type_number_t	cnt;
	int			i;

	for (i = 0; i < 64; i++) {
		cnt = sizeof (klog_buf);
		if (host_get_log(host, cursor, klog_buf, &cnt, &next)
		    != KERN_SUCCESS || cnt == 0)
			break;
		cursor = next;
	}
	return cursor;
}

static size_t
klog_read_since(mach_port_t host, natural_t from)
{
	natural_t		cursor = from, next;
	mach_msg_type_number_t	cnt;
	size_t			have = 0;

	while (have + sizeof (klog_buf) <= sizeof (klog_since)) {
		cnt = sizeof (klog_buf);
		if (host_get_log(host, cursor, klog_buf, &cnt, &next)
		    != KERN_SUCCESS || cnt == 0)
			break;
		memcpy(&klog_since[have], klog_buf, cnt);
		have += cnt;
		cursor = next;
	}
	return have;
}

/* Where `needle' first occurs in the `n' bytes at `hay', or NULL. */
static const char *
find(const char *hay, size_t n, const char *needle)
{
	size_t	k = strlen(needle), i;

	for (i = 0; i + k <= n; i++)
		if (memcmp(&hay[i], needle, k) == 0)
			return &hay[i];
	return NULL;
}

int
main(int argc, char **argv)
{
	mach_port_t	host = MACH_PORT_NULL, device = MACH_PORT_NULL;
	mach_port_t	wired = MACH_PORT_NULL, paged = MACH_PORT_NULL;
	mach_port_t	security = MACH_PORT_NULL;
	const char	*line = (argc > 1 && argv[1] != 0) ? argv[1] : "";
	int		want_host, want_device;
	int		passed = 0, arms = 0;
	kern_return_t	kr;

	want_host = strcmp(line, "both") == 0;
	want_device = want_host || strcmp(line, "device") == 0;

	say_asking(line);
	kr = bootstrap_ports(bootstrap_port, &host, &device, &wired, &paged,
			     &security);

	/*
	 * With -D printf_init() opens the console from the device port; without
	 * it, it asks bootstrap_console() -- so on the "none" line every line
	 * below is itself printed on the console bootstrap gave.
	 */
	printf_init(device);
	printf("boot_ports_test: started (%s) (#620)\n", line);

	if (!want_device && strcmp(line, "none") != 0) {
		printf("boot_ports_test: WRONG — the argument \"%s\" is none of "
		       "none, device and both: bootstrap.conf and this test "
		       "disagree about the line\n", line);
		printf("boot_ports_test: 0 of 1 arms passed (%s)\n", line);
		return 1;
	}
	if (kr != KERN_SUCCESS) {
		printf("boot_ports_test: WRONG — bootstrap_ports() answered "
		       "kr=%d, not a set of ports (%s)\n", (int)kr, line);
		printf("boot_ports_test: 0 of 1 arms passed (%s)\n", line);
		return 1;
	}

	/* [1] What bootstrap_ports() answered. */
	arms++;
	if ((host != MACH_PORT_NULL) == want_host &&
	    (device != MACH_PORT_NULL) == want_device &&
	    wired == MACH_PORT_NULL && paged == MACH_PORT_NULL &&
	    security == MACH_PORT_NULL) {
		printf("boot_ports_test: [1] host %s, device %s, both ledgers "
		       "and the security port null — what this line's flags "
		       "give (%s)\n", given(host), given(device), line);
		passed++;
	} else
		printf("boot_ports_test: [1] WRONG — host %s, device %s, wired "
		       "ledger %s, paged ledger %s, security %s; this line "
		       "gives the host port %s, the device port %s, and "
		       "nothing else (%s)\n", given(host), given(device),
		       given(wired), given(paged), given(security),
		       want_host ? "(-H)" : "to nobody",
		       want_device ? "(-D)" : "to nobody", line);

	/* [2] Does the host port this task holds reach the processor sets? */
	arms++;
	{
		processor_set_name_port_array_t	psets = 0;
		mach_msg_type_number_t		n = 0, i;
		mach_port_t			asked;
		const char			*whose;

		if (host != MACH_PORT_NULL) {
			asked = host;
			whose = "the host port bootstrap gave";
		} else {
			asked = mach_host_self();
			whose = "mach_host_self()";
		}
		kr = host_processor_sets(asked, &psets, &n);
		if (kr == KERN_SUCCESS) {
			for (i = 0; i < n; i++)
				(void) mach_port_deallocate(mach_task_self(),
							    psets[i]);
			(void) vm_deallocate(mach_task_self(),
					     (vm_address_t)psets,
					     n * sizeof (psets[0]));
		}
		if (host == MACH_PORT_NULL)
			(void) mach_port_deallocate(mach_task_self(), asked);

		if ((kr == KERN_SUCCESS) == want_host) {
			printf("boot_ports_test: [2] host_processor_sets() on "
			       "%s %s (kr=%d) — the privileged processor sets "
			       "are %s (%s)\n", whose,
			       kr == KERN_SUCCESS ? "answered" : "was refused",
			       (int)kr, want_host ? "this -H line's"
						  : "out of this task's reach",
			       line);
			passed++;
		} else
			printf("boot_ports_test: [2] WRONG — host_processor_sets() "
			       "on %s %s (kr=%d), and this line %s -H (%s)\n",
			       whose,
			       kr == KERN_SUCCESS ? "answered" : "was refused",
			       (int)kr, want_host ? "has" : "does not have",
			       line);
	}

	/* [3] The console, from bootstrap_console(). */
	arms++;
	{
		static char	msg[] = "boot_ports_test: [3] this line went out "
					"on bootstrap_console()'s port\n";
		mach_port_t	console = MACH_PORT_NULL;
		io_buf_len_t	written = 0;

		kr = bootstrap_console(bootstrap_port, &console);
		if (kr == KERN_SUCCESS && console != MACH_PORT_NULL)
			kr = device_write_inband(console, 0, 0, msg,
						 sizeof (msg) - 1, &written);
		if (kr == KERN_SUCCESS && written == sizeof (msg) - 1) {
			printf("boot_ports_test: [3] bootstrap_console() gave a "
			       "port, and %d bytes were written on it (%s)\n",
			       (int)written, line);
			passed++;
		} else
			printf("boot_ports_test: [3] WRONG — bootstrap_console() "
			       "port %s, write kr=%d, %d of %d bytes (%s)\n",
			       given(console), (int)kr, (int)written,
			       (int)(sizeof (msg) - 1), line);
		if (console != MACH_PORT_NULL)
			(void) mach_port_deallocate(mach_task_self(), console);
	}

	/* [4] Does the device port this task holds open "console"? */
	if (device == MACH_PORT_NULL && !want_device)
		printf("boot_ports_test: [4] NOT ASKED — no device port on this "
		       "line, so nothing to open \"console\" with (%s)\n", line);
	else {
		static security_token_t	no_token;
		mach_port_t		opened = MACH_PORT_NULL;

		arms++;
		kr = device_open(device, MACH_PORT_NULL, D_READ | D_WRITE,
				 no_token, (char *)"console", &opened);
		if (opened != MACH_PORT_NULL) {
			(void) device_close(opened);
			(void) mach_port_deallocate(mach_task_self(), opened);
		}
		if ((kr == KERN_SUCCESS) == want_device) {
			printf("boot_ports_test: [4] device_open(\"console\") on "
			       "the device port %s (kr=%d), and this line %s -D "
			       "(%s)\n",
			       kr == KERN_SUCCESS ? "answered" : "was refused",
			       (int)kr, want_device ? "has" : "does not have",
			       line);
			passed++;
		} else
			printf("boot_ports_test: [4] WRONG — device_open("
			       "\"console\") on the device port %s %s (kr=%d), "
			       "and this line %s -D (%s)\n", given(device),
			       kr == KERN_SUCCESS ? "answered" : "was refused",
			       (int)kr, want_device ? "has" : "does not have",
			       line);
	}

	/*
	 * [5] printf on a console that refuses (#510).  For one printf the
	 * port it writes to is MACH_PORT_DEAD, where a send fails at once and
	 * nothing is allocated; then the kernel's log is read from where it
	 * ended before.  printf must have returned a negative value, said the
	 * refusal with the size of what it refused -- the line, and the '\r'
	 * printf puts before its '\n' -- and sent the line on through
	 * mach_print.  Each of the three is printed as it was read.
	 */
	arms++;
	{
		static const char	mark[] = "libmach: the console refused ";
		mach_port_t		saved = console_port;
		mach_port_t		khost = mach_host_self();
		char			text[96], refusal[64];
		const char		*p;
		natural_t		from;
		size_t			have;
		int			ret, bytes = -1, want, delivered;

		(void) snprintf(text, sizeof (text), "boot_ports_test: [5] this "
				"line was refused by the console (%s)", line);
		want = (int)strlen(text) + 2;

		from = klog_end(khost);
		console_port = MACH_PORT_DEAD;
		ret = printf("%s\n", text);
		console_port = saved;
		have = klog_read_since(khost, from);
		(void) mach_port_deallocate(mach_task_self(), khost);

		/*
		 * Every refusal from `from' on, and the one of this line's size
		 * if there is one: the three lines of this test can run at once,
		 * and another's refusal may come first -- "device" is longer than
		 * the other two.  The line itself is this one's own by its name.
		 */
		for (p = find(klog_since, have, mark); p != NULL;
		     p = find(p, (size_t)(klog_since + have - p), mark)) {
			int	n = 0;

			p += sizeof (mark) - 1;
			while (p < klog_since + have && *p >= '0' && *p <= '9')
				n = n * 10 + (*p++ - '0');
			if (bytes != want)
				bytes = n;
		}
		if (bytes >= 0)
			(void) snprintf(refusal, sizeof (refusal), "says %d "
					"bytes were refused", bytes);
		else
			(void) snprintf(refusal, sizeof (refusal),
					"holds no refusal");
		delivered = find(klog_since, have, text) != NULL;

		if (ret < 0 && bytes == want && delivered) {
			printf("boot_ports_test: [5] printf on a refused console "
			       "returned %d, the kernel log %s, the %d the "
			       "buffer held, and the line came out through "
			       "mach_print (%s)\n", ret, refusal, want, line);
			passed++;
		} else
			printf("boot_ports_test: [5] WRONG — printf on a refused "
			       "console returned %d (a negative value wanted), "
			       "the kernel log %s (%d wanted), and the line %s "
			       "(%s)\n", ret, refusal, want,
			       delivered ? "came out through mach_print"
					 : "is not in it", line);
	}

	if (host != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), host);
	if (device != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), device);
	if (wired != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), wired);
	if (paged != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), paged);
	if (security != MACH_PORT_NULL)
		(void) mach_port_deallocate(mach_task_self(), security);

	printf("boot_ports_test: %d of %d arms passed (%s)\n", passed, arms,
	       line);
	return (passed == arms) ? 0 : 1;
}
