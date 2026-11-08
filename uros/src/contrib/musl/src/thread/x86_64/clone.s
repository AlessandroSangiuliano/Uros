/*
 * Uros patch (#414): __uros_clone takes the same seven arguments in the same
 * order, and the C ABI puts them in the same registers and the seventh in the
 * same stack slot, so a jump is the whole thunk.  What Linux does in one
 * system call is several Mach calls here, written in C in libposix-uros
 * (posix_clone.c).  The child never comes back through this file: it starts at
 * __uros_clone_trampoline, on its own stack.
 */
.text
.global __clone
.hidden __clone
.type   __clone,@function
__clone:
	jmp __uros_clone
