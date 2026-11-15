/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A static C program asks its C library what a program asks it (#414).
 *
 * This is the box "a static musl binary runs", and it is written as a program
 * and not as a test of libposix-uros: nothing here knows it is on Mach, there
 * is no Mach header and no trap, only POSIX and musl's gettid() (arm 24).
 * An arm that passes says the whole path holds, from the libc call through
 * musl's patched syscall_arch.h and the dispatch table to the kernel, on the
 * numbers and the structure layouts of this target.
 *
 * Each arm reads back what it did.  Where an arm needs something this boot may
 * not have -- a disk, a process server -- it says NOT ASKED and does not count.
 */

#define _GNU_SOURCE		/* gettid() */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>	/* PIPE_BUF (#904) */
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>	/* gettimeofday (#923) */
#include <sys/uio.h>	/* writev (#904) */
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE		4096u

static int	arms_run;
static int	arms_passed;

/*
 * One arm's verdict, one line.  `ok' is the arm's own conclusion and what
 * follows says what was read, either way.  A longer line is cut, and an
 * ablation's Expect: that matches its end then matches nothing: [25]'s is
 * near 300 characters.
 */
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
	printf("musl_test: [%d] %s%s\n", arm, ok ? "" : "WRONG -- ", text);
}

static void
not_asked(int arm, const char *why)
{
	printf("musl_test: [%d] NOT ASKED -- %s\n", arm, why);
}

/* The longest a thread is given to do what an arm waits for, in 10 ms steps. */
#define WAIT_STEPS	500

static int
access_by_stat(const char *path)
{
	struct stat	st;

	return stat(path, &st);
}

static void
pause_a_little(void)
{
	struct timespec	ts = { 0, 10 * 1000 * 1000 };

	(void)nanosleep(&ts, NULL);
}

/*
 * ── [1] Formatting, which is varargs and floating point ─────────────────
 *
 * x86-64 passes a double in a vector register and a long in a general one, and
 * a va_list is a structure and not a pointer: all of it is musl's and the
 * compiler's, and all of it needs the SSE state the kernel keeps for a thread.
 */
static void
arm_format(void)
{
	char		buf[80];
	const char	*want = "42 word 2.500 deadbeefcafe";

	snprintf(buf, sizeof buf, "%d %s %.3f %lx", 42, "word", 2.5,
		 0xdeadbeefcafeUL);
	verdict(1, strcmp(buf, want) == 0, "snprintf gave \"%s\"", buf);
}

/*
 * ── [2] The allocator, which on this system is mmap ─────────────────────
 *
 * brk answers ENOMEM, so everything malloc() hands out comes from the handler
 * that has a different name and a different unit on this target.
 */
static void
arm_malloc(void)
{
	static const size_t	sizes[] = { 16, 4096, 70000, 1 << 21 };
	size_t			i, j;

	for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		unsigned char	*p = malloc(sizes[i]);

		if (p == NULL) {
			verdict(2, 0, "malloc(%zu) failed, errno %d", sizes[i],
				errno);
			return;
		}
		for (j = 0; j < sizes[i]; j += 512)
			p[j] = (unsigned char)(j >> 9) ^ 0x5a;
		for (j = 0; j < sizes[i]; j += 512)
			if (p[j] != ((unsigned char)(j >> 9) ^ 0x5a)) {
				verdict(2, 0, "byte %zu of a %zu-byte block "
					"changed", j, sizes[i]);
				return;
			}
		free(p);
	}
	verdict(2, 1, "four blocks up to 2 MiB written, read back and freed");
}

/*
 * ── [3] mmap, mprotect, munmap, by address ──────────────────────────────
 */
static void
arm_mmap(void)
{
	unsigned char	*p;
	unsigned char	*again;

	p = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE,
		 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED) {
		verdict(3, 0, "mmap failed, errno %d", errno);
		return;
	}
	if ((uintptr_t)p & (PAGE - 1)) {
		verdict(3, 0, "mmap answered %p, not on a page", (void *)p);
		return;
	}
	p[0] = 1;
	p[2 * PAGE] = 3;
	if (mprotect(p + PAGE, PAGE, PROT_READ) != 0) {
		verdict(3, 0, "mprotect failed, errno %d", errno);
		return;
	}
	if (munmap(p, 3 * PAGE) != 0) {
		verdict(3, 0, "munmap failed, errno %d", errno);
		return;
	}
	/* Uros's MAP_FIXED is refused on a busy address, so this asks whether
	 * the range really went. */
	again = mmap(p, PAGE, PROT_READ | PROT_WRITE,
		     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (again != p) {
		verdict(3, 0, "the range at %p did not come back after "
			"munmap: %p, errno %d", (void *)p, (void *)again,
			errno);
		return;
	}
	if (again[0] != 0) {
		verdict(3, 0, "a fresh page read %u", again[0]);
		return;
	}
	(void)munmap(again, PAGE);
	verdict(3, 1, "three pages mapped, protected and unmapped; the "
		"address was free again and read zero");
}

/*
 * ── [4] errno and __thread: what the FS base is for ─────────────────────
 *
 * errno lives in musl's thread structure and a __thread variable in the block
 * below it, and both are reached through %fs.  The initial value of tls_word
 * comes from the program's PT_TLS segment, so a program started with no view of
 * its own headers reads zero, or somebody else's word.
 */
static __thread int	tls_word = 0x5a5a;
static __thread long	tls_zero;

struct tls_report {
	int	first_word;
	long	first_zero;
	int	errno_kept;
	int	word_kept;
	int	done;
};

