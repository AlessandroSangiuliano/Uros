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
 * So this exists to be loaded.  It had no libc, no runtime, no relocations and
 * no dependence on any of those issues, which means it could be built the day
 * the loader existed and could not break when they landed.  What it proved is
 * exactly what the loader has to be right about first: an ELF64 image was
 * read, its segments were mapped, a task and a thread were built around it,
 * and the entry point reached ring 3 and asked the kernel something.
 *
 * ── And what it is now (#426) ─────────────────────────────────────────
 *
 * The list above has one entry left on it.  libmach builds here, so this is
 * no longer its own startup: it is entered at __start_mach, mach_init runs
 * before it does, and it is called as `main(argc, argv)' — which is what a
 * server is.  The image kept its shape and changed what stands behind it.
 *
 * ⚠️ Still NOT the done-when.  A server also needs a bundle, a name server
 * and ports it did not make itself.  What can be said now is narrower and
 * new: the startup path a server comes through works on this architecture,
 * and the arguments the kernel wrote arrive at the far end of it.
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

/*
 * ══ The first message (#426) ══════════════════════════════════════════
 *
 * A task sending a message to itself: mach_reply_port() gives it a port it
 * holds the receive right for, and one mach_msg with SEND and RECEIVE set
 * puts a message on that port and takes it off again.  No server, no name
 * server, no second task — and still the whole path: the nine-argument stub,
 * the kernel's wide-argument push, ipc_kmsg_get with copyinmsg, the queue,
 * and copyoutmsg on the way back.
 *
 * ⚠️ This is the first time the WIDE stub is executed rather than
 * disassembled.  mach_msg_overwrite_trap takes nine arguments: six in the C
 * ABI's registers and three the caller leaves on its stack, which the stub
 * loads into rbx, r12 and r13 after saving them.  A misplacement there is not
 * a wrong number in a log — it is a message with the wrong reply port, or a
 * size read from the wrong word.  Which is why the answer is checked and not
 * merely printed.
 *
 * ⚠️ THE REPLY GOES IN A SECOND BUFFER, and that is what makes this a test of
 * the wide stub rather than only of the trap.
 *
 * The eighth argument, rcv_msg, is one of the three that travel on the
 * caller's stack and are loaded into rbx, r12 and r13 by the stub.  It is
 * read on the ordinary receive path and not only under MACH_RCV_OVERWRITE —
 * ipc/mach_msg.c does `ipc_kmsg_put(rcv_msg != MACH_MSG_NULL ? rcv_msg : msg,
 * ...)'.  So pointing it at a DIFFERENT buffer makes the delivery of argument
 * eight decidable: if the stub lost it, the reply lands in the send buffer
 * instead and this one stays as it was — all zeros, no identifier, no port.
 *
 * With three zeros there the test could not have failed.  notify and
 * scatter_list_size are ignored for these options, so garbage in them would
 * have changed nothing, and "the message went round" would have proved the
 * trap and said nothing about the stack arguments.
 *
 * ⚠️ And within the reply, msgh_local_port is the field that decides.  The id
 * is the sender's own value; the kernel writes the local port on the way out
 * (`msg->msgh_local_port = dest_name'), so it is the one thing in the answer
 * that ring 3 could not have put there.
 */
#define MACH_MSG_TYPE_MAKE_SEND	20
#define MACH_SEND_MSG		0x00000001
#define MACH_RCV_MSG		0x00000002

/* MACH_MSGH_BITS(MACH_MSG_TYPE_MAKE_SEND, 0) — remote in the low byte. */
#define MSGH_BITS		MACH_MSG_TYPE_MAKE_SEND

/*
 * <mach/message.h> laid out for this target, and the numbers are the
 * compiler's rather than this file's guess: every field of mach_msg_header_t
 * is 32 bits here, because #415 kept natural_t at 32 and mach_port_t with it.
 */
#define MSGH_BITS_OFF		0
#define MSGH_SIZE_OFF		4
#define MSGH_REMOTE_OFF		8
#define MSGH_LOCAL_OFF		12
#define MSGH_RESERVED_OFF	16
#define MSGH_ID_OFF		20
#define MSGH_SIZE		24

/* Room for the reply and the trailer the kernel appends behind it. */
#define MSG_BUF_SIZE		256

/* Ours, and recognisable in a hexadecimal dump. */
#define MSG_ID			0x5eed
#define MSG_ID_NARROW		0x5ee6

/* What the kernel is told, so that a stray probe call cannot look like this. */
#define IPC_A1			0x60
#define IPC_A1_NARROW		0x61
#define IPC_A1_ERROR		0x62
#define IPC_A1_ARGS		0x63
#define IPC_A6			0x66

	.section .rodata
hello:
	.asciz	"boot_probe: hello from ring 3, through a Mach trap\r\n"
argv0_prefix:
	.asciz	"boot_probe: the startup handed me argv[0] = "
newline:
	.asciz	"\r\n"

/*
 * ⚠️ A WRITABLE segment, which this image did not have until now — the loader
 * has had to map one, with the user and write bits, from p_flags it had never
 * been given in that combination.  The region count in the boot log says so.
 */
	.section .data
	.align	16
msg:
	.zero	MSG_BUF_SIZE
reply:
	.zero	MSG_BUF_SIZE

	.section .text
	.code64
	.globl	main
	.type	main,@function
main:
	/*
	 * ⚠️ A FUNCTION now, and not the entry point.  This image used to be
	 * its own crt0: it was entered at `_start', aligned the stack itself
	 * and answered to nobody.  It is entered at __start_mach today —
	 * libmach's startup, the one every server on the other target comes
	 * through — which runs mach_init and hands this argc and argv.
	 *
	 * So the prologue is the ABI's and not an image's: the six registers a
	 * function must give back, saved because a callee-saved register is a
	 * promise and this one makes it now.  The `andq $-16, %rsp' that used
	 * to be the first instruction is gone — a function that realigns its
	 * caller's stack has lost the frame it must return on, and the crt0
	 * has already done it at the one place the ABI names.
	 *
	 * ⚠️ Six pushes leave rsp eight past a boundary and not on one: it
	 * arrives that way, one return address deep, and 48 bytes do not
	 * change a value modulo 16.  Hence the subq — the ABI wants rsp
	 * aligned AT the call, so that the callee sees it eight past once its
	 * own return address is on.
	 */
	pushq	%rbp
	movq	%rsp, %rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	subq	$8, %rsp

	movl	%edi, %r13d			/* argc */
	movq	%rsi, %r12			/* argv */

	/*
	 * ── What the startup brought (#426) ──────────────────────────
	 *
	 * Four things the kernel can decide, and none of which this image
	 * could have produced on its own:
	 *
	 *   argc         from the argument block build_args_and_stack()
	 *                copied out, walked by crt0 and passed here;
	 *   argv[0]      its first eight bytes, as a value — the name the boot
	 *                script starts a bootstrap task under, which appears
	 *                nowhere in this file and is the kernel's own;
	 *   argv[argc]   the null terminator, so a count that happened to be
	 *                right for the wrong reason is still caught;
	 *   vm_page_size which mach_init did NOT compute: it asked, with
	 *                host_page_size(), and that is a MIG remote procedure
	 *                call — a message to the kernel and a reply back.
	 *
	 * ⚠️ The last is the one that is new.  Every trap so far has been one
	 * instruction into the kernel and out; this is the whole IPC path
	 * carrying a request a server would carry, from a startup nobody wrote
	 * for this architecture.  A page size of 4096 here is a round trip.
	 */
	movq	(%r12), %rax			/* argv[0]                  */
	movq	(%rax), %rdx			/* its first eight bytes    */
	movq	(%r12, %r13, 8), %r10		/* argv[argc] — must be zero */
	movq	vm_page_size(%rip), %r8
	movq	$SYSCALL_NR_PROBE, %rax
	movq	$IPC_A1_ARGS, %rdi
	movq	%r13, %rsi			/* argc */
	movq	$IPC_A6, %r9
	syscall

	/* And the name itself, in its own words, through the kernel. */
	leaq	argv0_prefix(%rip), %rdi
	call	mach_print
	movq	(%r12), %rdi
	call	mach_print
	leaq	newline(%rip), %rdi
	call	mach_print

	call	mach_null

	/* And one that does something, and says so in our own words. */
	leaq	hello(%rip), %rdi
	call	mach_print

	/* ── A port of our own, and a message to it ──────────────────── */
	call	mach_reply_port
	movl	%eax, %r14d			/* the port name, kept */

	leaq	msg(%rip), %r15
	movl	$MSGH_BITS, MSGH_BITS_OFF(%r15)
	movl	$MSGH_SIZE, MSGH_SIZE_OFF(%r15)
	movl	%r14d, MSGH_REMOTE_OFF(%r15)	/* to ourselves */
	movl	$0, MSGH_LOCAL_OFF(%r15)
	movl	$0, MSGH_RESERVED_OFF(%r15)
	movl	$MSG_ID, MSGH_ID_OFF(%r15)

	/*
	 * The three arguments above the sixth go where the C ABI puts them —
	 * on this stack, for the stub to pick up.  Thirty-two bytes and not
	 * twenty-four: the ABI wants rsp sixteen-byte aligned at the call, and
	 * three words would leave it eight out.
	 */
	leaq	reply(%rip), %rbx		/* where the answer must land */

	subq	$32, %rsp
	movq	$0, 0(%rsp)			/* notify            */
	movq	%rbx, 8(%rsp)			/* rcv_msg           */
	movq	$0, 16(%rsp)			/* scatter_list_size */

	movq	%r15, %rdi			/* msg       */
	movq	$(MACH_SEND_MSG|MACH_RCV_MSG), %rsi
	movq	$MSGH_SIZE, %rdx		/* send_size */
	movq	$MSG_BUF_SIZE, %rcx		/* rcv_size  — the stub moves it to r10 */
	movq	%r14, %r8			/* rcv_name  */
	movq	$0, %r9				/* timeout   */
	call	mach_msg_overwrite_trap

	addq	$32, %rsp

	/*
	 * And tell the kernel what came back, through the one call this image
	 * already had.  Reported rather than acted on: this program has no way
	 * to print a number, and the kernel is the only thing here that can
	 * say whether the answer is the right one.
	 */
	/*
	 * Read from the REPLY buffer, which is the point: rbx survives the
	 * call because the stub saves and restores it — it is one of the
	 * registers the stub borrows to carry the stack arguments in.
	 */
	movq	%rax, %rsi			/* what mach_msg answered */
	movl	MSGH_ID_OFF(%rbx), %edx		/* the identifier, back   */
	movl	MSGH_LOCAL_OFF(%rbx), %r10d	/* the field the KERNEL wrote */
	movq	$SYSCALL_NR_PROBE, %rax
	movq	$IPC_A1, %rdi
	movq	%r14, %r8			/* the port it all went through */
	movq	$IPC_A6, %r9
	syscall

	/*
	 * And again through the SIX-argument trap (#469), which is the one an
	 * ordinary mach_msg() uses now.  Same port, same buffers, one
	 * identifier apart — so the two shapes are compared on the same work
	 * rather than described.
	 *
	 * ⚠️ Six arguments means nothing on the stack at all: the trap number
	 * changes and the three pushes above simply are not there.  Its stub
	 * is `mov %rcx,%r10; mov $0x121,%rax; syscall; ret'.
	 */
	movl	$MSGH_BITS, MSGH_BITS_OFF(%r15)
	movl	$MSGH_SIZE, MSGH_SIZE_OFF(%r15)
	movl	%r14d, MSGH_REMOTE_OFF(%r15)
	movl	$0, MSGH_LOCAL_OFF(%r15)
	movl	$0, MSGH_RESERVED_OFF(%r15)
	movl	$MSG_ID_NARROW, MSGH_ID_OFF(%r15)

	movq	%r15, %rdi
	movq	$(MACH_SEND_MSG|MACH_RCV_MSG), %rsi
	movq	$MSGH_SIZE, %rdx
	movq	$MSG_BUF_SIZE, %rcx
	movq	%r14, %r8
	movq	$0, %r9
	call	urmach_msg

	movq	%rax, %rsi			/* what the narrow trap answered */
	movl	MSGH_ID_OFF(%r15), %edx		/* it replies into the send buffer */
	movl	MSGH_LOCAL_OFF(%r15), %r10d
	movq	$SYSCALL_NR_PROBE, %rax
	movq	$IPC_A1_NARROW, %rdi
	movq	%r14, %r8
	movq	$IPC_A6, %r9
	syscall

	/*
	 * ── And a call that FAILS, on purpose ────────────────────────
	 *
	 * The two above answered zero, which tells us a success comes back
	 * and nothing about an error.  A Mach trap returns kern_return_t,
	 * and kern_return_t is an `int' -- mach_trap_table's function
	 * pointer says so.  On this architecture that means the answer is
	 * in %eax and the TOP HALF OF %rax IS WHATEVER THE CALLEE LEFT
	 * THERE: the ABI does not require a function returning int to
	 * clear it.
	 *
	 * ⚠️ So the whole register is reported, not the low half.  A stub's
	 * C prototype says kern_return_t and a C caller reads %eax and is
	 * safe either way; this asks the other question, the one nobody has
	 * asked yet -- what does ring 3 actually get back in the register?
	 *
	 * ⚠️ There is no poisoning it beforehand: the stub puts the trap
	 * NUMBER in %rax two instructions before the syscall, so anything
	 * written here is gone.  What the answer shows is therefore what the
	 * kernel left, which is the question -- but it cannot distinguish
	 * "the kernel cleared the top half" from "nothing ever set it".
	 *
	 * Sending to MACH_PORT_NULL is the smallest way to make it fail
	 * with a value that is not zero and not small: 0x10000003.  Small
	 * negative numbers are the entry path's own answer for a call that
	 * does not exist (-38), so an error that is neither confirms the
	 * two conventions do not overlap.
	 */
	movl	$MSGH_BITS, MSGH_BITS_OFF(%r15)
	movl	$MSGH_SIZE, MSGH_SIZE_OFF(%r15)
	movl	$0, MSGH_REMOTE_OFF(%r15)	/* MACH_PORT_NULL */
	movl	$0, MSGH_LOCAL_OFF(%r15)
	movl	$0, MSGH_RESERVED_OFF(%r15)
	movl	$MSG_ID, MSGH_ID_OFF(%r15)

	movq	%r15, %rdi
	movq	$MACH_SEND_MSG, %rsi		/* send only: it must not wait */
	movq	$MSGH_SIZE, %rdx
	movq	$0, %rcx
	movq	$0, %r8
	movq	$0, %r9
	call	urmach_msg

	movq	%rax, %rsi			/* the whole 64 bits */
	movq	$0, %rdx
	movq	$0, %r10
	movq	$SYSCALL_NR_PROBE, %rax
	movq	$IPC_A1_ERROR, %rdi
	movq	%r14, %r8
	movq	$IPC_A6, %r9
	syscall

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
