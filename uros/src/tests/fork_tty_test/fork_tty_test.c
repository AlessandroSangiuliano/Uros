/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * fork_tty_test -- a child made by fork() writes on the terminal it
 * inherited (#948).
 *
 * It is typed at ush, so that it runs in a session with a controlling
 * terminal (scripts/ush-x86_64.py).  It opens /dev/tty, forks, and the child
 * writes a line on that fd before it exits.  A tty fd is not a libvfs fd: its
 * I/O goes to char_server through the port libposix-uros looked up for this
 * task's tty use, and the child has it only if fork() copied it into the
 * child's space under its name.  Before #948 it did not, and the child's write
 * went to whatever the name named there: it was answered EIO, and the line
 * never reached the terminal.
 *
 *   [1] the child's write on the terminal it inherited takes the whole line.
 *
 * The child leaves its answer as its exit status: 0 for the whole line, the
 * errno for a refusal, FTT_SHORT for part of it.  The parent waits for it for
 * 10 s and no longer, since a request sent to a port nobody answers is a child
 * that never ends.  waitpid() honours WNOHANG only for any child, and this task
 * has one.  ush-x86_64.py asks for the child's line on the terminal as well as
 * for the verdict: a write that says it took the line is not the line arriving.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define FTT_SHORT	200
#define FTT_WAIT_MS	10000
#define FTT_POLL_MS	50

static const char child_line[] =
	"fork_tty_test: the child's line, on the terminal it inherited (#948)\r\n";

static void
nap(int ms)
{
	struct timespec	ts;

	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (long)(ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
}

/* The one child, for FTT_WAIT_MS at most: its pid with *status filled, or 0. */
static pid_t
wait_for_child(int *status)
{
	pid_t	got = 0;
	int	waited;

	for (waited = 0; got == 0 && waited < FTT_WAIT_MS;
	     waited += FTT_POLL_MS) {
		got = waitpid(-1, status, WNOHANG);
		if (got == 0)
			nap(FTT_POLL_MS);
	}
	return got;
}

static void __attribute__((noreturn))
child(int fd)
{
	ssize_t	w = write(fd, child_line, sizeof child_line - 1);

	if (w == (ssize_t)(sizeof child_line - 1))
		_exit(0);
	if (w < 0)
		_exit(errno > 0 && errno < FTT_SHORT ? errno : FTT_SHORT + 1);
	_exit(FTT_SHORT);
}

int
main(void)
{
	pid_t	pid, got;
	int	fd, status = 0, s, passed = 0;

	printf("fork_tty_test: started -- a child made by fork() writes on the "
	       "terminal it inherited (#948)\n");

	fd = open("/dev/tty", O_RDWR);
	if (fd < 0) {
		printf("fork_tty_test: [1] NOT ASKED -- open(/dev/tty) answered "
		       "errno %d: no controlling terminal, so run it from ush "
		       "(#948)\n", errno);
		printf("fork_tty_test: 0 of 0 arms passed\n");
		return 0;
	}

	(void)fflush(stdout);
	pid = fork();
	if (pid < 0) {
		printf("fork_tty_test: [1] WRONG -- fork failed, errno %d (#948)\n",
		       errno);
		printf("fork_tty_test: 0 of 1 arms passed\n");
		return 0;
	}
	if (pid == 0)
		child(fd);

	got = wait_for_child(&status);
	if (got == 0) {
		(void)kill(pid, SIGKILL);
		(void)wait_for_child(&status);
		printf("fork_tty_test: [1] WRONG -- the child did not end in %d s: "
		       "its write on the terminal never came back (#948)\n",
		       FTT_WAIT_MS / 1000);
	} else if (got != pid || !WIFEXITED(status)) {
		printf("fork_tty_test: [1] WRONG -- waitpid gave %d with status "
		       "%#x for the child %d (#948)\n", (int)got,
		       (unsigned)status, (int)pid);
	} else if ((s = WEXITSTATUS(status)) == 0) {
		passed = 1;
		printf("fork_tty_test: [1] a forked child wrote its whole line, "
		       "%u bytes, on the terminal fd %d it inherited (#948)\n",
		       (unsigned)(sizeof child_line - 1), fd);
	} else if (s == FTT_SHORT) {
		printf("fork_tty_test: [1] WRONG -- a forked child's write on the "
		       "terminal fd %d it inherited took part of its line (#948)\n",
		       fd);
	} else {
		printf("fork_tty_test: [1] WRONG -- a forked child's write on the "
		       "terminal fd %d it inherited was answered errno %d, where "
		       "it must take the whole line (#948)\n", fd, s);
	}
	printf("fork_tty_test: %d of 1 arms passed\n", passed);
	(void)close(fd);
	return 0;
}
