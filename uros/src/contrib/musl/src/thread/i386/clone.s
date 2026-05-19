/*
 * Uros patch (#256 / Phase 6a): replace the Linux int $0x80 clone
 * with a C-ABI thunk that delegates to libposix-uros's __uros_clone.
 * That function does the Mach thread_create + i386_set_ldt +
 * thread_set_state + thread_resume dance directly — clone(2) on
 * Linux is a single syscall; on Mach it is several MIG RPCs that
 * are easier to express in C than in fragile inline asm.
 *
 * Calling convention (cdecl, matching musl's pthread_create caller):
 *   int __clone(int (*fn)(void*), void *stack, int flags,
 *               void *arg, pid_t *ptid, void *newtls, pid_t *ctid);
 *
 * Returns the new thread's tid in the parent, never returns 0 to the
 * caller (the child path lives entirely on the new Mach thread, set
 * up by __uros_clone — there's no "child returns from clone here").
 */

.text
.global __clone
.hidden __clone
.type   __clone, @function
__clone:
	pushl	%ebp
	movl	%esp, %ebp
	pushl	32(%ebp)       /* ctid    */
	pushl	28(%ebp)       /* newtls  */
	pushl	24(%ebp)       /* ptid    */
	pushl	20(%ebp)       /* arg     */
	pushl	16(%ebp)       /* flags   */
	pushl	12(%ebp)       /* stack   */
	pushl	 8(%ebp)       /* fn      */
	call	__uros_clone
	addl	$28, %esp
	popl	%ebp
	ret

.section .note.GNU-stack,"",@progbits
