/*
 * Copyright 1991-1998 by Open Software Foundation, Inc. 
 *              All Rights Reserved 
 *  
 * Permission to use, copy, modify, and distribute this software and 
 * its documentation for any purpose and without fee is hereby granted, 
 * provided that the above copyright notice appears in all copies and 
 * that both the copyright notice and this permission notice appear in 
 * supporting documentation. 
 *  
 * OSF DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE 
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS 
 * FOR A PARTICULAR PURPOSE. 
 *  
 * IN NO EVENT SHALL OSF BE LIABLE FOR ANY SPECIAL, INDIRECT, OR 
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM 
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN ACTION OF CONTRACT, 
 * NEGLIGENCE, OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION 
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE. 
 */
/*
 * MkLinux
 */

/*
 * This is the Mach standalone startup.  It has been written from
 * scratch to only depend upon interfaces present in a standalone
 * Mach environment.  It contains no OS personality code.
 *
 * General flow structure:
 *
 *	call mach library initialization (if present)
 *	get arguments and environment
 *	call thread library initialization (if present)
 *	call main
 *	call thread library exit (if present) - called with main return value
 *	call exit - called with main return value
 */

#include <mach_init.h>
#include <mach/mach_interface.h>
#include <mach/mach_traps.h>		/* mach_thread_set_name (#425) */
#include <mach/bootstrap.h>
#include <threadlib_init.h>


/*
 * Weak NULL fallbacks: libpthreads / libmach / libcthreads override
 * these with strong definitions (e.g. _threadlib_init_routine =
 * pthread_init).  Originally they were tentative ("common") defs,
 * which combined with -Wl,--allow-multiple-definition let the BSS
 * copy win over the strong override — pthread_init never ran,
 * __pthread_stack_size stayed 0, and the first pthread_create() in
 * cap_server (via gpu_console_init_async) faulted in
 * _pthread_allocate_stack.  Marking them weak makes any strong
 * library def take precedence, while still building servers like
 * name_server that don't link a thread library.
 *
 * ⚠️ The TYPES are in <threadlib_init.h> now and not written out here, which
 * is a different point from the weakness: this file and the three thread
 * libraries each declared the same object in their own words, and one of them
 * called a stack pointer an int.  See that header for what that cost.
 */
__attribute__((weak)) int	  (*_mach_init_routine)(void)      = 0;
__attribute__((weak)) vm_offset_t (*_threadlib_init_routine)(void) = 0;
__attribute__((weak)) void	  (*_threadlib_exit_routine)(int)  = 0;

/*
 * musl libc init (#259).  Provided by the patched musl
 * (src/internal/uros_main_thread.c) and pulled in only by musl-linked
 * servers; weak so legacy libmach servers link without it.  Must run
 * BEFORE main() because musl-linked code reads the stack canary from
 * %gs:0x14 (the per-thread TLS slot) in its function prologues — that
 * slot only becomes valid once __uros_libc_init drives __init_tls +
 * __init_ssp.  Calling it from inside main() (as Phase 3-6 did) faults
 * main()'s own prologue on the not-yet-installed TLS.
 */
extern void __uros_libc_init(void) __attribute__((weak));

static char *__nullarg = 0;
static char **__argv = &__nullarg;
static int __argc = 0;
char **__environment = &__nullarg;

extern int main(int, char **);
extern void exit(int);
extern void init_stack_guard(void);

static void __setup_ptrs(vm_offset_t, vm_size_t, char ***, int *);
static void __get_arguments(void);
static void __get_environment(void);
static void __setup_args_from_stack(unsigned long entry_sp);

void __start_mach(void);                       /* asm trampoline, below */
void __start_mach_c(unsigned long entry_sp);

/*
 * ELF entry trampoline.  The kernel / exec_server start us with the stack
 * pointer at the System V argument frame ([argc][argv..][0][envp..][0]
 * [auxv..]).  Capture that pointer before the C prologue perturbs it, then
 * call the C startup with it.  Static, non-PIE binaries only — the direct
 * call needs no PLT.
 *
 * ⚠️ The two are not the same sequence at different widths, and that is the
 * whole reason this is written twice rather than parameterised.  The
 * argument travels differently — the stack on one, a register on the other —
 * so on x86-64 there is nothing to push and nothing to make room for.
 *
 * ⚠️ Alignment is the part to get right in both, and the rule is about the
 * CALL and not about this function: the ABI wants the stack pointer
 * sixteen-byte aligned AT the call instruction, so that the callee sees it
 * eight past a boundary once the return address is on.  i386 aligns and then
 * subtracts twelve so its one pushed argument lands the total back on
 * sixteen; x86-64 aligns and calls, because it pushes nothing.
 */
