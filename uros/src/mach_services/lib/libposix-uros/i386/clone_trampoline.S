/*
 * libposix-uros — entry stub for pthread-created Mach threads (#256).
 *
 * thread_create_running drops the new thread here with:
 *   %ebp = thread function pointer (set up by posix_clone.c's
 *          state.ebp before thread_set_state)
 *   %esp = top of the new thread's stack, with the start-function
 *          argument already pushed by __uros_clone
 *   %gs  = TLS selector (or 0x1f if no TLS was requested) — kernel
 *          loads it from state.gs on resume
 *
 * Calls fn(arg) on a clean frame; when fn returns, we feed its return
 * value to _exit (musl) so the thread vanishes cleanly.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

.text
.globl __uros_clone_trampoline
.type  __uros_clone_trampoline, @function
__uros_clone_trampoline:
        movl    %ebp, %eax              /* fn = saved in ebp by clone */
        xorl    %ebp, %ebp              /* zero frame ptr — debugger hint */
        call    *%eax                   /* call fn(arg) — arg at 4(%esp) */
        /* fn returned with the exit code in %eax. */
        pushl   %eax
        call    _exit
1:      jmp     1b                      /* unreachable */

.section .note.GNU-stack,"",@progbits
