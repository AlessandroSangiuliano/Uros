/*
 * Uros patch (#414): there is no shared address space to borrow here, so vfork
 * is fork, and the whole of it, because a child needs fork()'s handlers to
 * have a usable personality (#292).  SYS_vfork has no entry in libposix-uros's
 * table, and SYSCALL would not reach the table anyway.
 */
.global vfork
.type vfork,@function
vfork:
	jmp fork
