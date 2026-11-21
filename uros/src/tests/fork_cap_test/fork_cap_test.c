/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * fork_cap_test -- what a child made by fork() holds of its parent's
 * capabilities (#945).
 *
 * bootstrap starts this program with a manifest of its own, so it has a
 * per-task port: its TASK_CAP_PORT, which libmach reads into mach_cap_port at
 * start and libcap asks on.  A child made by fork() holds none.  The kernel
 * does not hand a new task its parent's TASK_CAP_PORT -- the port is the
 * task's identity to cap_server, and a child holding it would be its parent --
 * and mach_task_self_init() forgets the name the child's memory kept for it.
 * libcap then asks cap_server's well-known port, which fork() copies into the
 * child and which issues no capability (#511).
 *
 * Before #945 the child kept its parent's name for its per-task port, and in
 * the child's space the name named nothing, or something else: hello_server's
 * child handed it to exec_server on exec and was answered MIG_BAD_ID.  And the
 * well-known port, where libcap goes without a per-task port, was not copied.
 *
 *   [1] this task asks for the capability its manifest grants, through
 *       libcap, and is granted it: its own port is cap_server's and enforces
 *       the manifest, so what the child is refused below is not refused to
 *       everyone;
 *   [2] a forked child asks for the same, through libcap, and cap_server
 *       answers it, refusing it with CAP_ERR_NOT_IN_MANIFEST.  A grant is the
 *       child asking as its parent; any other answer is a port that is not
 *       cap_server's, or none;
 *   [3] the child holds no per-task port: mach_cap_port is null in it, and so
 *       is the TASK_CAP_PORT the kernel gave it;
 *   [4] the child holds its parent's capability for disk0a in its copy of its
 *       parent's memory, and opening disk0a with those bytes the block server
 *       refuses it, where the parent opened it with them: a capability is the
 *       task's it was issued to, and the block server reads who sent the open
 *       from its trailer (#747);
 *   [5] nor can the child derive from it: cap_server answers
 *       CAP_ERR_NOT_OWNER (#747).
 *
 * The child says [2] and [3] itself, and leaves the number of them that were
 * WRONG as its exit status.  The parent waits for it for 10 s and no longer: a
 * request sent to a port nobody answers is a child that never ends, and that
 * is said too.  waitpid() honours WNOHANG only for any child, and this task has
 * one.
 */

#include <mach.h>
#include <mach/mach_port.h>
#include <mach/cap_types.h>
#include <libcap.h>
#include <mach_init.h>
#include <device/device.h>		/* #747: device_open_cap */
#include <servers/netname.h>		/* #747: disk0a's port */
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* The one id fork_cap_test.manifest grants, READ alone; it names no device. */
#define FCT_RESOURCE_ID		0x945
#define FCT_WAIT_MS		10000
#define FCT_POLL_MS		50

static int	arms_run;
static int	arms_passed;

/*
 * #747: this task's capability for disk0a, and how its own open with it went
 * -- asked before the fork, so that the child holds the same bytes.
 */
static struct uros_cap	fct_disk;
static kern_return_t	fct_disk_kr = KERN_FAILURE;
static kern_return_t	fct_open_kr = KERN_FAILURE;
static int		fct_no_disk;

static void
verdict(int arm, int ok, const char *fmt, ...)
{
	char	text[400];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(text, sizeof text, fmt, ap);
	va_end(ap);

	arms_run++;
	if (ok)
		arms_passed++;
	printf("fork_cap_test: [%d] %s%s\n", arm, ok ? "" : "WRONG -- ", text);
}

static void
not_asked(int arm, const char *why)
{
	printf("fork_cap_test: [%d] NOT ASKED -- %s\n", arm, why);
}

static void
nap(int ms)
{
	struct timespec	ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

/* The request a program makes, through libcap; the token is not kept. */
static kern_return_t
ask(void)
{
	struct uros_cap	cap;

	return cap_request(RESOURCE_BLK_DEVICE, FCT_RESOURCE_ID,
			   CAP_OP_BLK_READ, 0, &cap);
}

static const char *
answer_is(kern_return_t kr)
{
	if (kr == CAP_ERR_NOT_IN_MANIFEST)
		return "refused by cap_server with CAP_ERR_NOT_IN_MANIFEST";
	if (kr == KERN_SUCCESS)
		return "GRANTED, as its parent is: the child asked as its parent";
	return "not CAP_ERR_NOT_IN_MANIFEST(2413): the port it asked is not "
	       "cap_server's, or names nothing";
}

/*
 * #747: disk0a opened with `tok' and closed again: the block server's answer.
 * *no_disk is set when the name server has no disk0a to open.
 */
static kern_return_t
open_disk(const struct uros_cap *tok, int *no_disk)
{
	security_token_t	null_sec = { { 0, 0 } };
	char			blob[CAP_TOKEN_MAX];
	mach_port_t		part = MACH_PORT_NULL, handle = MACH_PORT_NULL;
	kern_return_t		kr;

	*no_disk = 0;
	if (netname_look_up(name_server_port, "", (char *)"disk0a", &part)
	    != KERN_SUCCESS) {
		*no_disk = 1;
		return KERN_FAILURE;
	}
	memcpy(blob, tok, sizeof(*tok));
	kr = device_open_cap(part, MACH_PORT_NULL, D_READ, null_sec,
			     (char *)"disk0a", blob,
			     (mach_msg_type_number_t)sizeof(*tok), &handle);
	if (kr == KERN_SUCCESS) {
		(void)device_close(handle);
		(void)mach_port_deallocate(mach_task_self(), handle);
	}
	(void)mach_port_deallocate(mach_task_self(), part);
	return kr;
}

/*
 * The child's half: [2] to [5], said here, and the count of them that were
 * WRONG left as the exit status.  It never returns.
 */
static void __attribute__((noreturn))
child(void)
{
	mach_port_t		asked, kernel_says = MACH_PORT_NULL;
	mach_port_type_t	type = 0;
	kern_return_t		kr, tkr, gkr;
	int			wrong = 0, ok;

	kr = ask();
	/* The port libcap asked, and what it is in this space. */
	asked = cap_server_port();
	tkr = mach_port_type(mach_task_self(), asked, &type);
	ok = kr == CAP_ERR_NOT_IN_MANIFEST;
	if (!ok)
		wrong++;
	verdict(2, ok, "a forked child asked for block device %#x READ through "
		"libcap and was answered %d, %s; it asked port %#x, whose type in "
		"its space is %#x (kr %d) (#945)", FCT_RESOURCE_ID, (int)kr,
		answer_is(kr), (unsigned)asked, (unsigned)type, (int)tkr);

	gkr = task_get_special_port(mach_task_self(), TASK_CAP_PORT,
				    &kernel_says);
	ok = mach_cap_port == MACH_PORT_NULL && gkr == KERN_SUCCESS &&
	     kernel_says == MACH_PORT_NULL;
	if (!ok)
		wrong++;
	verdict(3, ok, "the forked child's mach_cap_port is %#x, and the "
		"TASK_CAP_PORT the kernel gave it %#x (kr %d), where both must be "
		"null: a forked child holds no per-task port (#945)",
		(unsigned)mach_cap_port, (unsigned)kernel_says, (int)gkr);
	if (MACH_PORT_VALID(kernel_says))
		(void)mach_port_deallocate(mach_task_self(), kernel_says);

	/*
	 * #747: [4] and [5], with the bytes of the parent's capability, which
	 * this child's memory holds: a capability is its owner's, and the
	 * holder of a copy of it is somebody else.
	 */
	if (fct_no_disk) {
		not_asked(4, "no disk0a on this boot to open");
		not_asked(5, "no capability of the parent's to derive from");
	} else {
		struct uros_cap	kid;
		kern_return_t	okr, dkr;
		int		none;

		okr = open_disk(&fct_disk, &none);
		ok = fct_disk_kr == KERN_SUCCESS && fct_open_kr == KERN_SUCCESS &&
		     okr == KERN_NO_ACCESS;
		if (!ok)
			wrong++;
		verdict(4, ok, "the parent asked for disk0a READ (%d) and opened "
			"it with that capability (%d); this child, opening it with "
			"the same bytes, was answered %d by the block server, "
			"where it must be refused with KERN_NO_ACCESS(%d): the "
			"capability is its parent's (#747)", (int)fct_disk_kr,
			(int)fct_open_kr, (int)okr, (int)KERN_NO_ACCESS);

		memset(&kid, 0, sizeof(kid));
		dkr = cap_derive(&fct_disk, CAP_OP_BLK_READ, 0, &kid);
		ok = dkr == CAP_ERR_NOT_OWNER;
		if (!ok)
			wrong++;
		verdict(5, ok, "this child, deriving from its parent's "
			"capability, was answered %d by cap_server, where it must "
			"be CAP_ERR_NOT_OWNER(%d): only the owner derives (#747)",
			(int)dkr, (int)CAP_ERR_NOT_OWNER);
	}

	(void)fflush(stdout);
	_exit(wrong);
}

/*
 * Waits for the one child for FCT_WAIT_MS at most.  Returns its pid with
 * *status filled, or 0 if it did not end in time.
 */
static pid_t
wait_for_child(int *status)
{
	pid_t	got = 0;
	int	waited;

	for (waited = 0; got == 0 && waited < FCT_WAIT_MS;
	     waited += FCT_POLL_MS) {
		got = waitpid(-1, status, WNOHANG);
		if (got == 0)
			nap(FCT_POLL_MS);
	}
	return got;
}

/* [2] and [3]: fork, and read the child's count of its WRONG arms. */
static void
the_child_holds_no_per_task_port(void)
{
	pid_t	pid, got;
	int	status = 0;

	(void)fflush(stdout);
	pid = fork();
	if (pid < 0) {
		if (errno == EAGAIN) {
			not_asked(2, "fork refused: no process server on this "
				  "boot");
			not_asked(3, "no child");
			return;
		}
		verdict(2, 0, "fork failed, errno %d (#945)", errno);
		verdict(3, 0, "no child to ask: fork failed (#945)");
		return;
	}
	if (pid == 0)
		child();

	got = wait_for_child(&status);
	arms_run += fct_no_disk ? 2 : 4;
	if (got == 0) {
		(void)kill(pid, SIGKILL);
		if (wait_for_child(&status) == 0)
			printf("fork_cap_test: [2] WRONG -- the child did not end "
			       "in %d s, nor when it was killed (#945)\n",
			       FCT_WAIT_MS / 1000);
		else
			printf("fork_cap_test: [2] WRONG -- the child did not end "
			       "in %d s: its request was never answered, or it "
			       "never sent one (#945)\n", FCT_WAIT_MS / 1000);
		return;
	}
	if (got != pid || !WIFEXITED(status) ||
	    WEXITSTATUS(status) > (fct_no_disk ? 2 : 4)) {
		printf("fork_cap_test: [2] WRONG -- waitpid gave %d with status "
		       "%#x for the child %d, which leaves the count of its wrong "
		       "arms, 0 to %d (#945)\n", (int)got, (unsigned)status,
		       (int)pid, fct_no_disk ? 2 : 4);
		return;
	}
	arms_passed += (fct_no_disk ? 2 : 4) - WEXITSTATUS(status);
}

int
main(void)
{
	kern_return_t	kr;

	printf("fork_cap_test: started -- a child made by fork() holds none of "
	       "its parent's per-task port (#945)\n");

	if (mach_cap_port == MACH_PORT_NULL) {
		verdict(1, 0, "this task has no per-task port: bootstrap did not "
			"provision it with fork_cap_test.manifest (#945)");
	} else {
		kr = ask();
		verdict(1, kr == KERN_SUCCESS, "this task asked for block device "
			"%#x READ on its own per-task port %#x and was answered %d, "
			"where its manifest grants it (#945)", FCT_RESOURCE_ID,
			(unsigned)mach_cap_port, (int)kr);
	}

	/* #747: the parent's capability for disk0a, and its own open with it. */
	memset(&fct_disk, 0, sizeof(fct_disk));
	fct_disk_kr = cap_request(RESOURCE_BLK_DEVICE, cap_name_hash("disk0a"),
				  CAP_OP_BLK_READ, 0, &fct_disk);
	if (fct_disk_kr == KERN_SUCCESS)
		fct_open_kr = open_disk(&fct_disk, &fct_no_disk);

	the_child_holds_no_per_task_port();

	printf("fork_cap_test: %d of %d arms passed\n", arms_passed, arms_run);
	return 0;
}
