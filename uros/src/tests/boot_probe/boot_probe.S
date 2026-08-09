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

/*
 * And the Mach traps it makes — through libmach's stubs since #426.
 *
 * ⚠️ The trap numbers used to be spelled out here, as SYSCALL_MACH_BASE + 14
 * and + 15, in a file that could not include the header that defines the base.
 * Two halves that agreed by inspection: exactly the shape of #448, in a place
 * where nothing would ever have compared them again.
 *
 * They are gone.  `call mach_print' names a symbol that the linker resolves
 * against a stub generated from the machine-independent list of every trap in
 * the system, expanded by <mach/x86_64/syscall_sw.h>, which reads the base
 * from the kernel's own <syscall/syscall.h>.  One definition, three files
 * downstream of it, and a number that cannot be got wrong here because it is
 * not written here.
 *
 * This also makes the image a CONSUMER of the stubs rather than an imitation
 * of them.  What #426 delivers is the translation between two calling
 * conventions -- the C ABI's fourth argument in rcx, the kernel's in r10, and
 * arguments seven and up off the caller's stack and into the callee-saved
 * registers -- and none of that was exercised by a hand-written `syscall'
 * with one argument.
 *
 * mach_null is called first -- no arguments, returns at once -- because what
 * it proves is the DISPATCH: the range test, the bound against
 * mach_syscall_count, the call through the pointer table, and the way back
 * out through SYSRET.  A trap that does nothing proves all of that and
 * nothing else, so a failure has one meaning.
 *
 * ⚠️ mach_print needs the copy that #411 gave it: it is `printf("%s", s)'
 * with a pointer the USER supplied, and before that change the kernel walked
 * a string in a task's address space with the console lock already held.
 */

	.section .rodata
hello:
	.asciz	"boot_probe: hello from ring 3, through a Mach trap\r\n"

	.section .text
	.code64
	.globl	_start
_start:
	/*
	 * ⚠️ A CALL, which means this needs a stack — and needing one is part
	 * of what is being proved.  The stack and the argument block are built
	 * by build_args_and_stack() with copyout(), and until #468 those
	 * copies silently wrote nothing: the region was allocated and the
	 * words never arrived.  An image that only ever issued `syscall'
	 * touched its stack pointer never and could not have noticed.
	 */
	call	mach_null

	/* And one that does something, and says so in our own words. */
	leaq	hello(%rip), %rdi
	call	mach_print

	movq	$SYSCALL_NR_PROBE, %rax
	movq	$PROBE_A1, %rdi
	movq	$PROBE_A2, %rsi
	movq	$PROBE_A3, %rdx
	movq	$PROBE_A4, %r10
	movq	$PROBE_A5, %r8
	movq	$PROBE_A6, %r9
	syscall

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
