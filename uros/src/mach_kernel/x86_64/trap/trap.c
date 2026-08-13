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
#include <ddb/disasm.h>
#include <cpu/lapic.h>
#include <kern/ast.h>		/* #459: need_ast, ast_taken */
#include <kern/thread.h>		/* #467: current_act */
#include <kern/thread_act.h>
#include <vm/vm_fault.h>		/* #467: hand a fault to the MI kernel */
#include <kern/exception.h>		/* #467: exception() */
#include <mach/exception.h>		/* #467: EXC_BAD_ACCESS and friends */
#include <mach/machine/exception.h>	/* #467: EXC_X86_64_*, first consumer */
#include <vm/vm_kern.h>		/* #467: kernel_map */
#include <mach/vm_param.h>		/* #467: trunc_page */
#include <kern/cpu_number.h>
#include <kern/cpu_data.h>	/* #459: get_preemption_level */
#include <cpu/percpu.h>	/* #461: the spin-lock depth */
#include <cpu/regs.h>
#include <cpu/spl.h>
#include <ddb/ddb.h>
#include <ddb/ksym.h>
#include <cpu/tss.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <sync/atomic.h>	/* #461: one backtrace at a time */
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

static void tputdec(uint64_t v)
{
	char	buf[20];
	int	i = 0;

	if (v == 0) {
		tputc('0');
		return;
	}
	while (v != 0 && i < (int) sizeof(buf)) {
		buf[i++] = '0' + (char)(v % 10);
		v /= 10;
	}
	while (i-- > 0)
		tputc(buf[i]);
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
 * And the other error code, which describes a DESCRIPTOR (#409).
 *
 * Six vectors share this format — the segment faults and the general
 * protection — and every one of them was reported as a bare hexadecimal
 * number.  #458 said so while chasing a general protection whose register dump
 * could not be reconciled: "the one thing the handler could say about *why* is
 * missing on the path where the registers are least trustworthy".
 *
 * ⚠️ Zero is not a selector and must not be printed as one.  For these vectors
 * it means the fault had nothing to do with a descriptor at all — an
 * instruction that is illegal in the current mode, a non-canonical address, a
 * write to a reserved bit — which is a different diagnosis and by far the
 * commonest one in a kernel that loads a segment register perhaps five times
 * in its life.  A report that answered "GDT entry 0" there would send the
 * reader to the descriptor tables, which is exactly the wrong place.
 *
 * The layout is: bit 0 says the event came from outside the program, bits 2:1
 * name the table, and the rest is the index — the selector with its low three
 * bits repurposed, which is why the index is what gets printed rather than the
 * raw value that looks like a selector and is not one.
 */
static int carries_a_selector(uint64_t vector)
{
	return vector == T_INVALID_TSS || vector == T_SEGMENT_NOT_PRESENT
	    || vector == T_STACK_FAULT || vector == T_GENERAL_PROTECTION
	    || vector == T_ALIGNMENT_CHECK;
}

static void report_selector_error(uint64_t error)
{
	static const char *const table[4] = {
		"the GDT", "the IDT", "the LDT", "the IDT"
	};

	if (error == 0) {
		tputs("  no descriptor involved — an operation the mode or the "
		      "address did not allow\r\n");
		return;
	}

	tputs("  refused by ");
	tputs(table[(error >> 1) & 3]);
	tputs(" entry ");
	tputdec(error >> 3);
	if (error & 1)
		tputs(", raised by an event outside the program");
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

	/*
	 * And whether that is the KERNEL's cr3, which for a fault from ring 3
	 * is the whole diagnosis (#422).
	 *
	 * A user address is in every space; what differs is what it maps to.  A
	 * report that prints cr3 alone leaves the reader to know by heart which
	 * root belongs to whom — and the failure this was added for looked like
	 * a permission problem: an instruction fetch refused at a mapped page,
	 * which is exactly what the low identity mapping left over from early
	 * boot answers when a user thread is running on the kernel's tables.
	 */
	if (read_cr3() == pmap_kernel()->root_pa)
		tputs("  (the kernel's own tables)");
	tputs("\r\n");
}

/*
 * The faulting instruction: named if it can be, and its bytes either way.
 *
 * ⚠️ It used to be bytes alone, with the reason written here that a decoder
 * carried over from the 32-bit tree would print something confident and wrong
 * — worse than nothing, because a wrong mnemonic is acted upon.  That reason
 * was right and is why <ddb/disasm.h> refuses instead of guessing, and why it
 * is measured: scripts/disasm-coverage.sh runs it over all 149,229
 * instructions of this kernel's own .text against objdump, and the contract
 * is zero disagreements on length and on every name it pronounces.  It
 * declines about one in eighteen and prints `?' for those.
 *
 * The bytes stay regardless.  They are what an external disassembler takes,
 * and they are the answer when ours has none.
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
	uint8_t bytes[INSTRUCTION_BYTES];
	unsigned avail = 0;

	/*
	 * ⚠️ Kernel addresses only, and that is not caution (#422).
	 *
	 * The check below asks the KERNEL's page tables whether the address is
	 * mapped, and the read that follows goes through whatever CR3 is
	 * loaded.  For a fault from ring 3 those are two different address
	 * spaces -- and the kernel's tables still carry the low identity
	 * mapping left over from early boot, so a user rip of 0x401000 passed
	 * the check and then faulted on the read, in the kernel, while
	 * reporting a fault.  One diagnosis replaced by a second one about the
	 * reporter.
	 *
	 * Reading it properly means walking the faulting task's pmap, which is
	 * worth doing and is not this: the bytes at a user rip belong with the
	 * rest of what a user fault should report, and there is no path for a
	 * user fault to report anything yet (#467).
	 */
	if (!va_is_kernel(rip)) {
		tputs("  (no bytes: the instruction is in a user address space, "
		      "which this report cannot read yet -- #467)\r\n");
		return;
	}

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
		bytes[avail++] = b;
		tputs(" ");
		tputc("0123456789abcdef"[b >> 4]);
		tputc("0123456789abcdef"[b & 0xF]);
	}
	tputs("\r\n");

	/*
	 * ⚠️ Decoded from the copy that was just checked, never from `p'
	 * again.  Re-reading through the pointer would be a second trip
	 * through memory the first trip proved nothing about -- the mapping
	 * can have gone in between, and this is the one function that must
	 * not fault.
	 */
	if (avail != 0) {
		char text[32];

		if (disasm(bytes, avail, rip, text, sizeof text) != 0) {
			tputs("  instruction: ");
			tputs(text);
			tputs("\r\n");
		}
	}
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
static void report_named(const char *name, uint64_t off)
{
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

/*
 * ⚠️ The lookup on its own line, and not inside the call below it.
 *
 * `report_named(ksym_lookup(addr, &off), off)' reads `off' and calls the
 * function that fills it in as two arguments of one call, and C does not
 * sequence those against each other: the compiler is entitled to read the
 * variable first, and this one does.  Every offset in every backtrace came out
 * as zero — names right, positions gone — and the names being right is what
 * made it look fine.  Written twice here and once in the debugger before the
 * output was compared against the previous boot's.
 */
static void report_symbol(uint64_t addr)
{
	uint64_t off = 0;
	const char *name = ksym_lookup(addr, &off);

	report_named(name, off);
}

/*
 * For an address taken off a stack: it is one past a call, which at the end of
 * a function belongs to the next one (#409).  See ksym_lookup_call().
 */
static void report_return_symbol(uint64_t ret)
{
	uint64_t off = 0;
	const char *name = ksym_lookup_call(ret, &off);

	report_named(name, off);
}

/*
 * One report at a time, and one that says whose it is (#461).
 *
 * ⚠️ THIS WROTE STRAIGHT TO THE PORT, AND FOR AS LONG AS ONE PROCESSOR EVER
 * DIED THAT WAS FINE.  The first boot with the application processors in the
 * scheduler had three of them fail the same way at the same instant, and the
 * three reports came out interleaved character by character: not one line of
 * the panic message survived, the harness's `^panic' pattern matched nothing,
 * and the run was reported as passed.  The one message worth having is the
 * one that gets destroyed, because everything is failing at once precisely
 * when several processors fail at once.
 *
 * The comment above panic_bringup() says kern/debug.c's panic() takes a lock
 * so that two processors panicking together produce one legible report -- and
 * then hands the backtrace to this, which took none.  Half of the guarantee,
 * which is the expensive half.
 *
 * The lock is bounded and released before the caller halts.  A processor that
 * faults while holding it must not take the others' last words down with it,
 * so the wait gives up and prints anyway: interleaved output is worse than
 * serialised output and far better than none.
 *
 * ⚠️ The label is asked of the interrupt controller, not of cpu_number().
 * cpu_number() reads this processor's per-CPU block through %gs, and before
 * percpu_activate() the segment base is zero -- which early in boot is mapped,
 * so it answers a small plausible number rather than faulting.  A backtrace
 * labelled with a processor that is not the one that died is worse than one
 * with no label at all.
 */
static volatile uint64_t backtrace_lock;

void x86_64_backtrace(uint64_t rbp)
{
	pmap_t kernel = pmap_kernel();
	uint64_t spins;

	for (spins = 0; spins < 200000000ULL; spins++) {
		if (atomic_cmpxchg64(&backtrace_lock, 0, 1) == 0)
			break;
		cpu_pause();
	}

	tputs("  cpu ");
	if (lapic_present())
		tputdec(lapic_id());
	else
		tputc('?');
	tputs(" backtrace (addr2line for file and line):\r\n");

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
		report_return_symbol(ret);
		tputs("\r\n");

		if (next <= rbp)
			break;
		rbp = next;
	}

	atomic_store64(&backtrace_lock, 0);
}

/*
 * The same walk, answered instead of printed (#409).
 *
 * The issue asks for a correct backtrace from each of the six entries, and a
 * backtrace is only correct if something checks it.  Printing six of them into
 * the boot log and letting a reader form an impression is what #451 removed
 * from the harness — 130 self-tests with no way to fail.
 *
 * So this walks the chain exactly as the printer above and the debugger's `t'
 * do, and answers three things: how deep it got, what sits immediately above
 * the interrupted code, and whether a named function was reached.  The last is
 * what separates a chain that is whole from one that is merely non-empty — a
 * walk that breaks in the middle still returns frames, and they still have
 * names, and nothing about them says the bottom is missing.
 *
 * ⚠️ The first name is the CALLER, not the interrupted function.  A frame
 * pointer names where its own function will return to, so the function that
 * was executing is described by the saved rip and never appears in the chain.
 * A test asking for it here would be asking for something that is not there.
 *
 * ⚠️ And the caller is asked for by NAME rather than compared against one
 * chosen in advance, because a static function called once is a function the
 * compiler is entitled to inline — the frame a test predicted would then
 * simply not exist, and the test would be measuring the optimiser.
 */
static int name_matches(const char *a, const char *b)
{
	if (a == 0 || b == 0)
		return 0;

	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}

	return *a == *b;
}

