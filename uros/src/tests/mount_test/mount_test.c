/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Who may mount where, and what a reader takes from a mount's server (#716).
 *
 * ── The registry ──
 *
 * A mount was checked in with MACH_PORT_NULL and was anyone's to check out,
 * "/" included; a longer prefix needed nobody's leave, and the look-up takes
 * the longest match, so a task that registered "/etc" answered every open
 * under it.  A prefix is now mounted on a capability for it and unmounted on
 * the signature it was checked in with.
 *
 * [2] to [6] are the two attacks #716 names, from a task whose manifest grants
 * a mount capability -- for /mount_test, which is the wrong one here, a closer
 * miss than holding none: "/" checked out with no signature and with one that
 * is not its owner's, and "/etc" checked in with no token, with a token this
 * task is refused, and with its own.  [7] to [11] are the same rules on its
 * own prefix, where it holds the right capability, and [9] is their control:
 * the right signature and the right token mount.  [12] asks for a path as long
 * as netname takes, which the name server's old 256-byte buffer destroyed
 * unread.  Each refusal is asserted by its whole value, and each is followed
 * by the look-up that shows the registry did not move.
 *
 * ── The reader ──
 *
 * [13] to [19] read and write through libvfs from the mount this task serves
 * (mount_liar.c), which answers wrong on the files that ask it to.  libvfs
 * copied what a server said it had: a Mach read's length, a FLIPC read's
 * length and offset, a write's count.  [13] and [17] are the controls -- the
 * Mach path and the FLIPC path, each shown to be the path that answered --
 * and without them the refusals after each could pass on a path never taken.
 * [20] checks the mount out, which is the control for [11].
 */

#include <mach.h>
#include <mach/message.h>
#include <mach/mach_traps.h>
#include <mach/thread_switch.h>
#include <mach/cap_types.h>
#include <mach_init.h>			/* name_server_port */

#include <stdio.h>
#include <string.h>

#include <servers/netname.h>
#include <servers/netname_defs.h>

#include <libcap.h>
#include <libvfs.h>
#include <vfs_flipc.h>

#include "mount_test.h"

#define	MT_PREFIX	"/mount_test"
#define	MT_ARMS		20
#define	MT_WAIT_STEPS	1200		/* x 100 ms: "/" has 120 s to appear */

static int	passed, arms;

static void
mt_result(int ok)
{
	arms++;
	if (ok)
		passed++;
}

/*
 * One look-up of `path', the reference it hands back given back: the caller
 * compares the name, which outlives that reference for a port this task holds
 * another right to -- the receive right of its own mount, or the reference [1]
 * keeps to "/"'s.
 */
static kern_return_t
mt_look_up(const char *path, mach_port_t *port, netname_name_t matched)
{
	kern_return_t kr;

	*port = MACH_PORT_NULL;
	matched[0] = '\0';
	kr = netname_look_up_mount(name_server_port, (char *)path, port,
				   matched);
	if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(*port))
		(void)mach_port_deallocate(mach_task_self(), *port);
	return kr;
}

/* Whether `path' still resolves to `want' under `prefix'. */
static int
mt_resolves(const char *path, mach_port_t want, const char *prefix)
{
	netname_name_t	matched;
	mach_port_t	got;

	return mt_look_up(path, &got, matched) == NETNAME_SUCCESS &&
	       got == want && strcmp(matched, prefix) == 0;
}

