/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Somewhere with a known address and a known stack pointer, so that the frame
 * an entry builds can be compared against something (#409).
 *
 * ── Why this is not written in C ──────────────────────────────────────
 *
 * The check these exist for is "the reported rip and rsp are the ones the
 * interrupted code actually had".  Both sides of that comparison have to come
 * from the interrupted code itself: a caller in C can note its own %rsp, but
 * between that statement and the instruction that gets interrupted the
 * compiler is free to move the stack pointer, spill, or inline the whole thing
 * somewhere else — and then a disagreement means nothing and an agreement
 * means less.
 *
 * Here the two are three instructions apart and nothing in between touches
 * %rsp, so `entry_probe_rsp' is the value the frame must carry, exactly, and
 * `entry_probe_lo .. entry_probe_hi' is where the saved rip must lie.  That is
 * a known-good reference rather than a plausibility check, which is what #458
 * asked for after two hours went into reconciling an rsp that was never going
 * to reconcile.
 *
 * ⚠️ None of these pushes anything.  That is what lets a fault inside one be
 * resumed at trap_probe_faulted (trap/entry.S), whose `ret' then finds the
 * caller's return address exactly where the call left it.
 */

	.section .text
	.code64

/*
 * The stack pointer the probes were running on, published by the probe rather
 * than passed in.
 */
	.section .data
	.align	8
	.globl	entry_probe_rsp
entry_probe_rsp:
	.quad	0

/*
 * Set by a handler to tell the spin below that its event has arrived.  In
 * .data and not .bss for no deeper reason than that it is written from an
 * interrupt handler and reading a definite zero at boot is one less thing.
 */
	.globl	entry_probe_arrived
entry_probe_arrived:
	.quad	0

	.section .text

/*
 * Wait here for an interrupt, for at most `%rdi' turns of the loop.
 *
 * The bound is the caller's because only the caller knows what it is waiting
 * for — a tick at 500 Hz and a self-NMI that is already in flight are four
 * orders of magnitude apart, and a loop that spun for the longer of them on
 * every test would be paying for the worst case every time.
 *
 * Returns the number of turns left, so a caller can tell "the event arrived"
 * from "the bound ran out" without a second variable: zero is the timeout.
 */
	.globl	entry_probe_spin
	.globl	entry_probe_lo
	.globl	entry_probe_hi
	.align	16
entry_probe_spin:
entry_probe_lo:
	movq	%rsp, entry_probe_rsp(%rip)
	movq	$0, entry_probe_arrived(%rip)

1:	movq	entry_probe_arrived(%rip), %rax
	testq	%rax, %rax
	jnz	2f
	decq	%rdi
	jnz	1b
2:	movq	%rdi, %rax
entry_probe_hi:
	ret

/*
 * Fault at an address the caller is told, on a stack pointer the caller is
 * told, so that both fields of the frame can be checked and not just the one
 * that is easy.
 *
 * The invalid opcode is the vector with no error code and no ambiguity about
 * where it was raised: `ud2' is a fault, so the saved rip is the instruction
 * itself rather than the one after it — which is the property being checked,
 * and the reason this is not `int3'.
 */
	.globl	entry_probe_fault
	.globl	entry_probe_fault_rip
	.align	16
entry_probe_fault:
	movq	%rsp, entry_probe_rsp(%rip)
	leaq	1f(%rip), %rax
	movq	%rax, entry_probe_fault_rip(%rip)
1:	ud2
	xorl	%eax, %eax		/* never reached; the handler resumes */
	ret

/*
 * Take vector 2 at an instruction the caller is told.
 *
 * ⚠️ `int $2' is NOT an NMI and this does not claim to be one: it raises no
 * NMI-blocking latch and nothing outside the processor is involved.  What it
 * is, exactly, is a delivery through IDT[2] — the same gate, the same IST
 * slot, the same paranoid stub in trap/entry.S, the same frame.  So it is the
 * one way to make that path arrive at an address chosen in advance, which is
 * what the frame has to be checked against.
 *
 * The genuine article is tested for what this cannot show — that a message
 * from the interrupt controller arrives at all, and that the entry gets %gs
 * right when it does — by the self-sent NMI beside it and by the swapgs window
 * test.  Two halves, each saying what it is.
 *
 * The address recorded is the one after the instruction, because that is what
 * the processor saves for a software interrupt, and it is recorded on the way
 * back so that the label and the value cannot drift apart.
 */
	.globl	entry_probe_int2
	.globl	entry_probe_int2_rip
	.align	16
entry_probe_int2:
	movq	%rsp, entry_probe_rsp(%rip)
	int	$2
1:	leaq	1b(%rip), %rax
	movq	%rax, entry_probe_int2_rip(%rip)
	ret

	.section .data
	.align	8
entry_probe_fault_rip:
	.quad	0
entry_probe_int2_rip:
	.quad	0
