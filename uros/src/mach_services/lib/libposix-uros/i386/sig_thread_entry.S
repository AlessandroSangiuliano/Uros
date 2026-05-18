/*
 * libposix-uros — i386 entry stub for the signal handler thread.
 *
 * thread_create_running drops us here with %eip pointing at this
 * label and %esp pointing at the top of a freshly allocated stack.
 * The C-side _C_signal_loop expects a clean call frame, so we zero
 * %ebp (debugger termination hint), call into C, and if the loop
 * ever returns we fall through to thread_terminate(mach_thread_self())
 * to avoid running off the end of the stack.
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

.text
.globl __uros_sig_thread_entry
.type  __uros_sig_thread_entry, @function
__uros_sig_thread_entry:
        xorl    %ebp, %ebp
        call    __uros_sig_loop
        /* Loop returned (unexpected) — politely terminate self. */
        call    __uros_sig_thread_die
1:      jmp     1b

/*
 * Phase 4b (#253): same pattern for the exception-port thread.
 * Kept separate so each entry is single-purpose and a stack trace in
 * DDB is unambiguous about which thread is which.
 */
.globl __uros_exc_thread_entry
.type  __uros_exc_thread_entry, @function
__uros_exc_thread_entry:
        xorl    %ebp, %ebp
        call    __uros_exc_loop
        call    __uros_sig_thread_die
2:      jmp     2b

.section .note.GNU-stack,"",@progbits
