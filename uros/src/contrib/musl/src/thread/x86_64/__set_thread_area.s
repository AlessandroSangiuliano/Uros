/*
 * Uros patch (#414): arch_prctl is a Linux number, and SYSCALL on Uros reaches
 * the Mach trap table.  __init_tp calls __set_thread_area(TP_ADJ(p)), and on
 * x86-64 TP_ADJ(p)==p; libposix-uros sets this thread's FS base and answers 0
 * or -1, which is the contract __init_tp reads.  The idiom of i386's file (#259).
 */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	jmp __uros_set_thread_area_tp