#if defined(__x86_64__)

__asm__(
    ".text\n"
    ".globl __start_mach\n"
    ".type __start_mach,@function\n"
    "__start_mach:\n"
    "    xorl %ebp, %ebp\n"        /* outermost frame                     */
    "    movq %rsp, %rdi\n"        /* &argc — the first argument register */
    "    andq $-16, %rsp\n"        /* 16-byte align, nothing to push      */
    "    call __start_mach_c\n"
    "    hlt\n");

#elif defined(__i386__)

__asm__(
    ".text\n"
    ".globl __start_mach\n"
    ".type __start_mach,@function\n"
    "__start_mach:\n"
    "    xorl %ebp, %ebp\n"        /* outermost frame */
    "    movl %esp, %eax\n"        /* %eax = &argc (entry %esp) */
    "    andl $-16, %esp\n"        /* 16-byte align */
    "    subl $12, %esp\n"
    "    pushl %eax\n"             /* arg: entry_sp */
    "    call __start_mach_c\n"
    "    hlt\n");

#else
#error "crt0: no entry trampoline for this architecture"
#endif

/*
 * __attribute__((optimize("no-stack-protector"))) ensures this function
 * is not instrumented with canary checks — the guard is not yet
 * initialized when we enter here.
 */
void __attribute__((no_stack_protector))
__start_mach_c(unsigned long entry_sp)
{
	int retval;

	/* Must be first — initialize canary before any protected function. */
	init_stack_guard();

	if (*_mach_init_routine)
		(*_mach_init_routine)();

	__get_arguments();
	__get_environment();

	/*
	 * bootstrap_arguments only answers for tasks the bootstrap server
	 * loaded (it keys on task_port in servers[]); for execve'd images
	 * it returns KERN_INVALID_ARGUMENT, so __argc is still 0 here.
	 * Those images carry their argv/envp on the System V entry stack
	 * that exec_server built — read it from there.  Bootstrap-loaded
	 * servers already have their args (or a zeroed argc=0 stack), so
	 * this is a pure fallback that never overrides the RPC path.
	 */
	if (__argc == 0)
		__setup_args_from_stack(entry_sp);

	/*
	 * Say who we are, before main() (#425).
	 *
	 * struct task has no name, so a kernel report that lists tasks can only
	 * give their addresses -- and when the machine wedges, a log naming five
	 * live user tasks as five pointers cannot say which program stopped.
	 * That happened twice in one afternoon: the census printed
	 * `task=0xffffc000004d83a0 susp=2' and the answer had to be guessed from
	 * which test had failed to report.  The debugger was no better; its
	 * list_tasks() prints the same bare pointer.
	 *
	 * thread->name already exists and already survives into every kernel
	 * report, so the identity goes there instead of into a new field and a
	 * new RPC: one trap here, and from its first instruction every task
	 * carries its own name.  Naming a task by its first thread is exact for
	 * as long as that thread lives, which for a bundle task is its whole
	 * run.
	 *
	 * The last path component, not the whole string, for the reason #488
	 * found: the loaders disagree about what argv[0] holds -- GRUB writes
	 * the module name and QEMU -initrd writes the HOST path -- and the tail
	 * is the part they agree on.  ⚠️ Sixteen bytes including the NUL, so a
	 * long server name arrives truncated; copyinstr in the trap does the
	 * truncating, which is why no local buffer is needed here.
	 */
	if (__argc > 0 && __argv != 0 && __argv[0] != 0) {
		const char *base = __argv[0];
		const char *p;

		for (p = __argv[0]; *p != '\0'; p++)
			if (*p == '/')
				base = p + 1;

		if (*base != '\0')
			(void) mach_thread_set_name(base);
	}

	/*
	 * musl-linked servers: bring up TLS + stack canary before main()
	 * runs, so main()'s %gs:0x14 canary prologue sees a live TCB.
	 * No-op for legacy libmach servers (weak symbol absent).
	 */
	if (__uros_libc_init)
		__uros_libc_init();

	if (*_threadlib_init_routine) {
		vm_offset_t new_sp;

		new_sp = (*_threadlib_init_routine)();

		if (new_sp)
			/*
			 * 🔥 This was `int new_sp' and a `movl %0, %%esp', and
			 * both halves of that were a truncation that assembles
			 * cleanly: %esp is the low half of the stack pointer,
			 * so the instruction was legal and would have carried
			 * on below four gigabytes.
			 *
			 * The movq was written first, with a note saying the
			 * declaration was the thing that lied and that
			 * widening it belonged to #425.  #425 closed without
			 * it, so the cast to unsigned long widened a value
			 * that had already lost its top half -- pthread_init
			 * returns a stack pointer and was declared to return
			 * an int -- and the sign extension put it at
			 * 0xffffffffffeffde0, where `call main' faulted.
			 *
			 * The types are one declaration now, in
			 * <threadlib_init.h>, so this reads what it is given.
			 */
#if defined(__x86_64__)
			__asm__ volatile("movq %0, %%rsp" : : "g" (new_sp));
#else
			__asm__ volatile("movl %0, %%esp" : : "g" (new_sp));
#endif
	}

	retval = main(__argc, __argv);

	if (*_threadlib_exit_routine)
		(*_threadlib_exit_routine)(retval);

	exit(retval);
}

