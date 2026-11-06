/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * A static C program asks its C library what a program asks it (#414).
 *
 * This is the box "a static musl binary runs", and it is written as a program
 * and not as a test of libposix-uros: nothing here knows it is on Mach, there
 * is no Mach header and no trap, only POSIX.  An arm that passes says the
 * whole path holds, from the libc call through musl's patched syscall_arch.h
 * and the dispatch table to the kernel, on the numbers and the structure
 * layouts of this target.
 *
 * Each arm reads back what it did.  Where an arm needs something this boot may
 * not have -- a disk, a process server -- it says NOT ASKED and does not count.
 */

#include <errno.h>
#include <fcntl.h>
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE		4096u

static int	arms_run;
static int	arms_passed;

/*
 * One arm's verdict, one line.  `ok' is the arm's own conclusion and what
 * follows says what was read, either way.
 */
static void
verdict(int arm, int ok, const char *fmt, ...)
{
	char	text[200];
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
	return NULL;
}

static void
arm_tls(void)
{
	struct tls_report	r;
	pthread_t		th;
	int			rc;

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

	if (access_by_stat(path) != 0) {
		not_asked(arm, "the program is not on this boot's disk");
		return;
	}
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

	if (access_by_stat(path) != 0) {
		not_asked(arm, "the program is not on this boot's disk");
		return;
	}
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

int
main(void)
{
	printf("musl_test: started -- a static C program, on %zu-bit "
	       "pointers (#414)\n", sizeof(void *) * 8);

	arm_format();
	arm_malloc();
	arm_mmap();
	arm_tls();
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

	printf("musl_test: %d of %d arms passed\n", arms_passed, arms_run);
	return 0;
}