static void *
tls_thread(void *arg)
{
	struct tls_report	*r = arg;

	r->first_word = tls_word;
	r->first_zero = tls_zero;
	errno = ERANGE;
	tls_word = 0x1111;
	pause_a_little();		/* a processor, and back */
	r->errno_kept = (errno == ERANGE);
	r->word_kept = (tls_word == 0x1111);
	__atomic_store_n(&r->done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void
arm_tls(void)
{
	static struct tls_report	r;	/* the thread may never get to it */
	pthread_t		th;
	int			rc, i;

	if (tls_word != 0x5a5a || tls_zero != 0) {
		verdict(4, 0, "the main thread's __thread variables read %#x "
			"and %ld", tls_word, tls_zero);
		return;
	}
	memset(&r, 0, sizeof r);
	errno = EDOM;
	tls_word = 0x2222;
	rc = pthread_create(&th, NULL, tls_thread, &r);
	if (rc != 0) {
		verdict(4, 0, "pthread_create said %d", rc);
		return;
	}
	/*
	 * Waited for by hand and not by pthread_join() alone: a thread with no
	 * thread pointer dies on its first read of it, and the arm has to say
	 * so and not stop here for good.  pause_a_little() keeps errno, and
	 * the check below reads it.
	 */
	for (i = 0; i < WAIT_STEPS && !__atomic_load_n(&r.done, __ATOMIC_SEQ_CST);
	     i++)
		pause_a_little();
	if (!__atomic_load_n(&r.done, __ATOMIC_SEQ_CST)) {
		verdict(4, 0, "a new thread never got as far as its own word");
		return;
	}
	(void)pthread_join(th, NULL);
	if (errno != EDOM || tls_word != 0x2222) {
		verdict(4, 0, "the main thread's errno became %d and its "
			"word %#x while another thread wrote its own", errno,
			tls_word);
		return;
	}
	verdict(4, r.first_word == 0x5a5a && r.first_zero == 0 && r.errno_kept
		&& r.word_kept, "a new thread started with word %#x and %ld, "
		"and kept its own errno (%d) and word (%d)", r.first_word,
		r.first_zero, r.errno_kept, r.word_kept);
}

/*
 * ── [5] Threads, a mutex and a condition variable ───────────────────────
 */
#define T_THREADS	8
#define T_ROUNDS	20000

static pthread_mutex_t	t_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t	t_cond = PTHREAD_COND_INITIALIZER;
static int		t_count;
static int		t_released;

static void *
count_thread(void *arg)
{
	int	i;

	(void)arg;
	(void)pthread_mutex_lock(&t_lock);
	while (!t_released)
		(void)pthread_cond_wait(&t_cond, &t_lock);
	(void)pthread_mutex_unlock(&t_lock);

	for (i = 0; i < T_ROUNDS; i++) {
		(void)pthread_mutex_lock(&t_lock);
		t_count++;
		(void)pthread_mutex_unlock(&t_lock);
	}
	return arg;
}

static void
arm_threads(void)
{
	pthread_t	th[T_THREADS];
	void		*back;
	int		i, rc, bad = 0;

	for (i = 0; i < T_THREADS; i++) {
		rc = pthread_create(&th[i], NULL, count_thread,
				    (void *)(uintptr_t)(i + 1));
		if (rc != 0) {
			verdict(5, 0, "pthread_create %d said %d", i, rc);
			return;
		}
	}
	(void)pthread_mutex_lock(&t_lock);
	t_released = 1;
	(void)pthread_cond_broadcast(&t_cond);
	(void)pthread_mutex_unlock(&t_lock);

	for (i = 0; i < T_THREADS; i++) {
		(void)pthread_join(th[i], &back);
		if ((uintptr_t)back != (uintptr_t)(i + 1))
			bad++;
	}
	verdict(5, t_count == T_THREADS * T_ROUNDS && bad == 0,
		"%d threads counted to %d under a mutex (want %d), and "
		"%d join answers were wrong", T_THREADS, t_count,
		T_THREADS * T_ROUNDS, bad);
}

/*
 * ── [6] A detached thread gives back the stack it stands on ─────────────
 *
 * musl's __unmapself cannot be a C call on any target: the stack goes in the
 * middle of it.  Once the thread is gone, the address its stack had can be
 * asked for again, and MAP_FIXED here is refused if it is still in use.
 */
#define D_THREADS	16

static int		d_done;
static uintptr_t	d_stack[D_THREADS];

static void *
detached_thread(void *arg)
{
	char	here;

	d_stack[(uintptr_t)arg] = (uintptr_t)&here;
	__atomic_add_fetch(&d_done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

/*
 * A thread that has counted itself still has its way to go, so the answer may
 * take a moment.  The moments are one budget for the whole arm: it was one per
 * thread, and on a kernel where no stack ever comes back that is eighty seconds,
 * by which time the boot had ended with the verdict unsaid.
 */
static int
address_is_free(uintptr_t where, int *steps_left)
{
	uintptr_t	page = where & ~(uintptr_t)(PAGE - 1);
	void		*p;

	for (;;) {
		p = mmap((void *)page, PAGE, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
		if (p == (void *)page) {
			(void)munmap(p, PAGE);
			return 1;
		}
		if (*steps_left <= 0)
			return 0;
		(*steps_left)--;
		pause_a_little();
	}
}

static void
arm_detached(void)
{
	pthread_attr_t	attr;
	pthread_t	th;
	int		i, rc, freed = 0, steps = WAIT_STEPS;

	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (i = 0; i < D_THREADS; i++) {
		rc = pthread_create(&th, &attr, detached_thread,
				    (void *)(uintptr_t)i);
		if (rc != 0) {
			verdict(6, 0, "pthread_create %d said %d", i, rc);
			return;
		}
	}
	(void)pthread_attr_destroy(&attr);

	for (i = 0; i < WAIT_STEPS && __atomic_load_n(&d_done, __ATOMIC_SEQ_CST) < D_THREADS; i++)
		pause_a_little();
	if (__atomic_load_n(&d_done, __ATOMIC_SEQ_CST) < D_THREADS) {
		verdict(6, 0, "%d of %d detached threads ran",
			__atomic_load_n(&d_done, __ATOMIC_SEQ_CST), D_THREADS);
		return;
	}
	for (i = 0; i < D_THREADS; i++)
		freed += address_is_free(d_stack[i], &steps);
	verdict(6, freed == D_THREADS, "%d detached threads ended and %d of "
		"their stacks were given back", D_THREADS, freed);
}

/*
 * ── [7] Signal actions and masks, in the kernel's layouts ───────────────
 *
 * Nothing is delivered here: that is sig_test's.  This is the structure the
 * rt_ calls carry, which has cost two defects on i386 (#397, #401) and whose
 * words are another size here.
 */
static void
handler_a(int sig)
{
	(void)sig;
}

static void
arm_signals(void)
{
	struct sigaction	sa, old;
	sigset_t		set, was;
	struct {
		sigset_t	set;
		uint64_t	guard;
	} back;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = handler_a;
	sa.sa_flags = SA_RESTART;
	(void)sigemptyset(&sa.sa_mask);
	(void)sigaddset(&sa.sa_mask, SIGTERM);
	if (sigaction(SIGUSR1, &sa, NULL) != 0) {
		verdict(7, 0, "sigaction failed, errno %d", errno);
		return;
	}
	memset(&old, 0xff, sizeof old);
	if (sigaction(SIGUSR1, NULL, &old) != 0) {
		verdict(7, 0, "reading the action failed, errno %d", errno);
		return;
	}
	if (old.sa_handler != handler_a || !(old.sa_flags & SA_RESTART)
	    || !sigismember(&old.sa_mask, SIGTERM)) {
		verdict(7, 0, "the action came back different: handler %p, "
			"flags %#x", (void *)(uintptr_t)old.sa_handler,
			(unsigned)old.sa_flags);
		return;
	}

	(void)sigemptyset(&set);
	(void)sigaddset(&set, SIGUSR2);
	(void)sigaddset(&set, SIGRTMIN + 5);	/* above bit 31 */
	if (sigprocmask(SIG_BLOCK, &set, &was) != 0) {
		verdict(7, 0, "sigprocmask failed, errno %d", errno);
		return;
	}
	memset(&back, 0, sizeof back);
	back.guard = 0x1234567890abcdefULL;
	if (sigprocmask(SIG_SETMASK, &was, &back.set) != 0) {
		verdict(7, 0, "sigprocmask failed reading back, errno %d",
			errno);
		return;
	}
	verdict(7, sigismember(&back.set, SIGUSR2) == 1
		&& sigismember(&back.set, SIGRTMIN + 5) == 1
		&& back.guard == 0x1234567890abcdefULL,
		"the action came back whole, and the mask with a signal on "
		"each side of bit 31, nothing written past it");
}

/*
 * ── [8] A descriptor that is not a file ─────────────────────────────────
 */
static void
arm_console(void)
{
	struct stat	st;
	off_t		off;
	int		rc;

	rc = fstat(1, &st);
	if (rc != 0) {
		verdict(8, 0, "fstat(1) failed, errno %d", errno);
		return;
	}
	off = lseek(1, 0, SEEK_CUR);
	verdict(8, S_ISCHR(st.st_mode) && off == (off_t)-1 && errno == ESPIPE,
		"standard output has mode %#o, and lseek on it answered %ld "
		"with errno %d", (unsigned)st.st_mode, (long)off, errno);
}

/*
 * ── [9] A file: stat, fstat, lseek and read agree ───────────────────────
 *
 * On x86-64 the stat family and lseek are different numbers with other
 * structures behind them, and a size read through the wrong layout does not
 * look wrong.  So three ways of asking for the size must give one answer, and
 * the bytes at an offset must be the bytes that offset holds.
 */
#define F_PATH		"/mach_servers/cow_test"	/* on the root disk, and nobody writes it */
#define F_MOUNT_WAIT	(15 * 100)			/* 15 s of 10 ms steps */

static void
arm_file(void)
{
	struct stat	by_name, by_fd;
	unsigned char	head[16], again[16];
	off_t		end, middle;
	int		fd, i;

	for (i = 0; i < F_MOUNT_WAIT; i++) {
		if (stat(F_PATH, &by_name) == 0)
			break;
		pause_a_little();
	}
	if (i == F_MOUNT_WAIT) {
		not_asked(9, "no file on a mounted disk on this boot");
		return;
	}
	fd = open(F_PATH, O_RDONLY);
	if (fd < 0) {
		verdict(9, 0, "open failed, errno %d", errno);
		return;
	}
	if (fstat(fd, &by_fd) != 0) {
		verdict(9, 0, "fstat failed, errno %d", errno);
		(void)close(fd);
		return;
	}
	end = lseek(fd, 0, SEEK_END);
	middle = end / 2;
	if (lseek(fd, middle, SEEK_SET) != middle
	    || read(fd, head, sizeof head) != (ssize_t)sizeof head
	    || lseek(fd, middle, SEEK_SET) != middle
	    || read(fd, again, sizeof again) != (ssize_t)sizeof again) {
		verdict(9, 0, "seeking and reading at %ld failed, errno %d",
			(long)middle, errno);
		(void)close(fd);
		return;
	}
	(void)close(fd);
	verdict(9, by_name.st_size == by_fd.st_size && by_fd.st_size == end
		&& S_ISREG(by_fd.st_mode) && by_fd.st_nlink >= 1
		&& memcmp(head, again, sizeof head) == 0,
		"stat, fstat and lseek each said %ld, %ld, %ld bytes, mode "
		"%#o, and the same offset read the same bytes twice",
		(long)by_name.st_size, (long)by_fd.st_size, (long)end,
		(unsigned)by_fd.st_mode);
}

/*
 * ── [10] nanosleep's timespec ───────────────────────────────────────────
 *
 * The layout is the point: a good one sleeps and a bad one is refused, and with
 * the two fields read at the wrong width both answers would be other answers.
 */
static void
arm_sleep(void)
{
	struct timespec	good = { 0, 2 * 1000 * 1000 };
	struct timespec	bad = { 0, 2000 * 1000 * 1000L };
	int		rc_good, rc_bad, err_bad;

	rc_good = nanosleep(&good, NULL);
	rc_bad = nanosleep(&bad, NULL);
	err_bad = errno;
	verdict(10, rc_good == 0 && rc_bad == -1 && err_bad == EINVAL,
		"2 ms answered %d, and two billion nanoseconds %d with errno "
		"%d", rc_good, rc_bad, err_bad);
}

/*
 * ── [11] setjmp, and a canary that is there ─────────────────────────────
 *
 * This file is built with -fstack-protector-strong, so every function above
 * with an array has read %fs:0x28 going in and coming out.  What is left to ask
 * is that the word is not the zero it would be with nothing behind it.
 */
static void
arm_jump(void)
{
	jmp_buf			env;
	volatile int		count = 0;
	unsigned long		canary;

	__asm__ __volatile__("movq %%fs:0x28, %0" : "=r"(canary));
	if (setjmp(env) == 0) {
		count = 1;
		longjmp(env, 1);
	}
	count++;
	verdict(11, count == 2 && canary != 0, "longjmp came back with count "
		"%d, and the canary is %s", count, canary ? "set" : "zero");
}

/*
 * ── [12] fork and waitpid ───────────────────────────────────────────────
 *
 * The child writes a global, which it has its own copy of, and leaves a status.
 * fork needs a process server to register the child with.
 */
static int	fork_word = 7;

static void
arm_fork(void)
{
	pid_t	pid, got;
	int	status = 0;

	pid = fork();
	if (pid < 0) {
		if (errno == EAGAIN) {
			not_asked(12, "fork refused: no process server on "
				  "this boot");
			return;
		}
		verdict(12, 0, "fork failed, errno %d", errno);
		return;
	}
	if (pid == 0) {
		fork_word = 8;
		_exit(fork_word == 8 ? 42 : 43);
	}
	got = waitpid(pid, &status, 0);
	verdict(12, got == pid && WIFEXITED(status)
		&& WEXITSTATUS(status) == 42 && fork_word == 7,
		"child %d left status %#x, and the parent's word is still %d",
		(int)pid, (unsigned)status, fork_word);
}

/*
 * ── [13] A file mapped at an offset ─────────────────────────────────────
 *
 * This is where mmap and mmap2 differ: the same offset in bytes here and in
 * pages there.  So the page at one page into the file, read and mapped, must
 * be one page of bytes.
 */
static void
arm_file_map(void)
{
	unsigned char	by_read[PAGE];
	unsigned char	*mapped;
	int		fd, same;

	fd = open(F_PATH, O_RDONLY);
	if (fd < 0) {
		not_asked(13, "no file on a mounted disk on this boot");
		return;
	}
	if (lseek(fd, PAGE, SEEK_SET) != PAGE
	    || read(fd, by_read, PAGE) != (ssize_t)PAGE) {
		verdict(13, 0, "reading the second page failed, errno %d",
			errno);
		(void)close(fd);
		return;
	}
	mapped = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, PAGE);
	if (mapped == MAP_FAILED) {
		verdict(13, 0, "mmap of the file failed, errno %d", errno);
		(void)close(fd);
		return;
	}
	same = memcmp(mapped, by_read, PAGE) == 0;
	(void)munmap(mapped, PAGE);
	(void)close(fd);
	verdict(13, same, "the second page of a file, mapped and read, was "
		"%s", same ? "the same" : "not the same");
}

/*
 * ── [14], [15], [16] Another program, through exec_server ───────────────
 *
 * A static C program and a dynamic one, each of which returns from main, and
 * hello_exec, which has nothing under it and waits to be killed.  The status
 * waitpid reads back is the whole of what the parent can see here: what each
 * of them printed on its way is the harness's to read.
 */
/*
 * No disk is a boot this arm has nothing to say about.  A disk with the file
 * the other arms read and without this program is an image built wrong, and it
 * said NOT ASKED about that too, in a boot that looked clean.
 */
static int
program_is_there(int arm, const char *path)
{
	if (access_by_stat(path) == 0)
		return 1;
	if (access_by_stat(F_PATH) == 0)
		verdict(arm, 0, "a disk is mounted, and %s is not on it", path);
	else
		not_asked(arm, "no disk mounted on this boot");
	return 0;
}

static pid_t
start_program(const char *path)
{
	char	*argv[2];
	char	*envp[1];
	pid_t	pid;

	argv[0] = (char *)path;
	argv[1] = NULL;
	envp[0] = NULL;
	pid = fork();
	if (pid == 0) {
		(void)execve(path, argv, envp);
		_exit(127);
	}
	return pid;
}

static void
arm_program(int arm, const char *path)
{
	pid_t	pid, got;
	int	status = 0;

	if (!program_is_there(arm, path))
		return;
	pid = start_program(path);
	if (pid < 0) {
		not_asked(arm, "fork refused: no process server on this boot");
		return;
	}
	got = waitpid(pid, &status, 0);
	verdict(arm, got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"%s ended with status %#x", path, (unsigned)status);
}

static void
arm_parked_program(int arm, const char *path)
{
	pid_t	pid, got;
	int	status = 0;

	if (!program_is_there(arm, path))
		return;
	pid = start_program(path);
	if (pid < 0) {
		not_asked(arm, "fork refused: no process server on this boot");
		return;
	}
	(void)sleep(1);
	(void)kill(pid, SIGKILL);
	got = waitpid(pid, &status, 0);
	verdict(arm, got == pid && WIFSIGNALED(status)
		&& WTERMSIG(status) == SIGKILL, "%s was killed, status %#x",
		path, (unsigned)status);
}

/*
 * ── [17], [18] What waitpid says about a task that did not exit ─────────
 *
 * The address is the one the harness knows to be asked for.  A program with
 * this library under it catches the exception and says what it was; hello_exec
 * has nothing, and proc_server sees only that it has gone.  Either way the
 * status must not read as an exit.
 */
#define FAULT_ADDRESS	0x5300000

static void
arm_fault(void)
{
	pid_t	pid, got;
	int	status = 0;

	pid = fork();
	if (pid < 0) {
		not_asked(17, "fork refused: no process server on this boot");
		return;
	}
	if (pid == 0) {
		*(volatile int *)FAULT_ADDRESS = 1;
		_exit(0);
	}
	got = waitpid(pid, &status, 0);
	verdict(17, got == pid && WIFSIGNALED(status)
		&& WTERMSIG(status) == SIGSEGV, "a child that wrote to a page "
		"nobody mapped left status %#x", (unsigned)status);
}

static void
arm_fault_with_no_libc(void)
{
	char	*argv[3];
	char	*envp[1];
	pid_t	pid, got;
	int	status = 0;

	if (!program_is_there(18, "/hello_exec"))
		return;
	argv[0] = (char *)"/hello_exec";
	argv[1] = (char *)"fault";
	argv[2] = NULL;
	envp[0] = NULL;
	pid = fork();
	if (pid < 0) {
		not_asked(18, "fork refused: no process server on this boot");
		return;
	}
	if (pid == 0) {
		(void)execve(argv[0], argv, envp);
		_exit(127);
	}
	got = waitpid(pid, &status, 0);
	verdict(18, got == pid && WIFSIGNALED(status), "a program with no "
		"library that faulted left status %#x", (unsigned)status);
}

/*
 * ── [19] An exec'd program and page zero ────────────────────────────────
 *
 * This program again, through exec_server, with an argument.  Everything here
 * so far is a task bootstrap loaded, and bootstrap keeps page zero out of a
 * task's reach; what exec_server starts has to have it kept out as well.
 */
/*
 * ── [20] /proc/N/stat ───────────────────────────────────────────────────
 *
 * The one surface of proc_server a program reads as a file.  It is text, so
 * what can go wrong on this target is where it is served from and whether the
 * numbers in it are the process that asked (#496).
 */
static void
arm_proc(void)
{
	char	path[32], buf[256];
	char	state = 0;
	int	fd, pid = 0;
	ssize_t	n;

	snprintf(path, sizeof path, "/proc/%d/stat", (int)getpid());
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		verdict(20, 0, "open(%s) failed, errno %d", path, errno);
		return;
	}
	n = read(fd, buf, sizeof buf - 1);
	(void)close(fd);
	if (n <= 0) {
		verdict(20, 0, "reading %s answered %ld, errno %d", path,
			(long)n, errno);
		return;
	}
	buf[n] = '\0';
	(void)sscanf(buf, "%d (%*[^)]) %c", &pid, &state);
	verdict(20, pid == (int)getpid() && state == 'R', "%s says pid %d, "
		"state %c", path, pid, state);
}

/*
 * ── [24] A thread knows who it is from its first instruction ───────────
 *
 * musl owns a mutex by the thread's id, so a thread that took one before it
 * had an id could not give it back once it had: an error-checking mutex says
 * so, EPERM, where the others let it pass.  So the first thing each of these
 * does is to ask for its id, and the next to take a mutex, which it holds
 * until it has one.  It runs before the arms that make threads in earnest,
 * because what this leaves behind stops them.  A mutex each: one refused and
 * left on a thread's robust list, then taken by the next thread, ties the two
 * lists in a knot, and the first of them never gets out to let this arm answer.
 *
 * And none of them ends before the arm has answered.  A thread that ends goes
 * through the thread list, which one with no id can tear: in 3 boots of 8 of
 * 733-tid-after-resume, under KVM and TCG, this program died in pthread_exit,
 * at a null pointer, before this arm had said a word.
 */
#define O_THREADS	8

static pthread_mutex_t	o_lock[O_THREADS];
static int		o_nameless, o_refused, o_answered, o_go;

static void *
own_id_thread(void *arg)
{
	pthread_mutex_t	*m = arg;
	pid_t		first = gettid();
	int		i, rc;

	rc = pthread_mutex_lock(m);
	if (rc == 0) {
		pause_a_little();
		for (i = 0; i < WAIT_STEPS && gettid() == 0; i++)
			pause_a_little();
		rc = pthread_mutex_unlock(m);
	}
	if (first == 0)
		__atomic_add_fetch(&o_nameless, 1, __ATOMIC_SEQ_CST);
	if (rc != 0)
		__atomic_add_fetch(&o_refused, 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&o_answered, 1, __ATOMIC_SEQ_CST);
	while (!__atomic_load_n(&o_go, __ATOMIC_SEQ_CST))
		pause_a_little();
	return NULL;
}

static void
arm_own_id(void)
{
	pthread_mutexattr_t	attr;
	pthread_t		th[O_THREADS];
	int			i, rc = 0, made = 0, waited;
	int			answered, nameless, refused;

	(void)pthread_mutexattr_init(&attr);
	(void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
	for (i = 0; i < O_THREADS; i++)
		(void)pthread_mutex_init(&o_lock[i], &attr);
	(void)pthread_mutexattr_destroy(&attr);
	for (i = 0; i < O_THREADS && rc == 0; i++) {
		rc = pthread_create(&th[i], NULL, own_id_thread, &o_lock[i]);
		if (rc == 0)
			made++;
	}
	for (waited = 0; waited < WAIT_STEPS &&
	     __atomic_load_n(&o_answered, __ATOMIC_SEQ_CST) < made; waited++)
		pause_a_little();
	answered = __atomic_load_n(&o_answered, __ATOMIC_SEQ_CST);
	nameless = __atomic_load_n(&o_nameless, __ATOMIC_SEQ_CST);
	refused = __atomic_load_n(&o_refused, __ATOMIC_SEQ_CST);
	if (rc != 0)
		verdict(24, 0, "pthread_create %d said %d", made, rc);
	else if (answered < made)
		verdict(24, 0, "of %d threads, %d had answered after %d "
			"sleeps of 10 ms asked", made, answered, waited);
	else
		verdict(24, nameless == 0 && refused == 0,
			"%d threads asked for their id as their first act, %d "
			"got none, and %d were refused back the error-checking "
			"mutex they took next", made, nameless, refused);
	/* Only now may they end. */
	__atomic_store_n(&o_go, 1, __ATOMIC_SEQ_CST);
	for (i = 0; i < made; i++)
		(void)pthread_join(th[i], NULL);
}

/*
 * ── [22] Threads in the servers at once ─────────────────────────────────
 *
 * Four kinds of request to two servers, from four threads, so that a reply that
 * reached the wrong thread is a reply of the wrong shape, and a thread whose
 * reply was taken never comes back.
 *
 * They go at the pace of the slowest.  A stat takes 3.8 ms under KVM and 31 ms
 * under TCG, and a thousand of each of the others take 30 ms between them, so
 * with a thousand rounds each those three were over after three stats, and
 * stat went on alone, where nothing could take its reply, past this arm's 15 s
 * on every boot under TCG.
 *
 * And it waits for them.  On one processor stat and its server keep it for a
 * quantum, and under KVM the 20 were over before any of the others had
 * finished a call.
 *
 * 🔴 #922: SLOW IS NOT LOST.  The wait was 15 s for the whole arm, and under
 * TCG at -smp 4 on GitHub's EPYC 7763 a stat took about a second (two disk
 * reads each, #912): 12 and 16 of the 20 in three boots, with no wrong answer
 * in any thread, and the arm said WRONG.  The defect it is for is a reply that
 * reached the wrong thread, whose own thread then never comes back.  So the
 * stat thread ends on its count or, once it has a few, on its time, and the
 * wait ends when the four have ended or when one of them has completed no call
 * for R_STALL_STEPS -- per thread, because with the stat thread stuck the fast
 * three would go on completing calls for ever.  WRONG names the thread that
 * stopped.
 */
#define R_THREADS	4
#define R_ROUNDS	1000	/* the least of each of the fast three */
#define R_STAT		3	/* the kind that goes to the file server */
#define R_STATS		20	/* and how many of it, which ends all four */
#define R_STATS_LEAST	3	/* #922: the fewest, when its time ends it */
#define R_STAT_STEPS	(2 * WAIT_STEPS)	/* #922: the stat thread's time */
#define R_STALL_STEPS	(3 * WAIT_STEPS)	/* #922: no call this long */

static const char *const r_kind[R_THREADS] = {
	"getsid", "getpgid", "kill", "stat"
};
static int	r_wrong[R_THREADS], r_round[R_THREADS], r_ended[R_THREADS];
static int	r_done, r_over;
/*
 * #922: the stat thread's time is up -- raised by the wait, which counts it in
 * the same 10 ms sleeps as everything else here.  Not clock_gettime(): when
 * this was written no handler served it in libposix-uros (#923), and the
 * first version of this read a timespec the call had never written, so the
 * time never ran out.
 */
static int	r_stat_time_up;

static int
rpc_others_under_way(void)
{
	int	k;

	for (k = 0; k < R_STAT; k++)
		if (__atomic_load_n(&r_round[k], __ATOMIC_SEQ_CST) == 0)
			return 0;
	return 1;
}

static void *
rpc_thread(void *arg)
{
	int		kind = (int)(uintptr_t)arg;
	int		i, ok;

	while (kind == R_STAT && !rpc_others_under_way()
	       && !__atomic_load_n(&r_over, __ATOMIC_SEQ_CST))
		pause_a_little();
	for (i = 0; ; i++) {
		struct stat	st;

		switch (kind) {
		case 0:
			ok = getsid(0) > 0;
			break;
		case 1:
			ok = getpgid(0) > 0;
			break;
		case 2:
			ok = kill(getpid(), 0) == 0;
			break;
		default:
			ok = stat(F_PATH, &st) == 0;
			break;
		}
		if (!ok)
			__atomic_add_fetch(&r_wrong[kind], 1, __ATOMIC_SEQ_CST);
		__atomic_store_n(&r_round[kind], i + 1, __ATOMIC_SEQ_CST);
		if (kind == R_STAT) {
			/* #922: on its count, or on its time with a few */
			if (i + 1 >= R_STATS || (i + 1 >= R_STATS_LEAST &&
			    __atomic_load_n(&r_stat_time_up,
					    __ATOMIC_SEQ_CST))) {
				__atomic_store_n(&r_over, 1, __ATOMIC_SEQ_CST);
				break;
			}
		} else if (i + 1 >= R_ROUNDS &&
			   __atomic_load_n(&r_over, __ATOMIC_SEQ_CST)) {
			break;
		}
	}
	__atomic_store_n(&r_ended[kind], 1, __ATOMIC_SEQ_CST);
	__atomic_add_fetch(&r_done, 1, __ATOMIC_SEQ_CST);
	return NULL;
}

static void
arm_threads_in_servers(void)
{
	pthread_attr_t	attr;
	pthread_t	th;
	int		i, k, n, rc, done, stuck = -1, steps = 0;
	int		seen[R_THREADS] = { 0 }, still[R_THREADS] = { 0 };

	if (access_by_stat(F_PATH) != 0) {
		not_asked(22, "no disk mounted on this boot");
		return;
	}
	(void)pthread_attr_init(&attr);
	(void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (i = 0; i < R_THREADS; i++) {
		rc = pthread_create(&th, &attr, rpc_thread,
				    (void *)(uintptr_t)i);
		if (rc != 0) {
			verdict(22, 0, "pthread_create %d said %d", i, rc);
			return;
		}
	}
	(void)pthread_attr_destroy(&attr);

	/* #922: until all four have ended, or one has completed no call for
	 * R_STALL_STEPS -- the reply it waits for is not coming */
	while (__atomic_load_n(&r_done, __ATOMIC_SEQ_CST) < R_THREADS
	       && stuck < 0) {
		pause_a_little();
		if (++steps == R_STAT_STEPS)
			__atomic_store_n(&r_stat_time_up, 1, __ATOMIC_SEQ_CST);
		for (k = 0; k < R_THREADS; k++) {
			n = __atomic_load_n(&r_round[k], __ATOMIC_SEQ_CST);
			if (__atomic_load_n(&r_ended[k], __ATOMIC_SEQ_CST)
			    || n != seen[k])
				still[k] = 0;
			else if (++still[k] >= R_STALL_STEPS && stuck < 0)
				stuck = k;
			seen[k] = n;
		}
	}
	done = __atomic_load_n(&r_done, __ATOMIC_SEQ_CST);
	__atomic_store_n(&r_over, 1, __ATOMIC_SEQ_CST); /* if stat never came back */
	if (stuck >= 0) {
		verdict(22, 0, "the %s thread completed no call in %d s, after "
			"%d: a reply that never came back -- %s %d/%d, %s %d/%d, "
			"%s %d/%d, %s %d/%d made/wrong", r_kind[stuck],
			R_STALL_STEPS / 100, r_round[stuck],
			r_kind[0], r_round[0], r_wrong[0],
			r_kind[1], r_round[1], r_wrong[1],
			r_kind[2], r_round[2], r_wrong[2],
			r_kind[3], r_round[3], r_wrong[3]);
		return;
	}
	verdict(22, done == R_THREADS
		&& r_wrong[0] + r_wrong[1] + r_wrong[2] + r_wrong[3] == 0,
		"%d of %d threads ended, at once in two servers: %s %d made "
		"and %d wrong, %s %d and %d, %s %d and %d, %s %d and %d",
		done, R_THREADS,
		r_kind[0], r_round[0], r_wrong[0],
		r_kind[1], r_round[1], r_wrong[1],
		r_kind[2], r_round[2], r_wrong[2],
		r_kind[3], r_round[3], r_wrong[3]);
}

/*
 * ── [23] The null signal ────────────────────────────────────────────────
 *
 * kill(pid, 0) sends nothing and asks whether the process is there.  A child
 * that has ended and has not been waited for still is, and once it has been
 * the pid is nobody's.
 */
static void
arm_null_signal(void)
{
	pid_t	child;
	int	self, before, after, saved, status = 0;

	self = kill(getpid(), 0);
	child = fork();
	if (child < 0) {
		verdict(23, 0, "fork failed, errno %d", errno);
		return;
	}
	if (child == 0)
		_exit(0);
	before = kill(child, 0);
	(void)waitpid(child, &status, 0);
	errno = 0;
	after = kill(child, 0);
	saved = errno;
	verdict(23, self == 0 && before == 0 && after == -1 && saved == ESRCH,
		"kill(pid, 0) answered %d for this process, %d for a child not "
		"yet waited for, and %d with errno %d once it was", self, before,
		after, saved);
}

/*
 * ── [21] Many forks ─────────────────────────────────────────────────────
 *
 * Each child has a whole personality to bring up, with two threads of its own,
 * in the time it takes to answer, so a race there is met about once in a
 * hundred forks and no single fork shows it.  Each child leaves its own number,
 * so that a status that came from somewhere else is not taken for the answer.
 */
#define FORKS		200

static void
arm_many_forks(void)
{
	int	i, wrong = 0, last = 0;

	for (i = 0; i < FORKS; i++) {
		int	status = 0;
		pid_t	pid = fork();

		if (pid < 0) {
			verdict(21, 0, "fork %d failed, errno %d", i, errno);
			return;
		}
		if (pid == 0)
			_exit(i & 0x7f);
		if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status)
		    || WEXITSTATUS(status) != (i & 0x7f)) {
			wrong++;
			last = status;
		}
	}
	verdict(21, wrong == 0, "%d children, each its number back, and %d "
		"wrong, the last with status %#x", FORKS, wrong,
		(unsigned)last);
}

static int
as_exec_child(void)
{
	void	*anywhere, *at_zero;

	anywhere = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	at_zero = mmap(NULL, PAGE, PROT_READ | PROT_WRITE,
		       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	/* 0 for "as it should be": what the status can carry is one byte. */
	if (anywhere == MAP_FAILED || anywhere == NULL)
		return 1;
	if (at_zero != MAP_FAILED)
		return 2;
	return 0;
}

static void
arm_page_zero(void)
{
	char	*argv[3];
	char	*envp[1];
	pid_t	pid, got;
	int	status = 0;

	if (!program_is_there(19, "/musl_test"))
		return;
	argv[0] = (char *)"/musl_test";
	argv[1] = (char *)"child";
	argv[2] = NULL;
	envp[0] = NULL;
	pid = fork();
	if (pid < 0) {
		not_asked(19, "fork refused: no process server on this boot");
		return;
	}
	if (pid == 0) {
		(void)execve(argv[0], argv, envp);
		_exit(127);
	}
	got = waitpid(pid, &status, 0);
	verdict(19, got == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"an exec'd program was refused page zero, by an allocation "
		"that could land anywhere and by asking for it: status %#x",
		(unsigned)status);
}

/*
 * ── [25] The two clocks, and the sleeps they measure (#923) ─────────────
 *
 * clock_gettime() reaches libposix-uros as SYS_clock_gettime, which had no
 * handler: it answered ENOSYS and left the timespec as it was.  Both clocks
 * must answer, gettimeofday() must agree with the calendar to the second, and
 * a clock nobody has must be refused with EINVAL.
 *
 * The monotonic clock is then read 100000 times by four threads at once, each
 * sleeping a millisecond every 1000 readings so that it wakes on whatever
 * processor is free.  Before each reading a thread takes the largest reading
 * any of them has published, and after it publishes its own: no reading may
 * be below what was published before it was taken.  The processors' TSCs
 * need not agree, and the kernel's clock must not show it.  One thread alone
 * could not see it: a thread that wakes goes back to the processor it last
 * ran on when that one is idle (HW_FOOTPRINT), and with the kernel's floor
 * taken out it read 0 of 100000 back.
 *
 * And 50 sleeps of 10 ms must each take at least 10 ms -- a timed wait counted
 * from the next tick could end up to a tick early, which at 100 Hz is the
 * whole of a 10 ms sleep.  A sleep is short when both clocks say so, and the
 * shortest is said by the clock that measured it longer: where the TSCs
 * disagree the monotonic clock pauses rather than go back, and it alone
 * measures short a sleep that began on a processor ahead of the rest.
 */
#define CLK_READERS	4
#define CLK_READS	25000			/* each, 100000 in all */
#define CLK_STEPS	(12 * WAIT_STEPS)	/* the readers' time, 60 s */
#define CLK_NAP_NS	(10LL * 1000 * 1000)

static long long	clk_published;	/* ns, the largest reading published */
static long long	clk_worst;	/* ns, the furthest one went back */
static int		clk_back;	/* readings below what was published */
static int		clk_refused;	/* readers refused a reading */
static int		clk_finished;	/* readers done */

static long long
ts_ns(const struct timespec *t)
{
	return (long long)t->tv_sec * 1000000000LL + t->tv_nsec;
}

/* *x becomes v if v is larger, with any number of threads at it. */
static void
clk_raise(long long *x, long long v)
{
	long long	cur = __atomic_load_n(x, __ATOMIC_SEQ_CST);

	while (v > cur && !__atomic_compare_exchange_n(x, &cur, v, 0,
						       __ATOMIC_SEQ_CST,
						       __ATOMIC_SEQ_CST))
		;
}

static void *
clock_reader(void *arg)
{
	struct timespec	ms = { 0, 1000 * 1000 }, now;
	long long	seen, mine;
	int		i;

	for (i = 0; i < CLK_READS; i++) {
		if (i % 1000 == 999)
			(void)nanosleep(&ms, NULL);
		seen = __atomic_load_n(&clk_published, __ATOMIC_SEQ_CST);
		if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
			__atomic_add_fetch(&clk_refused, 1, __ATOMIC_SEQ_CST);
			break;
		}
		mine = ts_ns(&now);
		if (mine < seen) {
			__atomic_add_fetch(&clk_back, 1, __ATOMIC_SEQ_CST);
			clk_raise(&clk_worst, seen - mine);
		}
		clk_raise(&clk_published, mine);
	}
	__atomic_add_fetch(&clk_finished, 1, __ATOMIC_SEQ_CST);
	return arg;
}

static void
arm_clocks(void)
{
	struct timespec	mono = { -1, -1 }, cal = { -1, -1 }, other;
	struct timespec	start, end, ma, mb, ca, cb;
	struct timespec	nap = { 0, 10 * 1000 * 1000 };
	struct timeval	tv = { -1, -1 };
	pthread_t	th[CLK_READERS];
	int		rc_mono, rc_cal, rc_tv, rc_other, err_other, i;
	int		rc = 0, made = 0, finished, waited;
	int		short_both = 0, short_mono = 0;
	long long	dm, dc, longer, shortest = -1, took = -1;

	rc_mono = clock_gettime(CLOCK_MONOTONIC, &mono);
	rc_cal = clock_gettime(CLOCK_REALTIME, &cal);
	rc_tv = gettimeofday(&tv, NULL);
	errno = 0;
	rc_other = clock_gettime((clockid_t)99, &other);
	err_other = errno;

	(void)clock_gettime(CLOCK_MONOTONIC, &start);
	for (i = 0; i < CLK_READERS && rc_mono == 0 && rc == 0; i++) {
		rc = pthread_create(&th[i], NULL, clock_reader, NULL);
		if (rc == 0)
			made++;
	}
	for (waited = 0; waited < CLK_STEPS &&
	     __atomic_load_n(&clk_finished, __ATOMIC_SEQ_CST) < made; waited++)
		pause_a_little();
	finished = __atomic_load_n(&clk_finished, __ATOMIC_SEQ_CST);
	if (finished == made) {
		for (i = 0; i < made; i++)
			(void)pthread_join(th[i], NULL);
		if (clock_gettime(CLOCK_MONOTONIC, &end) == 0 && rc_mono == 0)
			took = (ts_ns(&end) - ts_ns(&start)) / 1000000;
	}

	for (i = 0; i < 50 && rc_mono == 0 && rc_cal == 0; i++) {
		(void)clock_gettime(CLOCK_MONOTONIC, &ma);
		(void)clock_gettime(CLOCK_REALTIME, &ca);
		(void)nanosleep(&nap, NULL);
		(void)clock_gettime(CLOCK_MONOTONIC, &mb);
		(void)clock_gettime(CLOCK_REALTIME, &cb);
		dm = ts_ns(&mb) - ts_ns(&ma);
		dc = ts_ns(&cb) - ts_ns(&ca);
		longer = dm > dc ? dm : dc;
		if (shortest < 0 || longer < shortest)
			shortest = longer;
		if (dm < CLK_NAP_NS)
			short_mono++;
		if (longer < CLK_NAP_NS)
			short_both++;
	}

	if (rc != 0)
		verdict(25, 0, "pthread_create %d said %d", made, rc);
	else if (finished < made)
		verdict(25, 0, "of %d threads reading the monotonic clock, %d "
			"had finished after %d sleeps of 10 ms asked", made,
			finished, waited);
	else
		verdict(25, rc_mono == 0 && rc_cal == 0 && rc_tv == 0 &&
			rc_other == -1 && err_other == EINVAL &&
			clk_refused == 0 && clk_back == 0 && short_both == 0 &&
			tv.tv_sec - cal.tv_sec >= 0 &&
			tv.tv_sec - cal.tv_sec <= 1,
			"monotonic %lld.%09ld, calendar %lld, day %lld; clock "
			"99: %d errno %d; %d of 100000 readings by %d threads "
			"went back, by %lld ns at most, %d threads refused, in "
			"%lld ms; %d of 50 10 ms sleeps short by both clocks, "
			"%d by the monotonic one, shortest %lld ns",
			(long long)mono.tv_sec, (long)mono.tv_nsec,
			(long long)cal.tv_sec, (long long)tv.tv_sec, rc_other,
			err_other, clk_back, made, clk_worst, clk_refused,
			took, short_both, short_mono, shortest);
}

/*
 * ── [26] The environment a program is started with (#916) ───────────────
 *
 * crt0 has a static program's environment, from the bootstrap server or from
 * the stack exec_server built, and musl's __environ was never set from it:
 * getenv() answered NULL for every name, environ read NULL, and execv()
 * passed an empty environment on.  Nor was the program's name, so err() and
 * warn() said "(null)".  This program starts itself again with two
 * variables.  The child reads one back, counts what environ holds and reads
 * its own short name, prints what it read, and answers in its status which
 * of them were wrong: libposix-uros has no pipe() to send the reading back
 * on (#904).
 */
#define ENV_FIRST	"MUSL_TEST_FIRST=1"
#define ENV_NAME	"MUSL_TEST_ENV"
#define ENV_VALUE	"set by arm 26 (#916)"

/* The child's status, a bit for each thing it found wrong. */
#define ENV_NONE	1	/* getenv() found no ENV_NAME */
#define ENV_OTHER	2	/* it found another value */
#define ENV_COUNT	4	/* environ held other than the two */
#define ENV_NAMED	8	/* its short name was not musl_test */

static int
as_env_child(void)
{
	const char	*v = getenv(ENV_NAME);
	char		**e;
	int		n = 0, wrong = 0;

	for (e = environ; e != NULL && *e != NULL; e++)
		n++;
	if (v == NULL)
		wrong |= ENV_NONE;
	else if (strcmp(v, ENV_VALUE) != 0)
		wrong |= ENV_OTHER;
	if (n != 2)
		wrong |= ENV_COUNT;
	if (program_invocation_short_name == NULL ||
	    strcmp(program_invocation_short_name, "musl_test") != 0)
		wrong |= ENV_NAMED;
	/* musl prints a null string as "(null)", which is what it was */
	printf("musl_test (env child): getenv(\"%s\") gave %s%s%s, environ "
	       "held %d, and the program's short name is %s\n", ENV_NAME,
	       v != NULL ? "\"" : "", v != NULL ? v : "nothing",
	       v != NULL ? "\"" : "", n, program_invocation_short_name);
	return wrong;
}

static void
arm_environment(void)
{
	char	*argv[3];
	char	*envp[3];
	pid_t	pid, got;
	int	status = 0, wrong;

	if (!program_is_there(26, "/musl_test"))
		return;
	argv[0] = (char *)"/musl_test";
	argv[1] = (char *)"env";
	argv[2] = NULL;
	envp[0] = (char *)ENV_FIRST;
	envp[1] = (char *)ENV_NAME "=" ENV_VALUE;
	envp[2] = NULL;
	pid = fork();
	if (pid < 0) {
		not_asked(26, "fork refused: no process server on this boot");
		return;
	}
	if (pid == 0) {
		(void)execve(argv[0], argv, envp);
		_exit(127);
	}
	got = waitpid(pid, &status, 0);
	wrong = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (got != pid || wrong < 0 || wrong > 15)
		verdict(26, 0, "a child exec'd with two variables ended with "
			"status %#x", (unsigned)status);
	else
		verdict(26, wrong == 0, "a child exec'd with %s and %s=\"%s\" "
			"answered %d: getenv() found %s, environ held %s, and "
			"its short name was %s", ENV_FIRST, ENV_NAME, ENV_VALUE,
			wrong, (wrong & ENV_NONE) ? "nothing" :
			(wrong & ENV_OTHER) ? "another value" : "it",
			(wrong & ENV_COUNT) ? "other than two" : "two",
			(wrong & ENV_NAMED) ? "not musl_test" : "musl_test");
}

/*
 * ── [30] dup(), dup2() and dup3(): two names for one description (#897) ─
 *
 * A descriptor names an open file description, and the description holds the
 * offset.  dup() gives it a second number, so a read through one moves the
 * other's offset too, and the file stays open until the last name closes.
 * The three answered ENOSYS, and the table under them kept a description per
 * descriptor.  The file is one on the root disk that nobody writes; the arm
 * only reads it.
 */
#define DUP_READ	16

static void
arm_dup(void)
{
	char	buf[DUP_READ];
	off_t	shared, by_two, after_close;
	int	fd, d, two, same, rc_bad, err_bad, rc_neg, err_neg;
	int	rc_self, err_self, rc_flag, err_flag, rc_gone, err_gone;

	if (!program_is_there(30, F_PATH))
		return;
	fd = open(F_PATH, O_RDONLY);
	if (fd < 0) {
		verdict(30, 0, "open failed, errno %d", errno);
		return;
	}
	d = dup(fd);
	if (d < 0) {
		verdict(30, 0, "dup(%d) failed, errno %d", fd, errno);
		(void)close(fd);
		return;
	}
	if (read(fd, buf, sizeof buf) != (ssize_t)sizeof buf) {
		verdict(30, 0, "reading %d bytes failed, errno %d", DUP_READ,
			errno);
		(void)close(d);
		(void)close(fd);
		return;
	}
	shared = lseek(d, 0, SEEK_CUR);
	two = dup2(fd, 20);
	by_two = lseek(20, 0, SEEK_CUR);
	same = dup2(fd, fd);
	rc_neg = dup2(fd, -1);
	err_neg = errno;
	rc_self = dup3(fd, fd, 0);
	err_self = errno;
	rc_flag = dup3(fd, 21, 0x1);	/* not a flag dup3() takes */
	err_flag = errno;
	/* The first name closed, the others still reach the file. */
	(void)close(fd);
	after_close = lseek(d, 0, SEEK_CUR);
	(void)close(d);
	rc_bad = dup2(d, 21);		/* closed, so nothing to copy */
	err_bad = errno;
	(void)close(20);
	rc_gone = (int)lseek(20, 0, SEEK_CUR);
	err_gone = errno;
	verdict(30, d >= 0 && d != fd && shared == DUP_READ && two == 20
		&& by_two == DUP_READ && same == fd && rc_neg == -1
		&& err_neg == EBADF && rc_self == -1 && err_self == EINVAL
		&& rc_flag == -1 && err_flag == EINVAL
		&& after_close == DUP_READ && rc_bad == -1 && err_bad == EBADF
		&& rc_gone == -1 && err_gone == EBADF,
		"after %d bytes read on %d, its dup %d was at %ld and dup2's "
		"20 at %ld; dup2 onto itself gave %d, onto -1 %d errno %d; "
		"dup3 onto itself %d errno %d, with a bad flag %d errno %d; "
		"with %d closed, %d was at %ld; dup2 of a closed one %d errno "
		"%d, and 20 closed read %d errno %d", DUP_READ, fd, d,
		(long)shared, (long)by_two, same, rc_neg, err_neg, rc_self,
		err_self, rc_flag, err_flag, fd, d, (long)after_close, rc_bad,
		err_bad, rc_gone, err_gone);
}

/*
 * ── [31] A dup() is still one description after execve() (#897) ────────
 *
 * The descriptors that cross an exec go to the new image as records, and the
 * new image opens each record again.  Two names of one description came out
 * two opens of the file, each with an offset of its own, each writing over
 * what the other wrote.  And a name dup3() gave with O_CLOEXEC must not
 * cross at all.  This program starts itself again holding 20 and 21, one
 * description, and 22, made with O_CLOEXEC; the child reads through 20, looks
 * at 21's offset and at 22, prints what it found, and answers in its status.
 *
 * The parent keeps its own 20, 21 and 22 open until the child is done.  Its
 * copies and the child's share one open file in the file server, and a
 * close() in either closes it for both, so that the child's read failed:
 * #932.  This arm asks about the exec, and that is a question of its own.
 */
#define DUP_LOST	1	/* 20 or 21 did not cross, or at another offset */
#define DUP_APART	2	/* a read on 20 left 21's offset where it was */
#define DUP_CLOEXEC	4	/* 22, made with O_CLOEXEC, crossed */
#define DUP_UNREAD	8	/* the read on 20 itself failed */

static int
as_dup_child(void)
{
	char	buf[DUP_READ];
	off_t	at20, at21, moved = -1, at22;
	ssize_t	got = -2;		/* -2: not asked */
	int	wrong = 0, err_read = 0, err22;

	at20 = lseek(20, 0, SEEK_CUR);
	at21 = lseek(21, 0, SEEK_CUR);
	if (at20 != DUP_READ || at21 != DUP_READ) {
		wrong |= DUP_LOST;
	} else {
		got = read(20, buf, sizeof buf);
		err_read = errno;
		if (got != (ssize_t)sizeof buf)
			wrong |= DUP_UNREAD;
		else if ((moved = lseek(21, 0, SEEK_CUR)) != 2 * DUP_READ)
			wrong |= DUP_APART;
	}
	at22 = lseek(22, 0, SEEK_CUR);
	err22 = errno;
	if (at22 != -1 || err22 != EBADF)
		wrong |= DUP_CLOEXEC;
	printf("musl_test (dup child): 20 at %ld and 21 at %ld; a read of %d "
	       "bytes on 20 gave %ld errno %d, and 21 was then at %ld; 22 "
	       "answered %ld errno %d\n", (long)at20, (long)at21, DUP_READ,
	       (long)got, got < 0 ? err_read : 0, (long)moved, (long)at22,
	       err22);
	return wrong;
}

static void
arm_dup_across_exec(void)
{
	char	buf[DUP_READ];
	char	*argv[3];
	char	*envp[1];
	pid_t	pid, got;
	int	fd, status = 0, wrong, rc20, rc21, rc22;

	if (!program_is_there(31, "/musl_test"))
		return;
	fd = open(F_PATH, O_RDONLY);
	if (fd < 0) {
		verdict(31, 0, "open failed, errno %d", errno);
		return;
	}
	if (read(fd, buf, sizeof buf) != (ssize_t)sizeof buf) {
		verdict(31, 0, "reading %d bytes failed, errno %d", DUP_READ,
			errno);
		(void)close(fd);
		return;
	}
	rc20 = dup2(fd, 20);
	rc21 = dup2(fd, 21);
	rc22 = dup3(fd, 22, O_CLOEXEC);
	(void)close(fd);
	if (rc20 != 20 || rc21 != 21 || rc22 != 22) {
		verdict(31, 0, "dup2 onto 20 and 21 gave %d and %d, dup3 onto "
			"22 %d, errno %d", rc20, rc21, rc22, errno);
		(void)close(20);
		(void)close(21);
		(void)close(22);
		return;
	}
	argv[0] = (char *)"/musl_test";
	argv[1] = (char *)"dup";
	argv[2] = NULL;
	envp[0] = NULL;
	pid = fork();
	if (pid < 0) {
		not_asked(31, "fork refused: no process server on this boot");
		(void)close(20);
		(void)close(21);
		(void)close(22);
		return;
	}
	if (pid == 0) {
		(void)execve(argv[0], argv, envp);
		_exit(127);
	}
	got = waitpid(pid, &status, 0);
	/* Only now: a close before the child's read closed the file for the
	 * child too (#932, and the comment above). */
	(void)close(20);
	(void)close(21);
	(void)close(22);
	wrong = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	if (got != pid || wrong < 0 || wrong > 15)
		verdict(31, 0, "a child exec'd holding 20, 21 and 22 ended "
			"with status %#x", (unsigned)status);
	else
		verdict(31, wrong == 0, "a child exec'd holding 20 and 21, one "
			"description at %d, and 22 made with O_CLOEXEC answered "
			"%d: 20 and 21 %s, %s, and 22 %s", DUP_READ, wrong,
			(wrong & DUP_LOST) ? "did not both cross at that offset"
			: "crossed at that offset",
			(wrong & DUP_LOST) ? "the read was not asked"
			: (wrong & DUP_UNREAD) ? "the read on 20 failed"
			: (wrong & DUP_APART) ? "a read on 20 left 21 where it was"
			: "a read on 20 moved 21",
			(wrong & DUP_CLOEXEC) ? "crossed" : "did not cross");
}

/*
 * ── [32] A pipe in one process (#904) ─────────────────────────────────────
 *
 * pipe() answered ENOSYS.  Bytes written come out in order and once, a read
 * takes part of a write and the next read the rest, writev()'s pieces come
 * out as one run, a dup() of the write end writes into the same pipe, and the
 * ends say what they are: a FIFO to fstat(), not seekable, and each refusing
 * the other's call.  With O_NONBLOCK an empty pipe answers EAGAIN to a read,
 * and the pipe takes 64 KiB, as Linux's does, and then EAGAIN to a write.
 */
#define PIPE_HOLDS	65536

static void
arm_pipe(void)
{
	char		buf[PIPE_BUF];
	struct iovec	v[2];
	struct stat	st;
	int		fds[2], nb[2], d;
	ssize_t		w1, r1, r2, wv, rv, wd, rd, r_wrong, w_wrong, r_empty;
	ssize_t		w_full, r_drain, w_again;
	int		e_seek, e_rwrong, e_wwrong, e_empty, e_full, fifo;
	long		filled = 0;
	int		bytes_ok;

	if (pipe(fds) != 0) {
		verdict(32, 0, "pipe failed, errno %d", errno);
		return;
	}
	w1 = write(fds[1], "abcdefghij", 10);
	r1 = read(fds[0], buf, 4);
	r2 = read(fds[0], buf + 4, sizeof buf - 4);
	bytes_ok = r1 == 4 && r2 == 6 && memcmp(buf, "abcdefghij", 10) == 0;
	v[0].iov_base = (void *)"12";
	v[0].iov_len = 2;
	v[1].iov_base = (void *)"345";
	v[1].iov_len = 3;
	wv = writev(fds[1], v, 2);
	rv = read(fds[0], buf, sizeof buf);
	bytes_ok = bytes_ok && rv == 5 && memcmp(buf, "12345", 5) == 0;
	d = dup(fds[1]);
	wd = d >= 0 ? write(d, "x", 1) : -1;
	if (d >= 0)
		(void)close(d);
	rd = read(fds[0], buf, sizeof buf);
	bytes_ok = bytes_ok && rd == 1 && buf[0] == 'x';
	fifo = fstat(fds[0], &st) == 0 && S_ISFIFO(st.st_mode);
	e_seek = lseek(fds[0], 0, SEEK_CUR) == -1 ? errno : 0;
	r_wrong = read(fds[1], buf, 1);
	e_rwrong = errno;
	w_wrong = write(fds[0], "y", 1);
	e_wwrong = errno;
	(void)close(fds[0]);
	(void)close(fds[1]);

	if (pipe2(nb, O_NONBLOCK) != 0) {
		verdict(32, 0, "pipe2(O_NONBLOCK) failed, errno %d", errno);
		return;
	}
	r_empty = read(nb[0], buf, sizeof buf);
	e_empty = errno;
	memset(buf, 'z', sizeof buf);
	for (;;) {
		ssize_t	w = write(nb[1], buf, sizeof buf);

		if (w <= 0 || filled > 2 * PIPE_HOLDS)
			break;
		filled += w;
	}
	w_full = write(nb[1], buf, 1);
	e_full = errno;
	r_drain = read(nb[0], buf, sizeof buf);
	w_again = write(nb[1], buf, sizeof buf);
	(void)close(nb[0]);
	(void)close(nb[1]);

	verdict(32, w1 == 10 && bytes_ok && wv == 5 && wd == 1 && fifo
		&& e_seek == ESPIPE && r_wrong == -1 && e_rwrong == EBADF
		&& w_wrong == -1 && e_wwrong == EBADF && r_empty == -1
		&& e_empty == EAGAIN && filled == PIPE_HOLDS && w_full == -1
		&& e_full == EAGAIN && r_drain == PIPE_BUF
		&& w_again == PIPE_BUF,
		"10 bytes read as %zd and %zd, writev's 5 as %zd, a dup's byte "
		"as %zd, %s; %s, lseek errno %d, read on the write end %zd "
		"errno %d, write on the read end %zd errno %d; O_NONBLOCK: "
		"empty read %zd errno %d, took %ld bytes, then %zd errno %d, "
		"and after a read of %zd a write of %zd", r1, r2, rv, rd,
		bytes_ok ? "in order" : "NOT in order",
		fifo ? "a FIFO" : "NOT a FIFO", e_seek, r_wrong, e_rwrong,
		w_wrong, e_wwrong, r_empty, e_empty, filled, w_full, e_full,
		r_drain, w_again);
}

/*
 * Children that may block, watched: a child waiting on a pipe that nothing
 * will write or end -- a pipe_server missing, or the end of file broken -- is
 * a boot with no verdict.  Each is given `secs'; one still there then is
 * killed.  Returns
 * how many were killed, each one's status in status[], and in *others how
 * many children not in pid[] the wait collected.
 *
 * The wait is waitpid(-1, WNOHANG), the one non-blocking wait libposix-uros
 * has: given a pid, WNOHANG is not honoured and the call blocks
 * (posix_wait.c), which held this watch on a child that never ended.  Every
 * arm above waits for its own children, so by here what it collects is one
 * of these, and *others says so if not.
 */
static int
watch_children(pid_t *pid, int n, int secs, int *status, int *others)
{
	int	left = n, killed = 0, i, step, st, ours;
	pid_t	got;

	*others = 0;
	for (i = 0; i < n; i++)
		status[i] = -1;
	for (step = 0; left > 0 && step < secs * 100; step++) {
		while (left > 0 && (got = waitpid(-1, &st, WNOHANG)) > 0) {
			ours = 0;
			for (i = 0; i < n; i++)
				if (pid[i] == got && status[i] == -1) {
					status[i] = st;
					left--;
					ours = 1;
				}
			if (!ours)
				(*others)++;
		}
		if (left > 0)
			pause_a_little();
	}
	for (i = 0; i < n; i++) {
		if (status[i] != -1)
			continue;
		(void)kill(pid[i], SIGKILL);
		(void)waitpid(pid[i], &status[i], 0);
		killed++;
	}
	return killed;
}

/*
 * ── [33] Two processes write one pipe at once (#904) ──────────────────────
 *
 * A write of up to PIPE_BUF bytes is whole however many write.  Two children
 * each write 64 records of PIPE_BUF bytes, each record filled with its
 * writer's mark and number, and a third reads all 128 and looks at every
 * record: one writer's bytes only, and each writer's numbers in order.  The
 * 512 KiB go round the 64 KiB ring eight times, so records are split where it
 * wraps, and the three block on a full pipe and on an empty one.
 */
#define PW_RECORDS	64

/* What the reading child answers in its status. */
#define PW_TORN		1	/* a record held two writers' bytes */
#define PW_ORDER	2	/* a writer's records came out of order */
#define PW_READ		4	/* a read failed */

static void
pipe_record(unsigned char *rec, int writer, int n)
{
	memset(rec, (writer * 64 + n) & 0xff, PIPE_BUF);
	rec[0] = (unsigned char)writer;
	rec[1] = (unsigned char)n;
}

static int
pipe_writer(int fd, int writer)
{
	unsigned char	rec[PIPE_BUF];
	int		n;

	for (n = 0; n < PW_RECORDS; n++) {
		pipe_record(rec, writer, n);
		if (write(fd, rec, sizeof rec) != (ssize_t)sizeof rec)
			return 1;
	}
	return 0;
}

static int
pipe_reader(int fd)
{
	static unsigned char	rec[PIPE_BUF];
	unsigned char		want[PIPE_BUF];
	int			next[3] = { 0, 0, 0 };
	int			wrong = 0, got, k;
	size_t			have;

	for (k = 0; k < 2 * PW_RECORDS; k++) {
		for (have = 0; have < sizeof rec; have += (size_t)got) {
			got = (int)read(fd, rec + have, sizeof rec - have);
			if (got <= 0)
				return wrong | PW_READ;
		}
		if (rec[0] != 1 && rec[0] != 2) {
			wrong |= PW_TORN;
			continue;
		}
		pipe_record(want, rec[0], rec[1]);
		if (memcmp(rec, want, sizeof want) != 0)
			wrong |= PW_TORN;
		if (rec[1] != next[rec[0]])
			wrong |= PW_ORDER;
		next[rec[0]] = rec[1] + 1;
	}
	if (next[1] != PW_RECORDS || next[2] != PW_RECORDS)
		wrong |= PW_ORDER;
	return wrong;
}

static void
arm_pipe_writers(void)
{
	pid_t	pid[3];
	int	status[3], fds[2], i, killed, others, rstat, w1, w2;

	if (pipe(fds) != 0) {
		verdict(33, 0, "pipe failed, errno %d", errno);
		return;
	}
	for (i = 0; i < 3; i++) {
		pid[i] = fork();
		if (pid[i] < 0) {
			while (--i >= 0) {
				(void)kill(pid[i], SIGKILL);
				(void)waitpid(pid[i], NULL, 0);
			}
			(void)close(fds[0]);
			(void)close(fds[1]);
			not_asked(33, "fork refused: no process server on "
				  "this boot");
			return;
		}
		if (pid[i] == 0)
			_exit(i == 0 ? pipe_reader(fds[0])
				     : pipe_writer(fds[1], i));
	}
	(void)close(fds[0]);
	(void)close(fds[1]);
	killed = watch_children(pid, 3, 60, status, &others);
	rstat = WIFEXITED(status[0]) ? WEXITSTATUS(status[0]) : -1;
	w1 = WIFEXITED(status[1]) ? WEXITSTATUS(status[1]) : -1;
	w2 = WIFEXITED(status[2]) ? WEXITSTATUS(status[2]) : -1;
	verdict(33, killed == 0 && others == 0 && rstat == 0 && w1 == 0 &&
		w2 == 0,
		"two writers of %d records of %d bytes each ended %d and %d, "
		"and the reader of all %d answered %d: %s, %s%s; %d of 3 still "
		"there after 60 s, %d other children collected", PW_RECORDS,
		PIPE_BUF, w1, w2,
		2 * PW_RECORDS, rstat,
		rstat < 0 ? "no answer" : (rstat & PW_TORN) ? "a record TORN"
		: "every record whole",
		rstat < 0 ? "no answer" : (rstat & PW_ORDER)
		? "OUT of order" : "each writer's in order",
		rstat > 0 && (rstat & PW_READ) ? ", a read FAILED" : "",
		killed, others);
}

/*
 * ── [42] End of file (#904) ──────────────────────────────────────────────
 *
 * A read on a pipe whose writers have all gone answers what is left in it,
 * then 0.  In one process, with O_NONBLOCK, so that a pipe with no end of
 * file answers EAGAIN instead of holding the arm: five bytes written, the
 * write end closed, and reads of 2, 3, and then 0 bytes.  close() tells
 * pipe_server before it returns (pipe_leave), so the third read cannot find
 * the pipe as it was.  And across fork: one child writes EOF_BYTES in three
 * writes and exits, which lets its write end go; another reads until 0.  The
 * parent holds no end, so the writer's going is the end of file.
 */
#define EOF_BYTES	300

static int
eof_reader(int fd)
{
	char	buf[128];
	long	have = 0;
	ssize_t	got;

	while ((got = read(fd, buf, sizeof buf)) > 0)
		have += got;
	if (got < 0)
		return 1;
	return have == EOF_BYTES ? 0 : 2;
}

static int
eof_writer(int fd)
{
	char	buf[EOF_BYTES / 3];
	int	i;

	memset(buf, 'e', sizeof buf);
	for (i = 0; i < 3; i++)
		if (write(fd, buf, sizeof buf) != (ssize_t)sizeof buf)
			return 1;
	return 0;
}

static void
arm_pipe_eof(void)
{
	char	buf[8];
	pid_t	pid[2];
	int	fds[2], status[2], killed, others, i, rstat, wstat;
	int	r1, r2, r3, e3;

	if (pipe2(fds, O_NONBLOCK) != 0) {
		verdict(42, 0, "pipe2 failed, errno %d", errno);
		return;
	}
	(void)write(fds[1], "hello", 5);
	(void)close(fds[1]);
	r1 = (int)read(fds[0], buf, 2);
	r2 = (int)read(fds[0], buf, 3);
	errno = 0;
	r3 = (int)read(fds[0], buf, sizeof buf);
	e3 = errno;
	(void)close(fds[0]);

	if (pipe(fds) != 0) {
		verdict(42, 0, "pipe failed, errno %d", errno);
		return;
	}
	for (i = 0; i < 2; i++) {
		pid[i] = fork();
		if (pid[i] < 0) {
			while (--i >= 0) {
				(void)kill(pid[i], SIGKILL);
				(void)waitpid(pid[i], NULL, 0);
			}
			(void)close(fds[0]);
			(void)close(fds[1]);
			not_asked(42, "fork refused: no process server on "
				  "this boot");
			return;
		}
		if (pid[i] == 0) {
			if (i == 0) {
				(void)close(fds[1]);
				_exit(eof_reader(fds[0]));
			}
			(void)close(fds[0]);
			_exit(eof_writer(fds[1]));
		}
	}
	(void)close(fds[0]);
	(void)close(fds[1]);
	killed = watch_children(pid, 2, 30, status, &others);
	rstat = WIFEXITED(status[0]) ? WEXITSTATUS(status[0]) : -1;
	wstat = WIFEXITED(status[1]) ? WEXITSTATUS(status[1]) : -1;
	verdict(42, r1 == 2 && r2 == 3 && r3 == 0 && killed == 0 &&
		others == 0 && rstat == 0 && wstat == 0,
		"in one process, 5 bytes and the write end closed: reads of "
		"%d, %d and %d (errno %d), %s; across fork the writer of %d "
		"bytes ended %d and the reader to end of file %d (%s); %d of 2 "
		"still there after 30 s, %d other children collected",
		r1, r2, r3, e3, r3 == 0 ? "end of file" : "NO end of file",
		EOF_BYTES, wstat, rstat,
		rstat == 0 ? "all of them, then 0" : rstat == 2 ? "a WRONG count"
		: rstat == 1 ? "a read FAILED" : "no answer", killed, others);
}

/*
 * ── [43] EPIPE and SIGPIPE (#904) ────────────────────────────────────────
 *
 * A write on a pipe whose readers have all gone answers EPIPE and raises
 * SIGPIPE.  The parent closes the read end before it forks, so the readers
 * are gone before either child writes.  One child ignores SIGPIPE and must
 * get -1 with EPIPE; the other leaves SIGPIPE as it is and must end by it.
 * The signal goes through proc_server and may come after the write returns,
 * so that child waits for it a while, and exits with 3 if it never comes.
 * Both in children: a SIGPIPE ignored here could come after the arm put the
 * old disposition back, and end musl_test.
 */
static void
arm_pipe_epipe(void)
{
	pid_t	pid[2];
	int	fds[2], status[2], killed, others, i, quiet, loud;

	if (pipe(fds) != 0) {
		verdict(43, 0, "pipe failed, errno %d", errno);
		return;
	}
	(void)close(fds[0]);
	for (i = 0; i < 2; i++) {
		pid[i] = fork();
		if (pid[i] < 0) {
			while (--i >= 0) {
				(void)kill(pid[i], SIGKILL);
				(void)waitpid(pid[i], NULL, 0);
			}
			(void)close(fds[1]);
			not_asked(43, "fork refused: no process server on "
				  "this boot");
			return;
		}
		if (pid[i] == 0) {
			ssize_t	w;

			(void)signal(SIGPIPE, i == 0 ? SIG_IGN : SIG_DFL);
			errno = 0;
			w = write(fds[1], "x", 1);
			if (i == 0)
				_exit(w == -1 && errno == EPIPE ? 0
				      : w == -1 ? 2 : 1);
			sleep(5);
			_exit(3);
		}
	}
	(void)close(fds[1]);
	killed = watch_children(pid, 2, 30, status, &others);
	quiet = WIFEXITED(status[0]) ? WEXITSTATUS(status[0]) : -1;
	loud = WIFSIGNALED(status[1]) ? WTERMSIG(status[1]) : -1;
	verdict(43, killed == 0 && others == 0 && quiet == 0 &&
		loud == SIGPIPE,
		"with no reader left, the child that ignores SIGPIPE got %s, "
		"and the other ended %s; %d of 2 still there after 30 s, %d "
		"other children collected",
		quiet == 0 ? "-1 with EPIPE" : quiet == 1 ? "its byte WRITTEN"
		: quiet == 2 ? "another errno" : "no answer",
		loud == SIGPIPE ? "by SIGPIPE"
		: WIFEXITED(status[1]) && WEXITSTATUS(status[1]) == 3
		? "with NO SIGPIPE" : "some other way", killed, others);
}

/*
 * ── [44] A holder killed with an end open (#904) ─────────────────────────
 *
 * Memory cannot tell that a task died; the death of its tokens can.  A child
 * that holds the write end and sleeps is killed, and a reader in another
 * child must then see end of file.  And a writer killed while it holds the
 * writers' lock: the pipe is full when it writes one byte, so it sleeps
 * inside write() with the lock held, and killed there it can never let the
 * lock go.  pipe_server frees it once the dead writer has no token on the
 * pipe left; then a second writer's ten bytes come out, after a reader has
 * drained the ring.  Without the free, the second writer waits for the lock
 * for good, and the reader for its bytes: both are watched.
 */
#define HK_FILL		65536	/* the ring, full */

/* What the draining reader answers: 0, its ten bytes after the fill. */
static int
hk_drain(int fd)
{
	static char	chunk[4096];
	char		got[10];
	ssize_t		n;
	long		drained = 0;
	size_t		have = 0;

	while (drained < HK_FILL) {
		n = read(fd, chunk, sizeof chunk);
		if (n <= 0)
			return 1;
		drained += n;
	}
	while (have < sizeof got) {
		n = read(fd, got + have, sizeof got - have);
		if (n <= 0)
			return 2;
		have += (size_t)n;
	}
	return memcmp(got, "0123456789", sizeof got) == 0 ? 0 : 3;
}

static void
arm_pipe_holder_killed(void)
{
	static char	chunk[4096];
	char		got[16];
	pid_t		pid[2], w1, two[2];
	int		fds[2], status[2], killed, others, i, rstat;
	int		n, dstat = -1, w2stat = -1;

	/* A writer that sleeps, killed: the reader's end of file. */
	if (pipe(fds) != 0) {
		verdict(44, 0, "pipe failed, errno %d", errno);
		return;
	}
	for (i = 0; i < 2; i++) {
		pid[i] = fork();
		if (pid[i] < 0) {
			while (--i >= 0) {
				(void)kill(pid[i], SIGKILL);
				(void)waitpid(pid[i], NULL, 0);
			}
			(void)close(fds[0]);
			(void)close(fds[1]);
			not_asked(44, "fork refused: no process server on "
				  "this boot");
			return;
		}
		if (pid[i] == 0) {
			if (i == 0) {
				(void)close(fds[1]);
				_exit(read(fds[0], got, sizeof got) == 0 ? 0
				      : 1);
			}
			(void)close(fds[0]);
			sleep(60);
			_exit(0);
		}
	}
	(void)close(fds[0]);
	(void)close(fds[1]);
	usleep(300 * 1000);
	(void)kill(pid[1], SIGKILL);
	killed = watch_children(pid, 2, 30, status, &others);
	rstat = WIFEXITED(status[0]) ? WEXITSTATUS(status[0]) : -1;

	/* A writer killed holding the writers' lock. */
	if (pipe(fds) != 0) {
		verdict(44, 0, "pipe failed, errno %d", errno);
		return;
	}
	memset(chunk, 'f', sizeof chunk);
	for (n = 0; n < HK_FILL; n += (int)sizeof chunk)
		if (write(fds[1], chunk, sizeof chunk) != (ssize_t)sizeof chunk)
			break;
	w1 = fork();
	if (w1 == 0) {
		(void)write(fds[1], "!", 1);	/* full: sleeps with the lock */
		_exit(0);
	}
	two[0] = two[1] = -1;
	if (w1 > 0) {
		usleep(500 * 1000);
		(void)kill(w1, SIGKILL);
		(void)waitpid(w1, NULL, 0);
		for (i = 0; i < 2; i++) {
			two[i] = fork();
			if (two[i] == 0) {
				if (i == 0)
					_exit(write(fds[1], "0123456789", 10)
					      == 10 ? 0 : 1);
				_exit(hk_drain(fds[0]));
			}
		}
	}
	(void)close(fds[0]);
	(void)close(fds[1]);
	if (two[0] > 0 && two[1] > 0) {
		int	st[2] = { 0, 0 }, oth = 0;

		killed += watch_children(two, 2, 30, st, &oth);
		others += oth;
		w2stat = WIFEXITED(st[0]) ? WEXITSTATUS(st[0]) : -1;
		dstat = WIFEXITED(st[1]) ? WEXITSTATUS(st[1]) : -1;
	} else {
		for (i = 0; i < 2; i++)
			if (two[i] > 0) {
				(void)kill(two[i], SIGKILL);
				(void)waitpid(two[i], NULL, 0);
			}
	}
	verdict(44, killed == 0 && others == 0 && rstat == 0 && w1 > 0 &&
		w2stat == 0 && dstat == 0,
		"the reader of a pipe whose writer was killed asleep ended %d "
		"(%s); after a writer was killed holding the writers' lock, a "
		"second writer of 10 bytes ended %d and the reader of the full "
		"ring and those 10 ended %d (%s); %d still there after their "
		"time, %d other children collected",
		rstat, rstat == 0 ? "end of file" : "NO end of file", w2stat,
		dstat, dstat == 0 ? "the lock was freed"
		: dstat == 3 ? "WRONG bytes" : "the lock was NOT freed",
		killed, others);
}

/*
 * ── [34] A pipe from one process to another, in a stream (#904) ──────────
 *
 * One child writes 256 KiB in writes of 1000 bytes, which fit the ring's
 * edges nowhere in particular, and another reads them in reads of 777 and
 * checks every byte: four times the ring, so each side waits for the other
 * again and again, and a read takes the rest of one write and the start of
 * the next.
 */
#define PS_BYTES	(256 * 1024)

static unsigned char
pipe_stream_byte(long i)
{
	return (unsigned char)((i * 7) ^ (i >> 9));
}

static int
pipe_stream_writer(int fd)
{
	unsigned char	chunk[1000];
	long		i = 0;
	int		k;

	while (i < PS_BYTES) {
		int	n = PS_BYTES - i < (long)sizeof chunk
			    ? (int)(PS_BYTES - i) : (int)sizeof chunk;

		for (k = 0; k < n; k++)
			chunk[k] = pipe_stream_byte(i + k);
		if (write(fd, chunk, (size_t)n) != n)
			return 1;
		i += n;
	}
	return 0;
}

static int
pipe_stream_reader(int fd)
{
	unsigned char	chunk[777];
	long		i = 0;
	int		k;

	while (i < PS_BYTES) {
		ssize_t	got = read(fd, chunk, sizeof chunk);

		if (got <= 0)
			return 2;
		for (k = 0; k < got; k++)
			if (chunk[k] != pipe_stream_byte(i + k))
				return 1;
		i += got;
	}
	return 0;
}

static void
arm_pipe_stream(void)
{
	pid_t	pid[2];
	int	status[2], fds[2], killed, others, rs, ws;

	if (pipe(fds) != 0) {
		verdict(34, 0, "pipe failed, errno %d", errno);
		return;
	}
	pid[0] = fork();
	if (pid[0] == 0)
		_exit(pipe_stream_reader(fds[0]));
	pid[1] = pid[0] < 0 ? -1 : fork();
	if (pid[1] == 0)
		_exit(pipe_stream_writer(fds[1]));
	(void)close(fds[0]);
	(void)close(fds[1]);
	if (pid[0] < 0 || pid[1] < 0) {
		if (pid[0] > 0) {
			(void)kill(pid[0], SIGKILL);
			(void)waitpid(pid[0], NULL, 0);
		}
		not_asked(34, "fork refused: no process server on this boot");
		return;
	}
	killed = watch_children(pid, 2, 60, status, &others);
	rs = WIFEXITED(status[0]) ? WEXITSTATUS(status[0]) : -1;
	ws = WIFEXITED(status[1]) ? WEXITSTATUS(status[1]) : -1;
	verdict(34, killed == 0 && others == 0 && rs == 0 && ws == 0,
		"%d bytes written in 1000s ended %d and read in 777s ended %d "
		"(%s); %d of 2 still there after 60 s, %d other children "
		"collected", PS_BYTES, ws, rs,
		rs == 0 ? "every byte as written" : rs == 1 ? "a byte WRONG"
		: rs == 2 ? "a read FAILED" : "no answer", killed, others);
}

int
main(int argc, char **argv)
{
	if (argc > 1 && strcmp(argv[1], "child") == 0)
		return as_exec_child();
	if (argc > 1 && strcmp(argv[1], "env") == 0)
		return as_env_child();
	if (argc > 1 && strcmp(argv[1], "dup") == 0)
		return as_dup_child();

	printf("musl_test: started -- a static C program, on %zu-bit "
	       "pointers (#414)\n", sizeof(void *) * 8);

	arm_format();
	arm_malloc();
	arm_mmap();
	arm_tls();
	arm_own_id();
	arm_threads();
	arm_detached();
	arm_signals();
	arm_console();
	arm_file();
	arm_sleep();
	arm_jump();
	arm_fork();
	arm_file_map();
	arm_program(14, "/hello_world");
	arm_parked_program(15, "/hello_exec");
	arm_program(16, "/hello_dyn_world");
	arm_fault();
	arm_fault_with_no_libc();
	arm_page_zero();
	arm_proc();
	arm_many_forks();
	arm_threads_in_servers();
	arm_null_signal();
	arm_clocks();
	arm_environment();
	/*
	 * #917: two programs that call libmach's mach_error() and
	 * mach_error_string(), static against uros_libc and dynamic against
	 * libc.so.  Each prints the text of KERN_INVALID_ARGUMENT and exits 0
	 * only if it is the one libmach's table holds; neither linked before.
	 */
	arm_program(27, "/mach_error_test");
	arm_program(28, "/mach_error_dyn");
	/*
	 * #929: a program linked as a build that is not ours links it,
	 * -static with a plain -lc through the installed musl-gcc.specs.  It
	 * starts in musl's crt1, and exits 0 only if getpid() answered and
	 * printf() printed.
	 */
	arm_program(29, "/static_lc_test");
	/* #897: dup(), dup2() and dup3(), in this task and across execve(). */
	arm_dup();
	arm_dup_across_exec();
	/* #904: pipes, in this process and between processes. */
	arm_pipe();
	arm_pipe_writers();
	/* #904: a pipe's ends going, which pipe_server sees by its tokens.
	 * Numbered after #934's wait arms, and called here, before [34]. */
	arm_pipe_eof();
	arm_pipe_epipe();
	arm_pipe_holder_killed();
	arm_pipe_stream();

	printf("musl_test: %d of %d arms passed\n", arms_passed, arms_run);
	return 0;
}