static void
__setup_ptrs(
	vm_offset_t strs,
	vm_size_t strs_size,
	char ***ptrs_ptr,
	int *nptrs_ptr)
{
	kern_return_t kr;
	vm_offset_t ptrs;
	int nptrs;
	char *ptr;
	char *endptr;
	int i;

	nptrs = 0;
	ptr = (char *)strs;
	endptr = ptr + strs_size;
	while (ptr < endptr) {
		nptrs++;
		while (*ptr++)
			;
	}
	kr = vm_allocate(mach_task_self(), &ptrs, (nptrs+1) * sizeof(char *),
			 TRUE);
	if (kr != KERN_SUCCESS) {
		vm_deallocate(mach_task_self(), strs, strs_size);
		return;
	}
	ptr = (char *)strs;
	*ptrs_ptr = (char **)ptrs;
	*nptrs_ptr = nptrs;
	for (i = 0; i < nptrs; i++) {
		(*ptrs_ptr)[i] = ptr;
		while (*ptr++)
			;
	}
}

static void
__get_arguments(void)
{
	kern_return_t kr;
	vm_offset_t arguments;
	mach_msg_type_number_t arguments_size;

	kr = bootstrap_arguments(bootstrap_port,
				 mach_task_self(),
				 &arguments,
				 &arguments_size);
	if (kr != KERN_SUCCESS || arguments_size == 0)
		return;
	__setup_ptrs(arguments, arguments_size, &__argv, &__argc);
}

static void
__get_environment(void)
{
	kern_return_t kr;
	vm_offset_t environment;
	mach_msg_type_number_t environment_size;
	int nptrs;

	kr = bootstrap_environment(bootstrap_port,
				   mach_task_self(),
				   &environment,
				   &environment_size);
	if (kr != KERN_SUCCESS || environment_size == 0)
		return;
	__setup_ptrs(environment, environment_size, &__environment, &nptrs);
}

/*
 * Parse argc/argv/envp from the System V i386 entry stack that
 * exec_server builds for execve'd images:
 *
 *   entry_sp -> [ argc ][ argv[0..argc-1] ][ NULL ][ envp[0..] ][ NULL ]...
 *
 * Only called as a fallback when bootstrap_arguments yielded nothing
 * (i.e. for an execve'd task).  The pointers live in the task's own
 * stack page, so we hand them to main() directly — no copy needed.
 */
static void
__setup_args_from_stack(unsigned long entry_sp)
{
	int *frame;
	int argc;
	char **argv;

	if (entry_sp == 0)
		return;

	frame = (int *)entry_sp;
	argc  = frame[0];
	/* A zeroed (bootstrap set_regs) or implausible frame -> leave the
	 * defaults so we don't dereference garbage. */
	if (argc <= 0 || argc > 4096)
		return;

	argv = (char **)&frame[1];
	if (argv[argc] != 0)            /* argv must be NULL-terminated */
		return;

	__argv = argv;
	__argc = argc;

	/* envp immediately follows the argv NULL terminator. */
	if (__environment == &__nullarg)
		__environment = &argv[argc + 1];
}
