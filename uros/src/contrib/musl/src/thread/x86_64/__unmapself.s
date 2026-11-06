/*
 * Uros patch (#414): a detached thread gives its stack back and exits, and has
 * no stack left to do the second half on.  The Linux numbers this used are not
 * reachable by SYSCALL here; libposix-uros has the same sequence in Mach traps.
 */
.text
.global __unmapself
.type   __unmapself,@function
__unmapself:
	jmp __uros_unmapself
