/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * bootlog DIR -- this boot's console, the kernel's log, in a file of its own
 * under DIR (#915).
 *
 * lapsus clears its memory when it resets, so ramoops keeps nothing there and
 * a boot that stops leaves nothing to read.  A file on a disk does: DIR is the
 * mount of the partition labelled UROSLOG, /log, which ext_server mounts by
 * its label and which the block server lets nobody else write (-W).
 *
 * The kernel keeps its log in a ring, klog, that any task reads with
 * host_get_log(), and every task's console output is in it too.  So this
 * starts early, copies the ring into memory from its first byte while it
 * waits for DIR, then makes DIR/boot-N.txt -- the first N no file has, asked
 * with O_EXCL so that two boots can never share one -- writes what it holds,
 * and goes on appending as the ring grows.  It syncs at most once a second,
 * and only after a second that brought something: a boot that stops loses
 * about a second, and an idle machine is left idle.
 *
 * ⚠️ What klog does not have, this cannot write: the kernel's lines before
 * klog_init() and every kputs() -- the early self-tests -- and the report of
 * a fatal trap.  And the ring is 64 KiB: a reader that falls that far behind
 * loses the oldest bytes, which the file says, with how many.
 */

#include <stdio.h>
#include <string.h>
#include <mach.h>
#include <mach/bootstrap.h>
#include <mach/mach_klog.h>
#include <mach/mach_traps.h>		/* mach_host_self */
#include <mach/thread_switch.h>
#include <sa_mach.h>			/* printf_init */
#include <mach_init.h>			/* name_server_port */
#include <servers/netname.h>
#include <servers/netname_defs.h>	/* netname_name_t */
#include <libvfs.h>

#define HOLD_BYTES	(1024u * 1024u)	/* the ring kept until DIR is there */
#define NAP_MIN_MS	50u		/* after a read that brought bytes */
#define NAP_MAX_MS	2000u		/* the longest wait on an idle ring */
#define SYNC_EVERY_MS	1000u
#define NAMES_MAX	100000u		/* boot-1.txt .. boot-100000.txt */

static mach_port_t	host;
static natural_t	cursor;		/* host_get_log's, modular */
static unsigned long	lost;		/* bytes the ring dropped before read */

static char		hold[HOLD_BYTES];
static unsigned int	held;
static unsigned long	held_dropped;	/* read but past the hold */

static vfs_fd_t		fd = VFS_FD_INVALID;
static unsigned long	written;

static void
nap(unsigned int ms)
{
	(void)thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, ms);
}

/* Bytes into the file, or into the hold until there is one. */
static void
put(const char *p, unsigned int n)
{
	if (fd != VFS_FD_INVALID) {
		if (vfs_write(fd, p, n) == (ssize_t)n)
			written += n;
		return;
	}
	if (n > HOLD_BYTES - held) {
		held_dropped += n - (HOLD_BYTES - held);
		n = HOLD_BYTES - held;
	}
	memcpy(hold + held, p, n);
	held += n;
}

/* Everything the ring has past the cursor: how many bytes came. */
static unsigned int
drain(void)
{
	unsigned int	total = 0;
	klog_data_t	buf;

	for (;;) {
		mach_msg_type_number_t	cnt = sizeof(buf);
		natural_t		next;
		char			note[96];

		if (host_get_log(host, cursor, buf, &cnt, &next)
		    != KERN_SUCCESS || cnt == 0)
			return total;
		/* The kernel moved a reader that fell behind to its oldest byte. */
		if (next - cnt != cursor) {
			unsigned long gap = (unsigned long)(natural_t)(next - cnt - cursor);

			lost += gap;
			snprintf(note, sizeof(note), "\n[bootlog: %lu bytes of the "
				 "kernel's log lost here, the ring went past "
				 "them]\n", gap);
			put(note, (unsigned int)strlen(note));
		}
		put(buf, cnt);
		cursor = next;
		total += cnt;
	}
}

/* Whether DIR is mounted, and ext_server answers there. */
static int
dir_ready(const char *dir)
{
	netname_name_t	matched;
	mach_port_t	port = MACH_PORT_NULL;
	vfs_stat_t	st;

	matched[0] = '\0';
	if (netname_look_up_mount(name_server_port, (char *)dir, &port, matched)
	    != KERN_SUCCESS)
		return 0;
	if (port != MACH_PORT_NULL)
		(void)mach_port_deallocate(mach_task_self(), port);
	return strcmp(matched, dir) == 0 && vfs_stat(dir, &st) == 0;
}