unsigned x86_64_backtrace_probe(uint64_t rbp, const char **first,
				const char *want, int *found)
{
	pmap_t kernel = pmap_kernel();
	unsigned depth, walked = 0;

	if (first != 0)
		*first = 0;
	if (found != 0)
		*found = 0;

	for (depth = 0; depth < BACKTRACE_MAX; depth++) {
		const uint64_t *f;
		uint64_t next, ret;
		const char *name;

		if ((rbp & 7) != 0 || !va_is_canonical(rbp))
			break;
		if (pmap_extract(kernel, rbp) == 0
		    || pmap_extract(kernel, rbp + 8) == 0)
			break;

		f = (const uint64_t *)(uintptr_t)rbp;
		next = f[0];
		ret = f[1];

		if (ret == 0)
			break;

		name = ksym_lookup_call(ret, 0);
		if (depth == 0 && first != 0)
			*first = name;
		if (found != 0 && name_matches(name, want))
			*found = 1;

		/*
		 * ⚠️ Counted after the frame is accepted, not by the loop
		 * variable.  The loop leaves as soon as the chain ends, so its
		 * counter is one short of what was reported — which printed
		 * "0 frames from syscall_entry", a line that names a frame and
		 * denies it in the same breath.
		 */
		walked++;

		if (next <= rbp)
			break;
		rbp = next;
	}

	return walked;
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

/*
 * ⚠️ One place that fills the record, because there were two and they differed.
 *
 * The ring-3 arm recorded the segment base %gs arrived with and the kernel arm
 * did not, so that field read as "the base was zero" after a kernel fault
 * instead of "this path does not know" — the same field meaning two things
 * depending on which arm ran, which <trap/trap.h> says about the paranoid
 * record is worse than having no field at all.
 *
 * Nothing had noticed because only the ring-3 test read it.  A second reader
 * is all it would have taken, and adding two more fields (#409) is exactly
 * that second reader.
 */
static void trap_record_frame(const struct trap_frame *frame)
{
	last_trap.vector = frame->vector;
	last_trap.error = frame->error;
	last_trap.rip = frame->rip;
	last_trap.cr2 = read_cr2();
	last_trap.cs = frame->cs;
	last_trap.gs_base = rdmsr(MSR_GS_BASE);
	last_trap.rsp = frame->rsp;
	last_trap.frame = (uint64_t)(uintptr_t)frame;
	last_trap.rbp = frame->rbp;
	last_trap.caught = 1;
}

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

/*
 * Turn a pending AST into a context switch, on the way out of an interrupt
 * (#459).
 *
 * hertz_tick() decrementing a quantum and raising AST_QUANTUM is only half of
 * preemption: something has to look at what was raised.  Nothing did -- the
 * word `need_ast' did not appear in this file or in entry.S -- so the clock
 * ticked, the quantum expired, and the processor carried on running the same
 * thread forever.  Measured before this existed: three threads bound to one
 * processor, the first one scheduled ran for a full second and the other two
 * counters stayed at zero.
 *
 * In C rather than in the return path in assembly, unlike i386, which checks
 * need_ast in three places in locore.S.  There is one return path here and it
 * already calls into C with the frame in hand, so a check written here is
 * read by anyone reading the dispatch -- and a context switch that happens
 * from a C frame is easier to reason about than one that happens between two
 * instructions of an epilogue.
 *
 * ⚠️ Only on the way back to a preemptable context.  Switching away from a
 * kernel path that is holding a lock is not preemption, it is a deadlock with
 * extra steps: the level is raised precisely so this cannot happen, and
 * ast_taken() is entitled to assume the caller is at a point where blocking
 * is legal.
 */
/*
 * What a return to RING 0 is entitled to take (#463).
 *
 * Not everything.  ast_taken() dispatches several kinds of work, and they do
 * not all belong at an arbitrary instruction boundary inside the kernel:
 *
 *   AST_BLOCK, AST_QUANTUM, AST_URGENT are about which thread owns this
 *   processor.  Their handler is thread_block_reason(), they are the whole
 *   point of preemption, and they are legal anywhere the preemption level
 *   says so -- which is the test below.
 *
 *   AST_APC is a RETURN-TO-USER hook, and kern/thread_act.c says so in as
 *   many words: nudge() exists to "ensure that the activation will execute
 *   its returnhandlers before it next executes any of its USER-level code".
 *   Its handler, act_execute_returnhandlers(), takes act_lock_thread() -- a
 *   mutex -- and special_handler() ends by parking the thread in
 *   assert_wait()/thread_block() when it is suspended.  Neither is safe in
 *   the middle of kernel work.
 *
 * Taking it there cost both of the failures this issue records.  A thread
 * with an assert_wait() outstanding reached for that mutex and tripped
 * kern/lock.c's mutex_lock_assert_safe(), which forbids exactly that because
 * the mutex would block and the pending wakeup would be lost.  And a thread
 * still suspended between thread_setrun() and thread_resume() was parked by
 * special_handler() on &suspend_count after the wakeup for it had already
 * gone by -- a machine that simply stopped, with no assertion at all.
 *
 * ⚠️ Deferred, not dropped, and that is a property of ast_taken() rather than
 * a hope: it computes `reasons = need_ast[cpu] & mask' and clears only what
 * it took, so a bit outside the mask stays pending for the next return that
 * can have it.  The nine other callers of act_execute_returnhandlers() are
 * unaffected -- every one of them is a point where a thread CHOSE to check,
 * which is what makes them safe and what makes this one different.
 *
 * ⚠️ And it is what i386 already does.  There the kernel is not preemptible
 * at all and ASTs from kernel mode are AST_URGENT alone.  This does not
 * invent a rule; it stops x86-64 being the only machine that breaks the one
 * the machine-independent code was written against.
 */
#define	AST_KERNEL_SAFE		AST_PREEMPT

static void
trap_take_ast(struct trap_frame *frame)
{
	unsigned cpu = cpu_number();
	ast_t	 take;

	if (need_ast[cpu] == AST_NONE)
		return;

	/*
	 * Ring 3 is always safe to preempt.  Ring 0 is safe only where the
	 * kernel says so, and it says so with the preemption level: that
	 * counter exists precisely to mark the spans in which a processor must
	 * not be taken away, and every path that holds something raises it.
	 *
	 * Consulting it rather than refusing ring 0 outright, because until
	 * #422 brings up userland every thread this kernel has IS a kernel
	 * thread -- a rule of "ring 3 only" would mean nothing is ever
	 * preempted, on a machine whose whole scheduler is kernel threads.
	 *
	 * ⚠️ It is a weaker guarantee than i386's marked safe points, and the
	 * difference is real: a path that holds a lock without raising the
	 * level would be preempted here and not there.  Narrowing this is
	 * exactly the reading #454 carries -- deciding, call site by call
	 * site, which of the two things each one meant.  Until then the level
	 * is what the tree offers, and the alternative is no preemption at
	 * all.
	 *
	 * 🔥 AND WHAT THE TREE OFFERED WAS A CONSTANT (#461).
	 *
	 * get_preemption_level() is defined in <kern/cpu_data.h> as `return 0'
	 * whenever MACH_RT is off, which it is on both of this kernel's
	 * targets, and disable_preemption() is defined as nothing.  So the
	 * paragraph above described a mechanism that was not there: EVERY path
	 * holding a lock was a path that had not raised the level, and this
	 * test could never once have been true.
	 *
	 * It cost a deadlock.  A thread preempted here while holding a spin
	 * lock; the other processors reaching for the same lock from interrupt
	 * context, where the gate has already cleared IF; and then nothing left
	 * that could schedule the holder.  Four processors spinning on one
	 * instruction with interrupts off -- one boot in six of the #461 test,
	 * four in six once idle processors halted instead of polling.
	 *
	 * So the level is now real on this target: x86_64/cpu_data.h supplies
	 * it out of the per-CPU block, and <kern/cpu_data.h> takes it whenever
	 * a machine says it has one.  The test below is unchanged and finally
	 * tests something.
	 */
	if ((frame->cs & 3) == USER_RPL)
		take = AST_ALL;
	else {
		if (get_preemption_level() != 0)
			return;
		take = AST_KERNEL_SAFE;
	}

	/*
	 * And nothing at all if none of what is pending is ours to take.
	 *
	 * ⚠️ Asked again, against the mask, and not left to the AST_NONE test
	 * at the top.  A kernel thread with AST_APC pending and nothing else
	 * would otherwise fail that test on EVERY trap return for as long as
	 * the bit stayed set -- which, until this target has user mode (#422),
	 * is for ever -- and pay for the whole of ast_taken() each time to
	 * decide there was nothing to do.
	 */
	if ((need_ast[cpu] & take) == 0)
		return;

	ast_taken(FALSE, take, splsched());
}

/*
 * The same check, for the four ways of leaving the kernel that are not a trap
 * return (#422).
 *
 * 🔥 They did not have one, and that is how bootstrap could not create a
 * thread.
 *
 * thread_create() makes a new thread runnable with AST_APC pending and its
 * activation's suspend_count at one, and the comment beside it says it "will
 * immediately suspend itself".  The suspending is done BY the AST: the handler
 * sees suspend_count and blocks the thread, and it is that block which holds
 * the thread still until its creator has called thread_set_state and
 * thread_resume.
 *
 * On i386 thread_bootstrap_return is a label on return_from_trap, so the check
 * is simply there.  Here the three entry points in entry.S went straight to
 * act_user_frame -- which panics, correctly, because a thread that has never
 * run has no frame yet.  The message was about thread state and the cause was
 * an AST nobody looked at: pthread_create's first thread died before its
 * creator could reach the next statement.
 *
 * ⚠️ AST_ALL unconditionally, and no preemption-level test.  Every one of these
 * paths is a return TO USER by construction -- entry.S says so where it
 * swapgs's without testing -- so the ring-0 question trap_take_ast has to ask
 * does not arise here.
 *
 * ⚠️ A loop, as i386's `jmp return_from_trap' is a loop: taking one AST can
 * raise another, and the thread must not reach ring 3 with one outstanding.
 * If the handler blocks instead of returning, this stack is discarded and the
 * thread resumes at its continuation -- thread_bootstrap_return, which arrives
 * back here.  Either road ends with nothing pending.
 */
void
thread_return_ast(void)
{
	while (need_ast[cpu_number()] & AST_ALL)
		ast_taken(FALSE, AST_ALL, splsched());
}


/*
 * Resolve a fault, with interrupts as the interrupted code had them (#411).
 *
 * 🔥 A trap gate clears IF, so a handler runs with interrupts off -- and
 * vm_fault() is not a leaf.  It reaches pmap_enter(), which can unmap a page it
 * is replacing, which shoots the translation down on every processor, which is
 * a cross-call: `ipi: a cross-call with interrupts off would deadlock', and
 * ipi_call_others() says so rather than hanging, which is the only reason this
 * took minutes instead of an afternoon.
 *
 * ⚠️ Restored from the FRAME, not enabled unconditionally.  A fault taken in a
 * section that had interrupts off on purpose must not come back with them on;
 * the interrupted code's own flags are the only right answer, and they are
 * sitting in the frame the entry saved.
 */
/*
 * ⚠️ Returns the kern_return_t and not a boolean, and the difference is not
 * tidiness (#467).  When a fault from ring 3 cannot be resolved, the task's
 * exception handler is told WHY -- EXC_BAD_ACCESS carries the code as its
 * first word -- and a boolean has already thrown that away.  i386 passes the
 * same value to the same place, through user_page_fault_continue().
 */
static kern_return_t
fault_in(vm_map_t map, uint64_t addr, vm_prot_t prot,
	 const struct trap_frame *frame)
{
	kern_return_t	kr;
	boolean_t	had = (frame->rflags & RFLAGS_IF) != 0;

	if (had)
		interrupts_enable();

	kr = vm_fault(map, trunc_page((vm_offset_t) addr), prot, FALSE);

	if (had)
		interrupts_disable();

	return kr;
}

/*
 * How many traps arrived with SMAP switched off, and whether the last one was
 * still switched off once this had run (#468).
 *
 * Counted rather than flagged because the interesting number is not zero: a
 * fault inside copyin or copyout is ordinary — it is how an address a task
 * does not own becomes an error return — so these say the window exists and
 * is being closed, not that something went wrong.
 */
static uint64_t	ac_traps;
static uint64_t	ac_after_last;

uint64_t trap_smap_lifted_count(void)
{
	return ac_traps;
}

uint64_t trap_smap_after_last(void)
{
	return ac_after_last;
}

/*
 * ── A fault from ring 3, handed to the task (#467) ────────────────────
 *
 * The machine-independent kernel already knows what to do with a thread that
 * has faulted: exception() finds the handler the task or the thread named,
 * sends it a message describing the fault, and either resumes the thread from
 * the state the handler wrote back or terminates it.  Every part of that
 * exists; what did not exist on this target was anything that called it, so a
 * user program that faulted stopped the machine.
 *
 * ⚠️ exception() DOES NOT RETURN.  It ends in thread_exception_return() on the
 * way back to ring 3, or in the thread's termination; the i386 caller marks
 * every call site NOTREACHED for the same reason.  So nothing may be written
 * after it that the fault path still needs.
 *
 * ⚠️ The frame this was called with IS the thread's saved user frame -- a trap
 * from ring 3 lands at KERNEL_STACK_USER_FRAME(top), which is where pcb->user
 * points -- so the state the handler reads is the state that faulted, and the
 * state it writes back is what resumes.  That is not a coincidence to rely on
 * quietly: trap.h derives both from one expression precisely so there is one
 * claim about those bytes.
 */
static void
x86_64_exception(int exc, int code, int subcode)
{
	exception_data_type_t	codes[EXCEPTION_CODE_MAX];

	codes[0] = code;
	codes[1] = subcode;

	exception(exc, codes, 2);
	/*NOTREACHED*/
}

/*
 * Which Mach exception a vector is, and what the handler is told about it.
 *
 * Straight from i386's user_trap(), because it is the same architecture
 * answering the same question -- the codes even have the same values, under
 * EXC_X86_64_* names that <mach/x86_64/exception.h> has carried since #413 and
 * that nothing had ever used.  This is their first consumer.
 *
 * ⚠️ The subcode for the four faults that carry a selector is the error code's
 * low sixteen bits, which name the descriptor -- not the whole error word.
 * The upper bits are the flags that say WHERE the selector came from, and a
 * handler reading them as part of a selector would be told about a descriptor
 * that does not exist.
 */
static boolean_t
user_fault_exception(const struct trap_frame *frame,
		     int *exc, int *code, int *subcode)
{
	*code = 0;
	*subcode = 0;

	switch (frame->vector) {
	case T_DIVIDE_ERROR:
		*exc = EXC_ARITHMETIC;	*code = EXC_X86_64_DIVERR;	break;
	case T_DEBUG:
		*exc = EXC_BREAKPOINT;	*code = EXC_X86_64_SGLSTP;	break;
	case T_BREAKPOINT:
		*exc = EXC_BREAKPOINT;	*code = EXC_X86_64_BPTFLT;	break;
	case T_OVERFLOW:
		*exc = EXC_ARITHMETIC;	*code = EXC_X86_64_INTOFLT;	break;
	case T_BOUND_RANGE:
		*exc = EXC_SOFTWARE;	*code = EXC_X86_64_BOUNDFLT;	break;
	case T_INVALID_OPCODE:
		*exc = EXC_BAD_INSTRUCTION; *code = EXC_X86_64_INVOPFLT; break;
	case T_INVALID_TSS:
		*exc = EXC_BAD_INSTRUCTION; *code = EXC_X86_64_INVTSSFLT;
		*subcode = (int) (frame->error & 0xffff);		break;
	case T_SEGMENT_NOT_PRESENT:
		*exc = EXC_BAD_INSTRUCTION; *code = EXC_X86_64_SEGNPFLT;
		*subcode = (int) (frame->error & 0xffff);		break;
	case T_STACK_FAULT:
		*exc = EXC_BAD_INSTRUCTION; *code = EXC_X86_64_STKFLT;
		*subcode = (int) (frame->error & 0xffff);		break;
	case T_GENERAL_PROTECTION:
		*exc = EXC_BAD_INSTRUCTION; *code = EXC_X86_64_GPFLT;
		*subcode = (int) (frame->error & 0xffff);		break;
	case T_ALIGNMENT_CHECK:
		*exc = EXC_BAD_INSTRUCTION; *code = EXC_X86_64_ALIGNFLT;	break;
	case T_FPU_ERROR:
		*exc = EXC_ARITHMETIC;	*code = EXC_X86_64_EXTERRFLT;	break;
	case T_SIMD_ERROR:
		*exc = EXC_ARITHMETIC;	*code = EXC_X86_64_SSEFLT;	break;
	default:
		/*
		 * ⚠️ FALSE, not a default exception.  A vector with no meaning
		 * for a user program -- a double fault, a machine check, an
		 * unclaimed interrupt -- is not the task's business, and
		 * inventing an exception for it would send a server a message
		 * it cannot act on while losing the report that says what
		 * really happened.  Those still stop the machine, and should.
		 */
		return FALSE;
	}

	return TRUE;
}

void trap_dispatch(struct trap_frame *frame)
{
	/*
	 * Why the page fault below could not be resolved, kept for the task's
	 * exception handler (#467).  KERN_SUCCESS while no fault has been
	 * attempted, so a non-page-fault carries a code that means "not this".
	 */
	kern_return_t	fault_kr = KERN_SUCCESS;

	/*
	 * ── SMAP back on, for the handler (#468) ──────────────────────────
	 *
	 * The processor does not clear EFLAGS.AC on a fault.  So a trap taken
	 * inside a bracketed copy — which is the ordinary way copyin reports
	 * an address a task does not own — runs every line below with SMAP
	 * lifted, and any stray kernel pointer into the user half quietly
	 * works instead of faulting.  The one place the kernel is allowed to
	 * touch user memory would be lending that permission to the whole
	 * fault path.
	 *
	 * Clearing it here rather than restoring it on the way out is what
	 * makes it free to get right: IRETQ reloads the flags the trap saved,
	 * so the interrupted copy resumes with its own AC still set and needs
	 * no cooperation from this end.
	 *
	 * ⚠️ The paranoid entry (#440) writes its record a few instructions
	 * before calling this, and those instructions read MSRs and kernel
	 * memory only.  Every other entry — fault, interrupt, IPI — reaches
	 * this line first.
	 */
	if (read_rflags() & RFLAGS_AC) {
		pmap_user_access_end();
		ac_traps++;
		ac_after_last = read_rflags() & RFLAGS_AC;
	}

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
			trap_take_ast(frame);
			return;
		}
		/* Nobody claimed it — fall through and say so. */
	}

	/*
	 * The machine-independent kernel asking for the debugger (#428).
	 *
	 * Debugger() raises a breakpoint precisely so that this frame exists
	 * and describes its caller rather than the debugger.  Checked here,
	 * against a per-processor flag that only Debugger() sets, so that a
	 * breakpoint from anywhere else -- the boot self-test's, or an `int3'
	 * left in code under examination -- cannot be mistaken for one.
	 *
	 * ⚠️ Returns, where every other arm of this function halts, and it
	 * must: Debugger() is a CALL and its callers carry on afterwards.
	 * kern/debug.c's panic() is the one that proves it -- on a double
	 * panic it unlocks, restores the level and returns from Debugger() so
	 * an operator can continue.  `int3' is a trap rather than a fault, so
	 * the saved rip is already past it and resuming needs nothing else.
	 */
	/*
	 * The debugger stopping this processor (#428).
	 *
	 * Checked before the fault report, because an NMI that reaches the
	 * report is treated as a fault and halts -- which is the right answer
	 * for a real one and the wrong one for a processor being held still so
	 * that an operator can look at it.
	 *
	 * ddb_park_here() answers FALSE when no debugger owns the machine, so
	 * a genuine NMI falls through to the report exactly as before.
	 */
	if (frame->vector == T_NMI && ddb_park_here(frame))
		return;

	/*
	 * One of the debugger's breakpoints (#428).
	 *
	 * Before the expected-trap machinery below, and harmless to it: the
	 * table is empty until an operator sets something, so the boot
	 * self-test that arms DR0 for the swapgs window (#440) falls straight
	 * through exactly as before.
	 */
	if (frame->vector == T_DEBUG && ddb_breakpoint_hit(frame))
		return;

	if (frame->vector == T_BREAKPOINT) {
		const char *why = ddb_debugger_taken();

		if (why != (const char *) 0) {
			ddb_enter(frame, why);
			return;
		}
	}

	/*
	 * A fault from ring 3, which is recoverable because of where it came
	 * from rather than what it was.  Checked before the vector-matching
	 * arrangement below, since that one resumes on the current stack and
	 * this frame's stack belongs to a user program.
	 */
	if (user_expect_armed && (frame->cs & 3) == USER_RPL) {
		user_expect_armed = 0;

		trap_record_frame(frame);

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
	if ((frame->cs & 3) == 0 && frame->vector == T_PAGE_FAULT
	    && !va_is_kernel(read_cr2())) {
		/*
		 * ⚠️ A user address gets vm_fault() FIRST, and the exception
		 * table only if that fails (#411/#467).
		 *
		 * The table is how copyin() and copyout() survive an address a
		 * task does not own: the instruction is allowed to fault and
		 * the call returns an error.  Reaching it first makes it also
		 * the answer for an address the task DOES own and has simply
		 * never touched -- a page that is in the map and not yet
		 * resident -- so a perfectly valid copy fails, and the caller
		 * is told the task does not have the memory it does have.
		 *
		 * i386 makes the same distinction in user_page_fault_continue():
		 * fault it in, and only report a failure when the fault itself
		 * says no.
		 */
		thread_act_t	act = current_act();
		vm_map_t	map = (act != THR_ACT_NULL) ? act->map : VM_MAP_NULL;
		vm_prot_t	prot = (frame->error & PF_WRITE)
				     ? (VM_PROT_READ | VM_PROT_WRITE)
				     : VM_PROT_READ;

		if (map != VM_MAP_NULL
		    && fault_in(map, read_cr2(), prot, frame) == KERN_SUCCESS)
			return;		/* the copy runs again and succeeds */
	}

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

		trap_record_frame(frame);

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
		 * ⚠️ Here as well as on the fault report, and that is the point
		 * of putting it here at all.
		 *
		 * The report path only runs on a fault nobody expected, which
		 * ends the boot -- so a description written only there is
		 * exercised on the one occasion when nobody can afford to find
		 * out it was wrong.  The probes take this arm on every boot, so
		 * the sentence is produced and read where it costs nothing.
		 */
		if (carries_a_selector(frame->vector))
			report_selector_error(frame->error);

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

	/*
	 * ── A page fault handed to the machine-independent kernel (#467) ──
	 *
	 * Everything above this line is an arrangement made in advance: a
	 * vector somebody armed, an instruction allowed to fault, a debugger
	 * that owns the machine.  This is the ordinary case, and until now
	 * there was none -- a page that was in a task's map and had never been
	 * faulted in stopped the machine, with a report that described the
	 * fault perfectly and could do nothing about it.
	 *
	 * ⚠️ Which map is asked is decided by the ADDRESS, not by the ring.
	 * A kernel path reaching a user address on a task's behalf -- copyin,
	 * copyout, the argument block the bootstrap builds -- faults at ring 0
	 * on an address the task owns, and asking the kernel map about it would
	 * answer no about the wrong space.  i386 makes the same split, in the
	 * same place, on the same test.
	 *
	 * The protection asked for is what was ATTEMPTED, from the error code,
	 * and not what would be convenient: asking for write on a read fault
	 * would quietly make a read-only mapping writable, and asking for less
	 * than was attempted would return success and fault again forever.
	 */
	if (frame->vector == T_PAGE_FAULT) {
		uint64_t	addr = read_cr2();
		thread_act_t	act = current_act();
		vm_map_t	map;
		vm_prot_t	prot;

		if (va_is_kernel(addr))
			map = kernel_map;
		else
			map = (act != THR_ACT_NULL) ? act->map : VM_MAP_NULL;

		prot = (frame->error & PF_WRITE)
		     ? (VM_PROT_READ | VM_PROT_WRITE) : VM_PROT_READ;
		if (frame->error & PF_INSTRUCTION)
			prot |= VM_PROT_EXECUTE;

		/*
		 * ⚠️ Only when the map is one this fault can be about.  A fault
		 * before there is a thread -- and there are plenty, this kernel
		 * boots a long way before the first one -- has no task to ask,
		 * and inventing kernel_map for it would hand an early boot
		 * failure to a subsystem that is not up.
		 */
		if (map != VM_MAP_NULL) {
			fault_kr = fault_in(map, addr, prot, frame);
			if (fault_kr == KERN_SUCCESS)
				return;	/* the instruction runs again */
		}
	}

	/*
	 * ── And if it came from ring 3, the task hears about it (#467) ────
	 *
	 * Below this line is the report, which ends in a halt: right for a
	 * fault in the kernel, and wrong for a user program, whose faults are
	 * ordinary and whose task has somewhere to send them.
	 *
	 * ⚠️ After the page-fault attempt above and not before it.  A page a
	 * task owns and has never touched is not an exception at all -- it is
	 * resolved and the instruction runs again -- and raising first would
	 * turn every first touch of every page into a message.
	 */
	if ((frame->cs & 3) == USER_RPL && current_act() != THR_ACT_NULL) {
		int	exc, code, subcode;

		if (frame->vector == T_PAGE_FAULT) {
			/*
			 * EXC_BAD_ACCESS, carrying WHY the fault could not be
			 * resolved and the address that could not be reached --
			 * exactly what i386 passes from
			 * user_page_fault_continue().
			 */
			x86_64_exception(EXC_BAD_ACCESS, (int) fault_kr,
					 (int) read_cr2());
			/*NOTREACHED*/
		}

		if (user_fault_exception(frame, &exc, &code, &subcode)) {
			x86_64_exception(exc, code, subcode);
			/*NOTREACHED*/
		}
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

	if (carries_a_selector(frame->vector))
		report_selector_error(frame->error);

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
