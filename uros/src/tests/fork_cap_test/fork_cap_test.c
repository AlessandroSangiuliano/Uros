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
 *       is the TASK_CAP_PORT the kernel gave it.
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
 * The child's half: [2] and [3], said here, and the count of them that were
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
	arms_run += 2;
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
	if (got != pid || !WIFEXITED(status) || WEXITSTATUS(status) > 2) {
		printf("fork_cap_test: [2] WRONG -- waitpid gave %d with status "
		       "%#x for the child %d, which leaves the count of its wrong "
		       "arms, 0 to 2 (#945)\n", (int)got, (unsigned)status,
		       (int)pid);
		return;
	}
	arms_passed += 2 - WEXITSTATUS(status);
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

	the_child_holds_no_per_task_port();

	printf("fork_cap_test: %d of %d arms passed\n", arms_passed, arms_run);
	return 0;
}
