/*
 * Uros patch (#259): replace the int $0x80 set_thread_area path with a
 * C-ABI tail-call into libposix-uros, mirroring the clone.s -> __uros_clone
 * idiom (#256).  Uros routes Linux syscalls through __uros_syscallN C
 * stubs, not int $0x80 (which the kernel treats as a Mach trap), so the
 * old inline trap never reached our SYS_set_thread_area handler.
 *
 * __init_tp calls __set_thread_area(TP_ADJ(p)); on i386 TP_ADJ(p)==p, so
 * the single cdecl argument is the pthread pointer.  __uros_set_thread_area_tp
 * installs the per-thread LDT descriptor and loads %gs, returning 0 on
 * success / -1 on failure -- exactly the contract __init_tp expects.
 */
.text
.global __set_thread_area
.hidden __set_thread_area
.type   __set_thread_area,@function
__set_thread_area:
	jmp __uros_set_thread_area_tp
