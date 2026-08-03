/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 trap and interrupt entry (#409, MD contract 4/6).
 */

#include <stdarg.h>
#include <stdint.h>

#include <cpu/desc.h>
#include <ddb/cons.h>
#include <cpu/lapic.h>
#include <cpu/regs.h>
#include <cpu/spl.h>
#include <ddb/ddb.h>
#include <ddb/ksym.h>
#include <cpu/tss.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <trap/extable.h>
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

void trap_replay_vector(unsigned vector)
{
	trap_handler_t h;

	if (vector < T_EXTERNAL_FIRST || vector >= IDT_VECTORS)
		return;

	h = ext_handler[vector - T_EXTERNAL_FIRST];
	if (h == 0)
		return;

	/*
	 * Only the vector, and the rest left at zero rather than filled with
	 * something that would read as a context. See <trap/trap.h>: this is a
	 * contract on what a deferrable handler may look at.
	 */
	struct trap_frame replay = { 0 };

	replay.vector = vector;
	h(&replay);
}

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
	case T_DEBUG:		return IST_DEBUG;
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
 * One register, named and padded, three to a line.
 *
 * Every one of them, which needs saying because the previous version of this
 * printed nine of sixteen — the eight the 32-bit report had, plus one. On an
 * architecture whose principal gain is having fifteen general registers
 * instead of six, a fault report that shows half of them is throwing away
 * the thing that was gained. The ones left out were r8 to r15, which is
 * exactly where a compiler puts the values it is working with.
 */
static void reg(const char *name, uint64_t value, int newline)
{
	tputs(newline ? "\r\n  " : "  ");
	tputs(name);
	tputs(" ");
	tputhex(value);
}

static void report_registers(const struct trap_frame *frame)
{
	reg("rip   ", frame->rip, 1);
	reg("rsp   ", frame->rsp, 0);
	reg("rflags", frame->rflags, 0);

	reg("rax   ", frame->rax, 1);
	reg("rbx   ", frame->rbx, 0);
	reg("rcx   ", frame->rcx, 0);
	reg("rdx   ", frame->rdx, 1);
	reg("rsi   ", frame->rsi, 0);
	reg("rdi   ", frame->rdi, 0);
	reg("rbp   ", frame->rbp, 1);
	reg("r8    ", frame->r8, 0);
	reg("r9    ", frame->r9, 0);
	reg("r10   ", frame->r10, 1);
	reg("r11   ", frame->r11, 0);
	reg("r12   ", frame->r12, 0);
	reg("r13   ", frame->r13, 1);
	reg("r14   ", frame->r14, 0);
	reg("r15   ", frame->r15, 0);

	/*
	 * The segments and the page-table root, which are not general
	 * registers and are not decoration either: the low two bits of cs are
	 * the ring the fault came from, and cr3 says which address space the
	 * addresses above are to be read in — without it, an address in a
	 * report is ambiguous the moment there is more than one space.
	 */
	reg("cs    ", frame->cs, 1);
	reg("ss    ", frame->ss, 0);
	reg("cr3   ", read_cr3(), 0);
	tputs("\r\n");
}

/*
 * The bytes at the faulting instruction, as bytes.
 *
 * ⚠️ Not disassembled, and that is a decision rather than a gap. x86-64 adds
 * REX prefixes, changes the meaning of several opcodes and extends the
 * addressing forms, so a decoder carried over from the 32-bit tree would
 * print something confident and wrong — which is worse than printing nothing
 * at all, because a wrong mnemonic is acted upon. The bytes are unambiguous
 * and any disassembler will take them.
 *
 * Read through the page tables first. A fault report that faults while
 * explaining a fault replaces the diagnosis with a double fault, and an
 * instruction pointer is exactly the field most likely to be wrong when
 * there is something to report.
 */
#define INSTRUCTION_BYTES	16

static void report_instruction(uint64_t rip)
{
	pmap_t kernel = pmap_kernel();
	const uint8_t *p = (const uint8_t *)(uintptr_t)rip;

	if (!va_is_canonical(rip) || pmap_extract(kernel, rip) == 0)
		return;

	tputs("  bytes at rip:");
	for (unsigned i = 0; i < INSTRUCTION_BYTES; i++) {
		uint8_t b;

		/* Each byte checked, because sixteen of them can cross into a
		 * page that is not there. */
		if (pmap_extract(kernel, rip + i) == 0)
			break;

		b = p[i];
		tputs(" ");
		tputc("0123456789abcdef"[b >> 4]);
		tputc("0123456789abcdef"[b & 0xF]);
	}
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

/*
 * The name of whatever contains an address, when there is one.
 *
 * Silent when there is not, rather than printing "unknown": the address is
 * already on the line, and a word that means "no answer" costs a column on
 * every frame to say what the absence of a name already says.
 */
static void report_symbol(uint64_t addr)
{
	uint64_t off = 0;
	const char *name = ksym_lookup(addr, &off);

	if (name == 0)
		return;

	tputs(" <");
	tputs(name);
	if (off != 0) {
		tputs("+");
		tputhex(off);
	}
	tputs(">");
}

void x86_64_backtrace(uint64_t rbp)
{
	pmap_t kernel = pmap_kernel();

	tputs("  backtrace (addr2line for file and line):\r\n");

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
		report_symbol(ret);
		tputs("\r\n");

		if (next <= rbp)
			break;
		rbp = next;
	}
}

/*
 * ⚠️ panic() is deliberately NOT here, and it used to be.
 *
 * It was written during bring-up, when there was no machine-independent
 * kernel and something had to stop the machine and say why.  kern/debug.c has
 * the real one, and it does what a bring-up panic cannot: it takes a lock so
 * two processors panicking at once produce one legible report rather than two
 * interleaved ones, it names the processor that died first, and it recognises
 * a second panic instead of recursing into it.
 *
 * What was lost with this one is the symbolising backtrace, and that is not
 * lost: halt_cpu() in x86_64/cpu/model.c prints it, which is the machine's
 * last call on the panic path and the point where it is still true.  The
 * walker above is exported for it (#453).
 */

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
	last_paranoid.rip = frame->rip;
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
			/*
			 * The priority level first. A vector whose class is at
			 * or below it is noted and left for later — the
			 * acknowledgement below still happens, because the
			 * local APIC would otherwise hold that class busy and
			 * the deferral would become a silence.
			 */
			if (spl_defer(frame->vector)) {
				lapic_eoi();
				return;
			}

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

	/*
	 * An instruction that was allowed to fault (#453).
	 *
	 * Checked before the armed expectation and before anything is
	 * reported, because this is not an exceptional condition: copyin() and
	 * copyout() reaching an address the task no longer has is ordinary,
	 * and the kernel's answer is an error return, not a diagnostic.
	 *
	 * ⚠️ Only for a fault taken in the kernel.  A fault from ring 3 whose
	 * rip happened to equal a kernel address in the table would otherwise
	 * be "recovered" into kernel code with a user context around it, which
	 * is a privilege escalation rather than a recovery.
	 */
	if ((frame->cs & 3) == 0) {
		uint64_t resume = ex_table_lookup(frame->rip);

		if (resume != 0) {
			frame->rip = resume;
			return;
		}
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
		if (expect_resume != TRAP_RESUME_HERE) {
			frame->rip = expect_resume;
			return;
		}

		/*
		 * Resuming where an instruction breakpoint fired means running
		 * into it again, because the exception is reported before the
		 * instruction executes and returning puts it back in front of
		 * the same one.  The resume flag is the architecture's answer:
		 * it suppresses instruction breakpoints for one instruction and
		 * the processor clears it afterwards.
		 *
		 * Set here rather than assumed.  The exception arrives with it
		 * clear on the emulator this is developed on — measured, rflags
		 * 0x2 — and a return that trusted the processor to have set it
		 * walked straight back into the breakpoint and stayed there.
		 * Setting a bit that is already set costs nothing, so this is
		 * right on a machine that sets it too.
		 */
		if (frame->vector == T_DEBUG)
			frame->rflags |= RFLAGS_RF;
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

	report_registers(frame);
	report_instruction(frame->rip);

	x86_64_backtrace(frame->rbp);

	/*
	 * And, if the debugger was asked for, the questions the report cannot
	 * answer in advance: what is at that address, which function is above
	 * this one, what does the memory the pointer came from look like.
	 *
	 * Only when asked. Without `-r` this is not reached, and an unattended
	 * boot ends in the halt below rather than at a prompt nobody is there
	 * to answer — which is what keeps every automated run terminating.
	 */
	if (ddb_enabled())
		ddb_enter(frame, trap_name(frame->vector));

	/*
	 * Nothing recovers yet: there is no MI exception path to hand this
	 * to, and pretending to resume would turn one diagnosis into an
	 * endless stream of them.  Stopping here leaves the report as the
	 * last thing on the wire, which is what it is for.
	 *
	 * Reached also when the debugger returns: continuing from a fault
	 * nobody has fixed means taking it again, so "continue" ends here too.
	 */
	tputs("UrMach x86-64: no handler — halted\r\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}

/*
 * ── The fault-recovery table (#453) ───────────────────────────────────
 *
 * Filled by EX_TABLE() at each instruction that is allowed to fault, and
 * bracketed by the linker.  See <trap/extable.h> for why this is a table and
 * not the single armed expectation above it.
 */
extern const struct ex_entry	__ex_table_start[], __ex_table_end[];

uint64_t ex_table_lookup(uint64_t rip)
{
	const struct ex_entry	*e;

	for (e = __ex_table_start; e < __ex_table_end; e++)
		if (e->fault_rip == rip)
			return e->resume_rip;

	return 0;
}