static mach_port_t
another_port(void)
{
	mach_port_t p = MACH_PORT_NULL;

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &p)
	    != KERN_SUCCESS)
		return MACH_PORT_NULL;
	if (mach_port_insert_right(mach_task_self(), p, p,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
		return MACH_PORT_NULL;
	return p;
}

static int
mt_all(const char *buf, char c, size_t n)
{
	size_t i;

	for (i = 0; i < n; i++)
		if (buf[i] != c)
			return 0;
	return 1;
}

/* What a read into this buffer may write past its first LIAR_SIZE bytes. */
static struct {
	char	buf[LIAR_SIZE];
	char	guard[2 * LIAR_MORE_BY];
} mt_read;

static void
mt_read_reset(void)
{
	memset(mt_read.buf, 0, sizeof mt_read.buf);
	memset(mt_read.guard, 0xA5, sizeof mt_read.guard);
}

static int
mt_guard_intact(void)
{
	return mt_all(mt_read.guard, (char)0xA5, sizeof mt_read.guard);
}

static void
mt_print_buf(char *out, size_t size)
{
	size_t i;

	for (i = 0; i < LIAR_SIZE && i + 1 < size; i++) {
		unsigned char c = (unsigned char)mt_read.buf[i];
		out[i] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
	}
	out[i] = '\0';
}

int
main(int argc, char **argv)
{
	netname_name_t	matched;
	mach_port_t	root = MACH_PORT_NULL, mine = MACH_PORT_NULL;
	mach_port_t	sig = MACH_PORT_NULL, other = MACH_PORT_NULL, got;
	struct uros_cap	tok;
	char		tok_blob[CAP_TOKEN_MAX];
	int		have_root = 0, have_tok = 0, mounted = 0;
	const char	*why;
	kern_return_t	kr;
	unsigned int	step;
	char		shown[LIAR_SIZE + 1];

	(void)argc; (void)argv;
	printf("mount_test: started (#716)\n");
	memset(tok_blob, 0, sizeof tok_blob);

	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
			       &mine) != KERN_SUCCESS ||
	    mach_port_insert_right(mach_task_self(), mine, mine,
				   MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS ||
	    urmach_netname_signature(&sig) != KERN_SUCCESS ||
	    (other = another_port()) == MACH_PORT_NULL) {
		printf("mount_test: WRONG — no port or no signature\n");
		printf("mount_test: 0 of %d arms passed\n", MT_ARMS);
		return 1;
	}
	why = liar_start(mine);
	if (why != NULL) {
		printf("mount_test: WRONG — the mount's server did not start: "
		       "%s\n", why);
		printf("mount_test: 0 of %d arms passed\n", MT_ARMS);
		return 1;
	}

	/*
	 * [1] "/" is ext_server's.  The presence every arm about "/" stands on:
	 * a refusal to take over a mount that is not there shows nothing.
	 */
	for (step = 0; step < MT_WAIT_STEPS; step++) {
		root = MACH_PORT_NULL;
		matched[0] = '\0';
		kr = netname_look_up_mount(name_server_port, "/", &root,
					   matched);
		if (kr == NETNAME_SUCCESS && MACH_PORT_VALID(root))
			break;
		thread_switch(MACH_PORT_NULL, SWITCH_OPTION_WAIT, 100);
	}
	have_root = (step < MT_WAIT_STEPS && strcmp(matched, "/") == 0);
	mt_result(have_root);
	if (have_root)
		printf("mount_test: [1] \"/\" is mounted, port 0x%x\n",
		       (unsigned)root);
	else
		printf("mount_test: [1] WRONG — no \"/\" mount after %u s "
		       "(kr=%d, matched \"%s\")\n", MT_WAIT_STEPS / 10, kr,
		       matched);

	/* [2] "/" checked out with no signature. */
	if (!have_root) {
		printf("mount_test: [2] WRONG — not asked: no \"/\"\n");
		mt_result(0);
	} else {
		kr = netname_check_out_mount(name_server_port, "/",
					     MACH_PORT_NULL);
		if (kr == NETNAME_INVALID_PORT && mt_resolves("/", root, "/")) {
			printf("mount_test: [2] \"/\" checked out with no "
			       "signature: NETNAME_INVALID_PORT, and \"/\" is "
			       "still 0x%x's\n", (unsigned)root);
			mt_result(1);
		} else {
			printf("mount_test: [2] WRONG — \"/\" checked out with "
			       "no signature: %d, not NETNAME_INVALID_PORT "
			       "(%d)%s\n", kr, NETNAME_INVALID_PORT,
			       mt_resolves("/", root, "/") ? ""
			       : ", and \"/\" is no longer ext_server's");
			mt_result(0);
		}
	}

	/* [3] "/" checked out with a signature that is not its owner's. */
	if (!have_root) {
		printf("mount_test: [3] WRONG — not asked: no \"/\"\n");
		mt_result(0);
	} else {
		kr = netname_check_out_mount(name_server_port, "/", sig);
		if (kr == NETNAME_NOT_YOURS && mt_resolves("/", root, "/")) {
			printf("mount_test: [3] \"/\" checked out with this "
			       "task's signature: NETNAME_NOT_YOURS, and \"/\" "
			       "is still 0x%x's\n", (unsigned)root);
			mt_result(1);
		} else {
			printf("mount_test: [3] WRONG — \"/\" checked out with "
			       "this task's signature: %d, not "
			       "NETNAME_NOT_YOURS (%d)%s\n", kr,
			       NETNAME_NOT_YOURS,
			       mt_resolves("/", root, "/") ? ""
			       : ", and \"/\" is no longer ext_server's");
			mt_result(0);
		}
	}

	/*
	 * [4] "/etc" checked in with no capability.  An empty token: what a
	 * task that never asked cap_server for one has to send.
	 */
	kr = netname_check_in_mount(name_server_port, "/etc", sig, mine,
				    tok_blob, 0);
	if (kr == CAP_ERR_INVALID_TOKEN &&
	    (!have_root || mt_resolves("/etc/passwd", root, "/"))) {
		printf("mount_test: [4] \"/etc\" checked in with no "
		       "capability: CAP_ERR_INVALID_TOKEN, and /etc/passwd "
		       "is still \"/\"'s\n");
		mt_result(1);
	} else {
		printf("mount_test: [4] WRONG — \"/etc\" checked in with no "
		       "capability: %d, not CAP_ERR_INVALID_TOKEN (%d)\n", kr,
		       CAP_ERR_INVALID_TOKEN);
		if (kr == NETNAME_SUCCESS)
			(void)netname_check_out_mount(name_server_port, "/etc",
						      sig);
		mt_result(0);
	}

	/* [5] A capability for "/etc", asked of cap_server: not this task's. */
	kr = cap_request(RESOURCE_MOUNT_POINT, cap_name_hash("/etc"),
			 CAP_OP_MOUNT_REGISTER, 0, &tok);
	if (kr == CAP_ERR_NOT_IN_MANIFEST) {
		printf("mount_test: [5] a capability for \"/etc\" is refused by "
		       "cap_server: CAP_ERR_NOT_IN_MANIFEST\n");
		mt_result(1);
	} else {
		printf("mount_test: [5] WRONG — a capability for \"/etc\": %d, "
		       "not CAP_ERR_NOT_IN_MANIFEST (%d)\n", kr,
		       CAP_ERR_NOT_IN_MANIFEST);
		mt_result(0);
	}

	/*
	 * [6] "/etc" checked in with this task's own capability, which is for
	 * /mount_test.  The token is valid -- only its prefix is not this one.
	 */
	kr = cap_request(RESOURCE_MOUNT_POINT, cap_name_hash(MT_PREFIX),
			 CAP_OP_MOUNT_REGISTER, 0, &tok);
	have_tok = (kr == KERN_SUCCESS);
	if (have_tok)
		memcpy(tok_blob, &tok, sizeof tok);
	else
		printf("mount_test: WRONG — this task's manifest grants "
		       MT_PREFIX ", and cap_server refused it: %d\n", kr);
	if (!have_tok) {
		printf("mount_test: [6] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, "/etc", sig,
					    mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == CAP_ERR_RESOURCE_MISMATCH &&
		    (!have_root || mt_resolves("/etc/passwd", root, "/"))) {
			printf("mount_test: [6] \"/etc\" checked in with the "
			       "capability for " MT_PREFIX ": "
			       "CAP_ERR_RESOURCE_MISMATCH, and /etc/passwd is "
			       "still \"/\"'s\n");
			mt_result(1);
		} else {
			printf("mount_test: [6] WRONG — \"/etc\" checked in "
			       "with the capability for " MT_PREFIX ": %d, not "
			       "CAP_ERR_RESOURCE_MISMATCH (%d)\n", kr,
			       CAP_ERR_RESOURCE_MISMATCH);
			if (kr == NETNAME_SUCCESS)
				(void)netname_check_out_mount(
					name_server_port, "/etc", sig);
			mt_result(0);
		}
	}

	/* [7] Its own prefix, with the right capability and no signature. */
	if (!have_tok) {
		printf("mount_test: [7] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX,
					    MACH_PORT_NULL, mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == NETNAME_INVALID_PORT &&
		    mt_look_up(MT_PREFIX "/honest", &got, matched)
		    == NETNAME_SUCCESS && strcmp(matched, MT_PREFIX) != 0) {
			printf("mount_test: [7] " MT_PREFIX " checked in with no "
			       "signature: NETNAME_INVALID_PORT\n");
			mt_result(1);
		} else {
			printf("mount_test: [7] WRONG — " MT_PREFIX " checked in "
			       "with no signature: %d, not "
			       "NETNAME_INVALID_PORT (%d)\n", kr,
			       NETNAME_INVALID_PORT);
			if (kr == NETNAME_SUCCESS)
				(void)netname_check_out_mount(
					name_server_port, MT_PREFIX,
					MACH_PORT_NULL);
			mt_result(0);
		}
	}

	/*
	 * [8] Signed by the port being mounted, which every task that looks
	 * the mount up holds a send right to.
	 */
	if (!have_tok) {
		printf("mount_test: [8] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX, mine,
					    mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == NETNAME_INVALID_PORT) {
			printf("mount_test: [8] " MT_PREFIX " checked in signed "
			       "by the port being mounted: "
			       "NETNAME_INVALID_PORT\n");
			mt_result(1);
		} else {
			printf("mount_test: [8] WRONG — " MT_PREFIX " checked in "
			       "signed by the port being mounted: %d, not "
			       "NETNAME_INVALID_PORT (%d)\n", kr,
			       NETNAME_INVALID_PORT);
			if (kr == NETNAME_SUCCESS)
				(void)netname_check_out_mount(
					name_server_port, MT_PREFIX, mine);
			mt_result(0);
		}
	}

	/* [9] The control for [7] and [8]: signature and capability both. */
	if (!have_tok) {
		printf("mount_test: [9] WRONG — not asked: no capability\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX, sig,
					    mine, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		mounted = (kr == NETNAME_SUCCESS);
		if (mounted &&
		    mt_resolves(MT_PREFIX "/honest", mine, MT_PREFIX)) {
			printf("mount_test: [9] " MT_PREFIX " checked in with "
			       "this task's signature and capability, and "
			       MT_PREFIX "/honest resolves to it\n");
			mt_result(1);
		} else {
			printf("mount_test: [9] WRONG — " MT_PREFIX " checked in "
			       "with this task's signature and capability: "
			       "%d%s\n", kr, mounted
			       ? ", and a look-up under it does not reach it"
			       : "");
			mt_result(0);
		}
	}

	/* [10] A second mount of a prefix that has one, capability or not. */
	if (!mounted) {
		printf("mount_test: [10] WRONG — not asked: [9] did not "
		       "mount\n");
		mt_result(0);
	} else {
		kr = netname_check_in_mount(name_server_port, MT_PREFIX, sig,
					    other, tok_blob,
					    (mach_msg_type_number_t)sizeof tok);
		if (kr == NETNAME_MOUNT_EXISTS &&
		    mt_resolves(MT_PREFIX "/honest", mine, MT_PREFIX)) {
			printf("mount_test: [10] " MT_PREFIX " checked in again "
			       "for another port: NETNAME_MOUNT_EXISTS\n");
			mt_result(1);
		} else {
			printf("mount_test: [10] WRONG — " MT_PREFIX " checked "
			       "in again for another port: %d, not "
			       "NETNAME_MOUNT_EXISTS (%d)\n", kr,
			       NETNAME_MOUNT_EXISTS);
			mt_result(0);
		}
	}

	/* [11] Its own mount checked out with no signature, and another. */
	if (!mounted) {
		printf("mount_test: [11] WRONG — not asked: [9] did not "
		       "mount\n");
		mt_result(0);
	} else {
		kern_return_t kr2;

		kr = netname_check_out_mount(name_server_port, MT_PREFIX,
					     MACH_PORT_NULL);
		kr2 = netname_check_out_mount(name_server_port, MT_PREFIX,
					      other);
		if (kr == NETNAME_INVALID_PORT && kr2 == NETNAME_NOT_YOURS &&
		    mt_resolves(MT_PREFIX "/honest", mine, MT_PREFIX)) {
			printf("mount_test: [11] " MT_PREFIX " checked out with "
			       "no signature: NETNAME_INVALID_PORT; with "
			       "another: NETNAME_NOT_YOURS; and it is still "
			       "mounted\n");
			mt_result(1);
		} else {
			printf("mount_test: [11] WRONG — " MT_PREFIX " checked "
			       "out with no signature: %d (not %d), with "
			       "another: %d (not %d)\n", kr,
			       NETNAME_INVALID_PORT, kr2, NETNAME_NOT_YOURS);
			mounted = mt_resolves(MT_PREFIX "/honest", mine,
					      MT_PREFIX);
			mt_result(0);
		}
	}

	/*
	 * [12] A path as long as netname_look_up_mount takes.  The name server
	 * received into 256 bytes, and the kernel destroyed a longer request
	 * unread: MIG_SERVER_DIED, from a server that was alive.
	 */
	{
		static char	path[NETNAME_PATH_MAX];
		size_t		len = sizeof MT_PREFIX "/" - 1;

		memcpy(path, MT_PREFIX "/", len);
		memset(path + len, 'a', sizeof path - 1 - len);
		path[sizeof path - 1] = '\0';
		kr = mt_look_up(path, &got, matched);
		if (kr == NETNAME_SUCCESS &&
		    (!mounted || (got == mine &&
				  strcmp(matched, MT_PREFIX) == 0))) {
			printf("mount_test: [12] a look-up of a %u-byte path is "
			       "answered, by \"%s\"\n",
			       (unsigned)(sizeof path - 1), matched);
			mt_result(1);
		} else {
			printf("mount_test: [12] WRONG — a look-up of a %u-byte "
			       "path: %d, matched \"%s\"\n",
			       (unsigned)(sizeof path - 1), kr, matched);
			mt_result(0);
		}
	}

	/* ── The reader: the Mach path first. ── */
	vfs_flipc_set_enabled(0);

	/* [13] The control: a read of an honest file, answered on Mach. */
	if (!mounted) {
		printf("mount_test: [13] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/honest", VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE) &&
		    liar_mach_reads[LIAR_HONEST] > 0) {
			printf("mount_test: [13] %d bytes of an honest file read "
			       "on the Mach path: \"%s\"\n", (int)n, shown);
			mt_result(1);
		} else {
			printf("mount_test: [13] WRONG — an honest file on the "
			       "Mach path: fd %d, %ld bytes \"%s\", %u Mach "
			       "reads\n", fd, (long)n, shown,
			       liar_mach_reads[LIAR_HONEST]);
			mt_result(0);
		}
	}

	/* [14] A Mach read answered LIAR_MORE_BY bytes longer than asked. */
	if (!mounted) {
		printf("mount_test: [14] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/more", VFS_O_RDONLY, 0);
		ssize_t	 n = 0;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		if (fd >= 0 && n == -1 && mt_guard_intact() &&
		    liar_mach_reads[LIAR_MORE] > 0) {
			printf("mount_test: [14] a read of %d bytes answered "
			       "with %d: refused, and nothing written past the "
			       "buffer\n", LIAR_SIZE, LIAR_SIZE + LIAR_MORE_BY);
			mt_result(1);
		} else {
			printf("mount_test: [14] WRONG — a read of %d bytes "
			       "answered with %d: returned %ld, %s, fd %d\n",
			       LIAR_SIZE, LIAR_SIZE + LIAR_MORE_BY, (long)n,
			       mt_guard_intact() ? "the guard after the buffer "
			       "intact" : "the guard after the buffer written",
			       fd);
			mt_result(0);
		}
	}

	/* [15] A Mach write answered as one byte more than was sent. */
	if (!mounted) {
		printf("mount_test: [15] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/write_more", VFS_O_RDWR, 0);
		ssize_t	 n = 0;
		off_t	 at = -1;

		if (fd >= 0) {
			n = vfs_write(fd, "12345678", 8);
			at = vfs_lseek(fd, 0, VFS_SEEK_CUR);
			(void)vfs_close(fd);
		}
		if (fd >= 0 && n == -1 && at == 0) {
			printf("mount_test: [15] a write of 8 bytes answered as "
			       "9: refused, and the offset is still 0\n");
			mt_result(1);
		} else {
			printf("mount_test: [15] WRONG — a write of 8 bytes "
			       "answered as 9: returned %ld, offset %ld, fd "
			       "%d\n", (long)n, (long)at, fd);
			mt_result(0);
		}
	}

	/*
	 * [16] A read of 4 GiB, of a LIAR_SIZE-byte file.  The count went to
	 * the server in 32 bits, as zero, and zero bytes back read as the end
	 * of the file.
	 */
	if (!mounted) {
		printf("mount_test: [16] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/honest", VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, (size_t)1 << 32);
			(void)vfs_close(fd);
		}
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE)) {
			printf("mount_test: [16] a read of 4 GiB of a %d-byte "
			       "file returns its %d bytes\n", LIAR_SIZE,
			       LIAR_SIZE);
			mt_result(1);
		} else {
			printf("mount_test: [16] WRONG — a read of 4 GiB of a "
			       "%d-byte file returned %ld, fd %d\n", LIAR_SIZE,
			       (long)n, fd);
			mt_result(0);
		}
	}

	/* ── The FLIPC path. ── */
	why = mounted ? liar_flipc_start() : "not mounted";
	if (why != NULL)
		printf("mount_test: WRONG — no FLIPC endpoint: %s\n", why);
	vfs_flipc_set_enabled(1);

	/* [17] The control: a read of an honest file, answered on FLIPC. */
	if (why != NULL) {
		printf("mount_test: [17] WRONG — not asked: no endpoint\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/flipc_honest",
				       VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'F', LIAR_SIZE) &&
		    liar_flipc_reads[LIAR_FLIPC_HONEST] > 0 &&
		    liar_mach_reads[LIAR_FLIPC_HONEST] == 0) {
			printf("mount_test: [17] %d bytes of an honest file read "
			       "through FLIPC: \"%s\"\n", (int)n, shown);
			mt_result(1);
		} else {
			printf("mount_test: [17] WRONG — an honest file through "
			       "FLIPC: fd %d, %ld bytes \"%s\", %u FLIPC and %u "
			       "Mach reads\n", fd, (long)n, shown,
			       liar_flipc_reads[LIAR_FLIPC_HONEST],
			       liar_mach_reads[LIAR_FLIPC_HONEST]);
			mt_result(0);
		}
	}

	/*
	 * [18] A FLIPC read answered LIAR_FLIPC_MORE_BY bytes longer than
	 * asked, from inside the region.  libvfs reads through a window of
	 * VFS_FLIPC_MAX_READ bytes it allocates on the fd's first read, and
	 * the extra bytes land past it.
	 *
	 * ⚠️ So a guard page is put where they land, or the arm would turn on
	 * what this task happens to have mapped there: the window is allocated
	 * first-fit, and a probe of its size, allocated and given back, finds
	 * the hole it will take.  Nothing in this task allocates between the
	 * probe and the read.  If the page cannot be placed the arm says so
	 * and judges by what the read returned.
	 */
	if (why != NULL) {
		printf("mount_test: [18] WRONG — not asked: no endpoint\n");
		mt_result(0);
	} else {
		vm_address_t	probe = 0, guard = 0;
		vfs_fd_t	fd;
		ssize_t		n = -1;
		int		guard_ok = 1;

		if (vm_allocate(mach_task_self(), &probe, VFS_FLIPC_MAX_READ,
				TRUE) == KERN_SUCCESS) {
			(void)vm_deallocate(mach_task_self(), probe,
					    VFS_FLIPC_MAX_READ);
			guard = probe + VFS_FLIPC_MAX_READ;
			if (vm_allocate(mach_task_self(), &guard, vm_page_size,
					FALSE) == KERN_SUCCESS)
				memset((void *)guard, 0xA5, vm_page_size);
			else
				guard = 0;
		}
		fd = vfs_open(MT_PREFIX "/flipc_more", VFS_O_RDONLY, 0);
		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		if (guard != 0) {
			guard_ok = mt_all((const char *)guard, (char)0xA5,
					  vm_page_size);
			(void)vm_deallocate(mach_task_self(), guard,
					    vm_page_size);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE) &&
		    guard_ok && liar_flipc_reads[LIAR_FLIPC_MORE] > 0 &&
		    liar_mach_reads[LIAR_FLIPC_MORE] > 0) {
			printf("mount_test: [18] a FLIPC read answered %d bytes "
			       "longer than asked: refused, read again on the "
			       "Mach path (\"%s\")%s\n", LIAR_FLIPC_MORE_BY,
			       shown, guard ? ", nothing written past the "
			       "window" : ", no guard page could be placed");
			mt_result(1);
		} else {
			printf("mount_test: [18] WRONG — a FLIPC read answered "
			       "%d bytes longer than asked: %ld bytes \"%s\", "
			       "%u FLIPC and %u Mach reads, %s\n",
			       LIAR_FLIPC_MORE_BY, (long)n, shown,
			       liar_flipc_reads[LIAR_FLIPC_MORE],
			       liar_mach_reads[LIAR_FLIPC_MORE],
			       guard == 0 ? "no guard page placed" : guard_ok
			       ? "the page past the window intact"
			       : "the page past the window written");
			mt_result(0);
		}
	}

	/*
	 * [19] A FLIPC read answered from an offset that wraps to just before
	 * the data region, for a length that would fit.
	 */
	if (why != NULL) {
		printf("mount_test: [19] WRONG — not asked: no endpoint\n");
		mt_result(0);
	} else {
		vfs_fd_t fd = vfs_open(MT_PREFIX "/flipc_outside",
				       VFS_O_RDONLY, 0);
		ssize_t	 n = -1;

		mt_read_reset();
		if (fd >= 0) {
			n = vfs_read(fd, mt_read.buf, LIAR_SIZE);
			(void)vfs_close(fd);
		}
		mt_print_buf(shown, sizeof shown);
		if (n == LIAR_SIZE && mt_all(mt_read.buf, 'M', LIAR_SIZE) &&
		    liar_flipc_reads[LIAR_FLIPC_OUTSIDE] > 0 &&
		    liar_mach_reads[LIAR_FLIPC_OUTSIDE] > 0) {
			printf("mount_test: [19] a FLIPC read answered from "
			       "outside the data region: refused, read again on "
			       "the Mach path (\"%s\")\n", shown);
			mt_result(1);
		} else {
			printf("mount_test: [19] WRONG — a FLIPC read answered "
			       "from outside the data region: %ld bytes \"%s\", "
			       "%u FLIPC and %u Mach reads\n", (long)n, shown,
			       liar_flipc_reads[LIAR_FLIPC_OUTSIDE],
			       liar_mach_reads[LIAR_FLIPC_OUTSIDE]);
			mt_result(0);
		}
	}

	/* [20] The control for [11]: the mount's own signature checks it out. */
	if (!mounted) {
		printf("mount_test: [20] WRONG — not asked: not mounted\n");
		mt_result(0);
	} else {
		kr = netname_check_out_mount(name_server_port, MT_PREFIX, sig);
		if (kr == NETNAME_SUCCESS &&
		    mt_look_up(MT_PREFIX "/honest", &got, matched)
		    == NETNAME_SUCCESS && strcmp(matched, MT_PREFIX) != 0) {
			printf("mount_test: [20] " MT_PREFIX " checked out with "
			       "its own signature, and " MT_PREFIX "/honest "
			       "resolves to \"%s\" again\n", matched);
			mt_result(1);
		} else {
			printf("mount_test: [20] WRONG — " MT_PREFIX " checked "
			       "out with its own signature: %d, a look-up under "
			       "it now matches \"%s\"\n", kr, matched);
			mt_result(0);
		}
	}

	if (have_root)
		(void)mach_port_deallocate(mach_task_self(), root);
	printf("mount_test: %d of %d arms passed\n", passed, arms);
	return (passed == arms && arms == MT_ARMS) ? 0 : 1;
}
