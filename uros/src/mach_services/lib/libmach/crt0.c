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
#include <mach/bootstrap.h>


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
 */
__attribute__((weak)) int  (*_mach_init_routine)(void)      = 0;
__attribute__((weak)) int  (*_threadlib_init_routine)(void) = 0;
__attribute__((weak)) void (*_threadlib_exit_routine)(int)  = 0;

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

void __start_mach(void);

/*
 * __attribute__((optimize("no-stack-protector"))) ensures this function
 * is not instrumented with canary checks — the guard is not yet
 * initialized when we enter here.
 */
void __attribute__((no_stack_protector))
__start_mach(void)
{
	int retval;

	/* Must be first — initialize canary before any protected function. */
	init_stack_guard();

	if (*_mach_init_routine)
		(*_mach_init_routine)();

	__get_arguments();
	__get_environment();

	/*
	 * musl-linked servers: bring up TLS + stack canary before main()
	 * runs, so main()'s %gs:0x14 canary prologue sees a live TCB.
	 * No-op for legacy libmach servers (weak symbol absent).
	 */
	if (__uros_libc_init)
		__uros_libc_init();

	if (*_threadlib_init_routine) {
		int new_sp;

		new_sp = (*_threadlib_init_routine)();

		if (new_sp)
			__asm__ volatile("movl %0, %%esp" : : "g" (new_sp) );
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
