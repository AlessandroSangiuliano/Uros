/*
 * libposix-uros — i386 setjmp-style state capture for fork() (#255).
 *
 * Snapshots the calling thread's user-mode register state into a
 * `struct i386_thread_state` and returns 0.  Segment registers and
 * EFLAGS are written as the same constants signals.c and exceptions.c
 * use for fresh thread_create_running spawns — Uros user tasks all
 * share the same user-mode descriptor layout, so reading the current
 * %cs/%ds/... isn't worth the asm cost (and on some kernels reading
 * %gs in user mode behaves oddly).
 *
 * Layout of `struct i386_thread_state`:
 *   off  field                 off  field
 *     0  gs                    24   ebp
 *     4  fs                    28   esp   (kernel — unused)
 *     8  es                    32   ebx
 *    12  ds                    36   edx
 *    16  edi                   40   ecx
 *    20  esi                   44   eax
 *    48  eip                   60   uesp  (user)
 *    52  cs                    64   ss
 *    56  efl
 *
 * Caller convention (cdecl): state ptr at 4(%esp).
 *
 * Author: Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * License: MIT
 */

.text
.globl __uros_capture_state
.type  __uros_capture_state, @function
__uros_capture_state:
        pushl   %eax                    # save EAX so we can use it as state ptr
        movl    8(%esp), %eax           # EAX = state ptr (skip saved eax + ret addr)

        /* Segment registers + EFLAGS: hardcoded to the same values
         * signals.c spawn_handler_thread uses (USER_CS=0x17, others
         * 0x1f, EFLAGS=0x202 with IF=1).  These are constants for
         * every user task in Uros. */
        movl    $0x1f,  0(%eax)         # gs
        movl    $0x1f,  4(%eax)         # fs
        movl    $0x1f,  8(%eax)         # es
        movl    $0x1f, 12(%eax)         # ds
        movl    $0x17, 52(%eax)         # cs
        movl    $0x202, 56(%eax)        # eflags (IF=1)
        movl    $0x1f, 64(%eax)         # ss

        /* General-purpose registers — actual current values. */
        movl    %edi, 16(%eax)
        movl    %esi, 20(%eax)
        movl    %ebp, 24(%eax)
        movl    $0,   28(%eax)          # kernel esp — unused
        movl    %ebx, 32(%eax)
        movl    %edx, 36(%eax)
        movl    %ecx, 40(%eax)

        /* Original EAX is on the stack at 0(%esp).  Use %edx as a
         * scratch since we no longer need it. */
        movl    0(%esp), %edx
        movl    %edx, 44(%eax)

        /* EIP = return address the caller will jump back to,
         * stored at 4(%esp) (right above our pushed EAX). */
        movl    4(%esp), %edx
        movl    %edx, 48(%eax)

        /* UESP = caller's ESP at the moment of `call` = current ESP +
         * 4 (skip our saved EAX) + 4 (skip return addr) = +8.  When
         * the child resumes via thread_create_running with EIP set
         * above, the kernel's iret-equivalent lands it as if our
         * `ret` had popped the return addr — same call frame, just
         * with the parent-overwritten EAX value visible to the C
         * caller. */
        leal    8(%esp), %edx
        movl    %edx, 60(%eax)

        popl    %eax                    # restore parent's EAX
        xorl    %eax, %eax              # return 0 from this call
        ret

.section .note.GNU-stack,"",@progbits
