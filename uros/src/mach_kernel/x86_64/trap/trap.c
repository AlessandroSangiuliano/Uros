/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 trap and interrupt entry (#409, MD contract 4/6).
 */

#include <stdint.h>

#include <cpu/desc.h>
#include <cpu/regs.h>
#include <cpu/tss.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <trap/trap.h>

/*
 * A 64-bit gate: sixteen bytes, against the eight an i386 gate takes, with
 * the handler address split across three fields and a slot for an IST
 * index.  That index is the reason this is worth having early — it lets a
 * chosen vector run on a stack of its own instead of whatever stack was
 * current when it fired, which is what makes a double fault or an NMI
 * survivable rather than fortunate.
 */
struct idt_gate {
	uint16_t offset_low;
	uint16_t selector;
	uint8_t  ist;			/* bits 2:0, zero means "current stack" */
	uint8_t  type_attr;
	uint16_t offset_mid;
	uint32_t offset_high;
	uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

#define IDT_PRESENT		0x80
#define IDT_INTERRUPT_GATE	0x0E	/* clears IF on entry; trap gate 0x0F does not */

static struct idt_gate idt[IDT_VECTORS];

/* Filled by trap/entry.S: one stub address per vector. */
extern const uint64_t isr_table[IDT_VECTORS];

/*
 * What claims each vector above the architectural ones.  Sparse by nature —
 * most of the space is unassigned, and an interrupt arriving at one of those
 * is a fact worth reporting rather than ignoring.
 */
static trap_handler_t ext_handler[IDT_VECTORS - T_EXTERNAL_FIRST];

void trap_set_handler(unsigned vector, trap_handler_t handler)
{
	if (vector < T_EXTERNAL_FIRST || vector >= IDT_VECTORS)
		panic("trap: only the vectors above the exceptions can be claimed");

	ext_handler[vector - T_EXTERNAL_FIRST] = handler;
}

static void idt_set(unsigned vector, uint64_t handler, unsigned ist)
{
	struct idt_gate *g = &idt[vector];

	g->offset_low = (uint16_t)handler;
	g->offset_mid = (uint16_t)(handler >> 16);
	g->offset_high = (uint32_t)(handler >> 32);
	g->selector = KERNEL_CS_SELECTOR;
	g->ist = ist & 0x7;
	g->type_attr = IDT_PRESENT | IDT_INTERRUPT_GATE;
	g->reserved = 0;
}

/*
 * A stack per interrupt-stack-table slot, and never a shared one: an NMI
 * arriving while a double fault is being handled would otherwise land on
 * the stack that fault is using and overwrite the report being written.
 *
 * The top of a stack is the bottom of the next, which is what makes one
 * block of consecutive stacks the natural thing for a caller to hand over.
 */
void trap_ist_setup(struct tss64 *tss, uint64_t block, uint64_t stack_size)
{
	for (unsigned i = 0; i < IST_COUNT; i++)
		tss->ist[i] = block + (i + 1) * stack_size;
}

/* Which vectors run on a stack of their own, and which one. */
static unsigned vector_ist(unsigned vector)
{
	switch (vector) {
	case T_DOUBLE_FAULT:	return IST_DOUBLE_FAULT;
	case T_NMI:		return IST_NMI;
	case T_MACHINE_CHECK:	return IST_MACHINE_CHECK;
	default:		return 0;	/* the current stack will do */
	}
}

void trap_init(void)
{
	struct idt_ptr p = { sizeof(idt) - 1, (uint64_t)(uintptr_t)idt };

	/*
	 * Every vector gets a stub, including the ones the architecture
	 * reserves.  An unassigned vector would fault into nothing and take
	 * the machine down without a word — precisely the failure this is
	 * here to end.
	 */
	for (unsigned v = 0; v < IDT_VECTORS; v++)
		idt_set(v, isr_table[v], vector_ist(v));

	__asm__ volatile("lidt %0" : : "m"(p) : "memory");
}

/* ------------------------------------------------------------------ */
/*  Reporting                                                           */
/* ------------------------------------------------------------------ */
#define COM1 0x3F8

static void tputc(char c)
{
	while (!(inb(COM1 + 5) & 0x20))
		;
	outb(COM1, (uint8_t)c);
}

static void tputs(const char *s)
{
	for (; *s; s++)
		tputc(*s);
}

static void tputhex(uint64_t v)
{
	tputs("0x");
	for (int i = 60; i >= 0; i -= 4) {
		uint8_t nib = (v >> i) & 0xF;
		tputc(nib < 10 ? '0' + nib : 'a' + (nib - 10));
	}
}

const char *trap_name(uint64_t vector)
{
	switch (vector) {
	case T_DIVIDE_ERROR:		return "divide error";
	case T_DEBUG:			return "debug";
	case T_NMI:			return "non-maskable interrupt";
	case T_BREAKPOINT:		return "breakpoint";
	case T_OVERFLOW:		return "overflow";
	case T_BOUND_RANGE:		return "bound range exceeded";
	case T_INVALID_OPCODE:		return "invalid opcode";
	case T_NO_FPU:			return "device not available";
	case T_DOUBLE_FAULT:		return "double fault";
	case T_INVALID_TSS:		return "invalid TSS";
	case T_SEGMENT_NOT_PRESENT:	return "segment not present";
	case T_STACK_FAULT:		return "stack fault";
	case T_GENERAL_PROTECTION:	return "general protection";
	case T_PAGE_FAULT:		return "page fault";
	case T_FPU_ERROR:		return "x87 floating-point error";
	case T_ALIGNMENT_CHECK:		return "alignment check";
	case T_MACHINE_CHECK:		return "machine check";
	case T_SIMD_ERROR:		return "SIMD floating-point error";
	default:
		return vector >= T_EXTERNAL_FIRST ? "unclaimed interrupt"
						  : "reserved vector";
	}
}

/*
 * A page fault's error code describes the access that was refused, not the
 * mapping that refused it — so the same address can fault for opposite
 * reasons, and saying which is most of the diagnosis.
 */
static void report_page_fault(uint64_t error)
{
	tputs("  ");
	tputs(error & PF_INSTRUCTION ? "instruction fetch"
	      : error & PF_WRITE     ? "write" : "read");
	tputs(error & PF_USER ? " from user" : " from kernel");
	tputs(error & PF_PRESENT ? ", refused by the mapping"
				 : ", nothing mapped there");
	if (error & PF_RESERVED)
		tputs(", reserved bit set in an entry");
	tputs("\r\n  faulting address ");
	tputhex(read_cr2());
	tputs("\r\n");
}

/*
 * Walk the frame chain and print the return addresses.
 *
 * This runs in the worst context the kernel has — after a fault, sometimes
 * because the stack itself is wrong — so it trusts nothing it is about to
 * read.  Every frame is checked for alignment, for being a canonical
 * address, and for being mapped at all, the last through the page tables
 * rather than by trying it and seeing.  A backtrace that faults while
 * explaining a fault would replace the diagnosis with a double fault.
 *
 * Frames are also required to move upward: a stack grows down, so a caller's
 * frame is always at a higher address, and anything else is not a chain but
 * whatever happened to be in the register.
 *
 * Addresses are absolute, and there is no symbol table on this target yet
 * (#211 built one for i386): resolve them with addr2line against the ELF.
 *
 * ⚠️ An optimised build has fewer frames than the source suggests, because an
 * inlined function leaves none — the trace is still complete, it is the
 * function that is gone.  A short trace is worth resolving before it is
 * worth suspecting.
 */
#define BACKTRACE_MAX	16

static void backtrace(uint64_t rbp)
{
	pmap_t kernel = pmap_kernel();

	tputs("  backtrace (resolve with addr2line):\r\n");

	for (unsigned depth = 0; depth < BACKTRACE_MAX; depth++) {
		const uint64_t *frame;
		uint64_t next, ret;

		if ((rbp & 7) != 0 || !va_is_canonical(rbp))
			break;

		/* Both words of the frame have to be there before either is read. */
		if (pmap_extract(kernel, rbp) == 0
		    || pmap_extract(kernel, rbp + 8) == 0)
			break;

		frame = (const uint64_t *)(uintptr_t)rbp;
		next = frame[0];
		ret = frame[1];

		if (ret == 0)
			break;

		tputs("    ");
		tputhex(ret);
		tputs("\r\n");

		if (next <= rbp)
			break;
		rbp = next;
	}
}

void panic(const char *what)
{
	tputs("\r\nUrMach x86-64: panic: ");
	tputs(what);
	tputs("\r\n");

	/*
	 * The caller's frame, not this one: what matters is who could not
	 * continue, and the walk climbs from there.
	 */
	backtrace((uint64_t)(uintptr_t)__builtin_frame_address(0));

	for (;;)
		__asm__ volatile("cli; hlt");
}

/*
 * One armed expectation.  Not a stack: a fault while recovering from a
 * fault is not something to paper over, and leaving the second one to be
 * reported and halted on is the honest outcome.
 */
static uint64_t expect_vector;
static uint64_t expect_resume;
static int expect_armed;

static struct trap_record last_trap;

void trap_expect(uint64_t vector, uint64_t resume_rip)
{
	expect_vector = vector;
	expect_resume = resume_rip;
	expect_armed = 1;
	last_trap.caught = 0;
}

/* The same idea for the faults that arrive from ring 3; see <trap/trap.h>. */
static uint64_t user_resume_rip;
static uint64_t user_resume_rsp;
static int user_expect_armed;

void trap_expect_user(uint64_t resume_rip, uint64_t resume_rsp)
{
	user_resume_rip = resume_rip;
	user_resume_rsp = resume_rsp;
	user_expect_armed = 1;
	last_trap.caught = 0;
}

const struct trap_record *trap_last(void)
{
	return &last_trap;
}

/* The other record, for the four vectors that take the paranoid entry. */
static struct trap_paranoid_record last_paranoid;

const struct trap_paranoid_record *trap_last_paranoid(void)
{
	return &last_paranoid;
}

void trap_paranoid_forget(void)
{
	last_paranoid.taken = 0;
}

void trap_dispatch_paranoid(struct trap_frame *frame, uint64_t gs_on_entry,
			    uint64_t swapped)
{
	/*
	 * Recorded before the handler runs, because the handler is entitled to
	 * halt: a machine check that reports and stops should still have left
	 * behind the evidence that its entry got %gs right, and a record
	 * written afterwards would only exist for the traps that were survived.
	 *
	 * gs_on_dispatch is read from the machine rather than inferred from the
	 * other two.  Inferring it would be checking the entry's arithmetic
	 * against itself; reading it asks whether the instruction actually ran.
	 */
	last_paranoid.vector = frame->vector;
	last_paranoid.cs = frame->cs;
	last_paranoid.gs_on_entry = gs_on_entry;
	last_paranoid.gs_on_dispatch = rdmsr(MSR_GS_BASE);
	last_paranoid.swapped = swapped;
	last_paranoid.taken = 1;

	trap_dispatch(frame);
}

void trap_dispatch(struct trap_frame *frame)
{
	/*
	 * An interrupt somebody asked for, before any of the fault machinery:
	 * it is not a fault, it has no error code to report, and the expected-
	 * trap arrangement below is about instructions that failed, which this
	 * is not.
	 */
	if (frame->vector >= T_EXTERNAL_FIRST) {
		trap_handler_t h = ext_handler[frame->vector - T_EXTERNAL_FIRST];

		if (h != 0) {
			h(frame);
			return;
		}
		/* Nobody claimed it — fall through and say so. */
	}

	/*
	 * A fault from ring 3, which is recoverable because of where it came
	 * from rather than what it was.  Checked before the vector-matching
	 * arrangement below, since that one resumes on the current stack and
	 * this frame's stack belongs to a user program.
	 */
	if (user_expect_armed && (frame->cs & 3) == USER_RPL) {
		user_expect_armed = 0;

		last_trap.vector = frame->vector;
		last_trap.error = frame->error;
		last_trap.rip = frame->rip;
		last_trap.cr2 = read_cr2();
		last_trap.cs = frame->cs;
		last_trap.gs_base = rdmsr(MSR_GS_BASE);
		last_trap.caught = 1;

		/*
		 * Every field, not just the instruction pointer: IRETQ in long
		 * mode reloads the stack segment and stack pointer whatever the
		 * privilege change, so leaving the user's there would return to
		 * kernel code standing on a user stack.
		 *
		 * Flags carry interrupts enabled back with them. Returning with
		 * them off would leave the rest of the boot unable to take a
		 * cross-call — a machine that stops answering, from a line that
		 * looks like housekeeping.
		 */
		frame->rip = user_resume_rip;
		frame->rsp = user_resume_rsp;
		frame->cs = KERNEL_CS_SELECTOR;
		frame->ss = KERNEL_DS_SELECTOR;
		frame->rflags = 0x202;
		return;
	}

	if (expect_armed && frame->vector == expect_vector) {
		/*
		 * Disarm first.  If resuming faults again the expectation is
		 * already spent, so the second one is reported rather than
		 * looping through this same path forever.
		 */
		expect_armed = 0;

		last_trap.vector = frame->vector;
		last_trap.error = frame->error;
		last_trap.rip = frame->rip;
		last_trap.cr2 = read_cr2();
		last_trap.cs = frame->cs;
		last_trap.caught = 1;

		tputs("UrMach x86-64: expected ");
		tputs(trap_name(frame->vector));
		tputs(" (error ");
		tputhex(frame->error);
		if (frame->vector == T_PAGE_FAULT) {
			tputs(", at ");
			tputhex(read_cr2());
			tputs(frame->error & PF_PRESENT
			      ? ", refused by the mapping"
			      : ", nothing mapped there");
		}
		tputs(") — resuming\r\n");

		/*
		 * The frame is what iretq will reload, so changing the saved
		 * instruction pointer changes where the return lands.  The
		 * stack pointer is untouched, which is why the resume point
		 * has to be inside the function that armed this: its frame is
		 * still exactly as the faulting instruction left it.
		 *
		 * Unless the caller asked to carry on where it was, which for
		 * a vector that is not the instruction's fault is the only
		 * sensible answer — see TRAP_RESUME_HERE in <trap/trap.h>.
		 */
		if (expect_resume != TRAP_RESUME_HERE)
			frame->rip = expect_resume;
		return;
	}

	tputs("\r\nUrMach x86-64: trap ");
	tputs(trap_name(frame->vector));
	tputs(" (vector ");
	tputhex(frame->vector);
	tputs(", error ");
	tputhex(frame->error);
	tputs(")\r\n");

	if (frame->vector == T_PAGE_FAULT)
		report_page_fault(frame->error);

	/*
	 * For a vector with a stack of its own, say so and show it: the frame
	 * sits on the interrupt stack the CPU switched to, while the saved
	 * stack pointer is the one the interrupted code was using.  Two
	 * different values are the switch having happened — and for a double
	 * fault, the reason there is a report at all.
	 */
	if (vector_ist(frame->vector)) {
		tputs("  on interrupt stack ");
		tputhex((uint64_t)(uintptr_t)frame);
		tputs(", interrupted stack was ");
		tputhex(frame->rsp);
		tputs("\r\n");
	}

	tputs("  rip ");
	tputhex(frame->rip);
	tputs("  rsp ");
	tputhex(frame->rsp);
	tputs("  rflags ");
	tputhex(frame->rflags);
	tputs("\r\n  rax ");
	tputhex(frame->rax);
	tputs("  rbx ");
	tputhex(frame->rbx);
	tputs("  rcx ");
	tputhex(frame->rcx);
	tputs("\r\n  rdx ");
	tputhex(frame->rdx);
	tputs("  rsi ");
	tputhex(frame->rsi);
	tputs("  rdi ");
	tputhex(frame->rdi);
	tputs("\r\n");

	backtrace(frame->rbp);

	/*
	 * Nothing recovers yet: there is no MI exception path to hand this
	 * to, and pretending to resume would turn one diagnosis into an
	 * endless stream of them.  Stopping here leaves the report as the
	 * last thing on the wire, which is what it is for.
	 */
	tputs("UrMach x86-64: no handler — halted\r\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}