/*
 * DIR/boot-N.txt for the first N no file has.  O_EXCL makes the test and the
 * creation one step, so the answer is VFS_ERR_EXIST for a taken N and
 * anything else is a reason to stop.
 */
static kern_return_t
open_next(const char *dir, char *path, size_t pathlen, unsigned int *n_out)
{
	kern_return_t	kr = KERN_FAILURE;
	unsigned int	n;

	for (n = 1; n <= NAMES_MAX; n++) {
		snprintf(path, pathlen, "%s/boot-%u.txt", dir, n);
		kr = vfs_open_rc(path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_EXCL,
				 0644, &fd);
		if (kr == KERN_SUCCESS) {
			*n_out = n;
			return KERN_SUCCESS;
		}
		if (kr != VFS_ERR_EXIST)
			return kr;
	}
	return kr;
}

int
main(int argc, char **argv)
{
	mach_port_t	host_port, device_port, wired, paged, security;
	const char	*dir;
	char		path[128], head[192];
	unsigned int	n = 0, waited_ms = 0, since_sync = 0;
	unsigned int	nap_ms = NAP_MIN_MS;
	int		unsynced = 0, said_waiting = 0;
	kern_return_t	kr;

	kr = bootstrap_ports(bootstrap_port, &host_port, &device_port,
			     &wired, &paged, &security);
	if (kr != KERN_SUCCESS)
		_exit(1);	/* no console yet -- nothing to say it with */
	printf_init(device_port);
	host = mach_host_self();	/* any host port reads the ring */

	dir = argc > 1 ? argv[1] : NULL;
	if (dir == NULL || dir[0] != '/' || strlen(dir) > 64) {
		printf("bootlog: no directory to write in, so no log is kept "
		       "(#915)\n");
		return 0;
	}
	printf("bootlog: started, keeping the kernel's log until %s is "
	       "mounted (#915)\n", dir);

	/* Until DIR is there: the ring into the hold. */
	while (!dir_ready(dir)) {
		(void)drain();
		nap(100);
		waited_ms += 100;
		if (!said_waiting && waited_ms >= 60000) {
			said_waiting = 1;
			printf("bootlog: %s is not mounted after 60 s; still "
			       "keeping the log in memory (#915)\n", dir);
		}
	}
	(void)drain();

	kr = open_next(dir, path, sizeof(path), &n);
	if (kr != KERN_SUCCESS) {
		printf("bootlog: no file made in %s (kr=0x%x), so no log is "
		       "kept on this boot (#915)\n", dir, (unsigned)kr);
		return 0;
	}
	snprintf(head, sizeof(head), "bootlog: boot %u, the kernel's log from "
		 "its first byte in the ring; %s was mounted after %u ms "
		 "(#915)\n", n, dir, waited_ms);
	(void)vfs_write(fd, head, strlen(head));
	if (vfs_write(fd, hold, held) == (ssize_t)held)
		written = held;
	if (held_dropped > 0) {
		snprintf(head, sizeof(head), "\n[bootlog: %lu bytes read before "
			 "%s was mounted did not fit in the %u held]\n",
			 held_dropped, dir, HOLD_BYTES);
		(void)vfs_write(fd, head, strlen(head));
	}
	(void)vfs_sync(fd);
	printf("bootlog: writing %s, %lu bytes so far (#915)\n", path, written);

	/*
	 * Then as it grows.  The wait doubles while the ring is idle, up to two
	 * seconds, and is short again as soon as a read brings bytes.
	 */
	for (;;) {
		if (drain() > 0) {
			unsynced = 1;
			nap_ms = NAP_MIN_MS;
		} else if (nap_ms < NAP_MAX_MS) {
			nap_ms *= 2;
			if (nap_ms > NAP_MAX_MS)
				nap_ms = NAP_MAX_MS;
		}
		if (unsynced && since_sync >= SYNC_EVERY_MS) {
			(void)vfs_sync(fd);
			unsynced = 0;
			since_sync = 0;
		}
		nap(nap_ms);
		since_sync += nap_ms;
	}
	/* NOTREACHED */
	return 0;
}
