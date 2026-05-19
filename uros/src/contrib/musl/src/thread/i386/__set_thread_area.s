.text
.global __set_thread_area
.hidden __set_thread_area
.type   __set_thread_area,@function
__set_thread_area:
	push %ebx
	push $0x51
	push $0xfffff
	push 16(%esp)
	call 1f
1:	addl $4f-1b,(%esp)
	pop %ecx
	mov (%ecx),%edx
	push %edx
	mov %esp,%ebx
	xor %eax,%eax
	mov $243,%al
	int $128
	testl %eax,%eax
	jnz 2f
	movl (%esp),%edx
	movl %edx,(%ecx)
	# Uros patch (#256 / Phase 6a): the entry_number returned by
	# our SYS_set_thread_area is an LDT slot, not a Linux GDT slot.
	# Selector = (idx << 3) | TI(LDT,0x4) | RPL(3) — so the
	# constant added below becomes 7, not 3.
	leal 7(,%edx,8),%edx
3:	movw %dx,%gs
1:
	addl $16,%esp
	popl %ebx
	ret
2:
	mov %ebx,%ecx
	xor %eax,%eax
	xor %ebx,%ebx
	xor %edx,%edx
	mov %ebx,(%esp)
	mov $1,%bl
	mov $16,%dl
	mov $123,%al
	int $128
	testl %eax,%eax
	jnz 1b
	mov $7,%dl
	inc %al
	jmp 3b

.data
	.align 4
4:	.long -1
