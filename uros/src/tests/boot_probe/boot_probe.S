/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The first 64-bit image this kernel loads (#422).
 *
 * ── Why the smallest possible program, and why in assembly ────────────
 *
 * The issue's done-when is a bundle whose servers start, and no server can be
 * built for this target yet: musl (#414), libmach (#426), libpthreads (#425)
 * and libposix (#424) are all still i386-only, so `src/CMakeLists.txt' skips
 * the whole userland here.  Waiting for them would mean writing the kernel's
 * ELF64 loader, the task set-up and the argument block with nothing to run
 * against them — the shape that cost #453 and #459 five defects each.
 *
 * So this exists to be loaded.  It has no libc, no runtime, no relocations and
 * no dependence on any of those issues, which means it can be built today and
 * cannot break when they land.  What it proves is exactly what the loader has
 * to be right about first: an ELF64 image was read, its segments were mapped,
 * a task and a thread were built around it, and the entry point reached ring 3
 * and asked the kernel something.
 *
 * ⚠️ It is NOT a substitute for the done-when.  A server needs argv, a bundle,
 * ports and a C runtime; this needs a page of text and a stack.  Passing here
 * says the loader works, not that the fleet boots.
 *
 * ── How it says it is alive ───────────────────────────────────────────
 *
 * With the only call this kernel answers: SYSCALL_NR_PROBE, which exists for
 * the ring-3 test in the boot self-checks.  Reusing it rather than adding a
 * "print" call keeps the syscall table at the one entry #411 designed, and it
 * means the answer is checked by machinery that is already tested — the probe
 * returns its six arguments packed one byte each, so a wrong argument register
 * shows up as a wrong byte in a known position.
 *
 * Then it calls again, forever.  A single call would prove the entry point was
 * reached; the loop proves the task keeps running — that it is scheduled,
 * preempted by the timer, and resumed, which is the whole of a user task being
 * alive rather than merely started.
 */

/* <syscall/syscall.h>, which this cannot include: it is built for ring 3. */
#define SYSCALL_NR_PROBE	0

/*
 * The marker the kernel looks for.  Distinct from the ring-3 self-test's
 * 1,2,3,4,5,6 so that the two callers cannot be confused: they take the same
 * path into the kernel a few hundred milliseconds apart, and a count that both
 * could have produced would prove neither.
 */
#define PROBE_A1	0x51
#define PROBE_A2	0x52
#define PROBE_A3	0x53
#define PROBE_A4	0x54
#define PROBE_A5	0x55
#define PROBE_A6	0x56

	.section .text
	.code64
	.globl	_start
_start:
	/*
	 * The argument registers of the contract in <syscall/syscall.h>: rdi,
	 * rsi, rdx, r10, r8, r9 — r10 and not rcx, because the instruction
	 * takes rcx for the return address.
	 *
	 * Loaded once, outside the loop: the contract declares them destroyed,
	 * so they have to be reloaded each time, which is what the loop does.
	 * Written out rather than hidden in a macro because these six lines are
	 * the entire user side of the calling convention.
	 */
1:
	movq	$SYSCALL_NR_PROBE, %rax
	movq	$PROBE_A1, %rdi
	movq	$PROBE_A2, %rsi
	movq	$PROBE_A3, %rdx
	movq	$PROBE_A4, %r10
	movq	$PROBE_A5, %r8
	movq	$PROBE_A6, %r9
	syscall
	jmp	1b
