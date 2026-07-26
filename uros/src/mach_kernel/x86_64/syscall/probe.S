/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Ring 3, entered on purpose (#411, MD contract 6/6).
 */

#include <cpu/desc.h>
#include <syscall/probe.h>
#include <syscall/syscall.h>

	.section .text
	.code64

/*
 * user_probe_enter(entry, stack) — drop to ring 3, come back on a fault.
 *
 * There is no instruction for "return to user mode".  There is only IRETQ,
 * which returns to whatever the five words on the stack describe — so
 * entering ring 3 is the same operation as returning from an interrupt that
 * never happened, with a frame written by hand.  The processor drops
 * privilege because the code selector in that frame says ring 3, and for no
 * other reason.
 *
 * The callee-saved registers are pushed before the frame because the way
 * back is not a return: the fault handler rewrites its own frame to land at
 * user_probe_return with the stack pointer saved below, so whatever ring 3
 * did to the registers is undone from the stack rather than by the caller.
 */
	.globl	user_probe_enter
	.globl	user_probe_return
	.align	16
user_probe_enter:
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	/*
	 * Arm the way back before there is any way back, and do it from here
	 * rather than from the caller: the stack the return has to land on is
	 * the one that exists at this instant, after the pushes above and
	 * before the frame below, and only this function knows where that is.
	 *
	 * The arguments are moved out of the way first — they are the same
	 * registers the call uses — and the stack is realigned around it, so
	 * that this stays a well-formed call whatever the callee turns into.
	 */
	movq	%rsp, %rbx			/* where the return must land */
	movq	%rdi, %r12			/* entry                      */
	movq	%rsi, %r13			/* user stack                 */

	leaq	user_probe_return(%rip), %rdi
	movq	%rbx, %rsi
	subq	$8, %rsp
	call	trap_expect_user
	addq	$8, %rsp

	/*
	 * The frame IRETQ will consume, pushed in the order it pops: stack
	 * segment, stack pointer, flags, code segment, instruction pointer.
	 *
	 * Flags are 0x202 — the one reserved bit that must be set, and
	 * interrupts enabled.  Ring 3 with interrupts disabled would be a
	 * user program the machine cannot take away from, which is not a
	 * thing worth being able to do even once.
	 */
	/*
	 * And %gs becomes the user's, which is what makes the kernel's copy
	 * reachable again only through swapgs.  Every real return to ring 3
	 * does this; the probe does it so that the entry paths are tested
	 * against two different bases rather than against one value stored
	 * twice — with those equal, a missing swapgs is indistinguishable
	 * from a correct one.
	 */
	swapgs

	pushq	$USER_DS_RPL3
	pushq	%r13
	pushq	$0x202
	pushq	$USER_CS_RPL3
	pushq	%r12
	iretq

user_probe_return:
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	ret


/*
 * The probe itself — the only code in this kernel that runs without rights.
 *
 * It travels in the image and is copied into a page belonging to an address
 * space, so every address it names has to be the one it will have *there*,
 * not here.  Hence the absolute forms: there is no relocation that could fix
 * these up, because the page it runs from is not the page it was linked at.
 *
 * What it does is deliberately two separate things.  First a store, which is
 * the evidence that user code ran at all — a fault on the very first
 * instruction would look identical from the kernel's side otherwise.  Then
 * HLT, which is privileged: in ring 0 it stops the processor, and in ring 3
 * it is a general protection fault and nothing else.  That is the whole
 * proof, in one instruction, and it needs no cooperation from anything.
 */
	.section .user_probe, "ax"
	.code64
	.globl	user_probe_code
user_probe_code:
	movabsq	$USER_PROBE_WITNESS, %rax
	movabsq	$USER_PROBE_DATA_VA, %r11
	movq	%rax, (%r11)

	/*
	 * A syscall, issued the way the contract in <syscall/syscall.h> says
	 * one is issued: number in rax, six arguments in rdi, rsi, rdx, r10,
	 * r8, r9 — r10 rather than rcx because the instruction takes rcx for
	 * the return address.
	 *
	 * Distinct small values rather than anything meaningful, so that the
	 * answer names each register: an argument that arrived in the wrong
	 * one shows up as a wrong byte, in a known position.
	 */
	movq	$SYSCALL_NR_PROBE, %rax
	movq	$1, %rdi
	movq	$2, %rsi
	movq	$3, %rdx
	movq	$4, %r10
	movq	$5, %r8
	movq	$6, %r9
	syscall

	/*
	 * r11 has to be reloaded: the instruction destroyed it, which is the
	 * contract working rather than failing.
	 */
	movabsq	$USER_PROBE_DATA_VA, %r11
	movq	%rax, 8(%r11)

	hlt
