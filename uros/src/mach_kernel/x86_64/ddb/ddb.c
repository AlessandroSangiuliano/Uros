/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The debugger (#428).
 */

#include <stdint.h>

#include <boot/bootarg.h>
#include <boot/multiboot2.h>
#include <ddb/cons.h>
#include <ddb/ddb.h>
#include <ddb/ksym.h>
#include <kern/cpu_data.h>	/* cpu_data[].active_thread */
#include <kern/cpu_number.h>
#include <kern/thread.h>	/* struct thread_shuttle */
#include <kern/processor.h>	/* default_pset */
#include <kern/queue.h>
#include <mach/boolean.h>
#include <kern/misc_protos.h>	/* panic */
#include <mach/machine.h>	/* machine_slot[] */
#include <mach/machine/vm_types.h>
#include <cpu/lapic.h>		/* lapic_send_nmi */
#include <cpu/regs.h>		/* cpu_pause */
#include <sync/atomic.h>	/* one debugger at a time */
#include <pmap/layout.h>
#include <pmap/pte.h>		/* PAGE_SIZE_4K */
#include <pmap/pmap.h>
#include <trap/trap.h>

static int enabled;

/*
 * ⚠️ ASKS boot_flag(), and used to parse the command line itself (#428).
 *
 * The private parse required `-r' to stand alone -- `p[2]' had to be a space
 * or the end -- and its reason was sound as far as it went: an `r' inside a
 * path or a device name must not turn the debugger on.
 *
 * But boot/bootarg.c already answers that question, for every other flag on
 * this machine, and answers it better: it opens a flag word only on a leading
 * dash and reads the letters within it, so `-rB' is r AND B exactly as `-CT'
 * is C and T.  Two parsers for one command line, agreeing on the simple case
 * and disagreeing on the useful one -- and nothing compares them, because
 * they are in different files and neither is wrong on its own (#448 again).
 *
 * It cost the first attempt at demonstrating this issue's own done-when.
 * Booting `-rB' gave B and silently not r, so Debugger() reported that the
 * debugger had not been asked for -- on the boot arranged to ask for it.  The
 * message was correct and the machine was right; the flag had genuinely not
 * arrived.
 *
 * ⚠️ The `info_pa' argument stays, and stays unused, because the caller has
 * it and a future reader will ask why this does not need it: the multiboot
 * information is found once by boot/multiboot2.c, and asking it twice is how
 * two answers appear.
 */
void ddb_init(uint32_t info_pa)
{
	(void) info_pa;

	enabled = boot_flag('r');
}

int ddb_enabled(void)
{
	return enabled;
}

/*
 * The same answer under the machine-independent kernel's own name (#428).
 *
 * kern/debug.c asks it before calling Debugger() from panic(), because the
 * compile-time MACH_KDB test it used to rely on stopped meaning "there is a
 * debugger" the moment a target had one without the ddb/ tree.  A forwarder
 * rather than a rename: `ddb_enabled' is what this file's own code asks, and
 * one of the two names had to be the machine-independent spelling.
 */
boolean_t debugger_available(void)
{
	return enabled ? TRUE : FALSE;
}

/* ------------------------------------------------------------------ */
/*  Reading a line                                                      */
/* ------------------------------------------------------------------ */
/*
 * Echoed as it is typed, because the far end is a serial line and nothing
 * else is going to show the operator what they have written.
 *
 * Backspace is handled and nothing else is: this is a prompt for two-letter
 * commands under a fault, not a shell, and every editing feature is another
 * thing that can be wrong at the worst moment.
 */
#define DDB_LINE_MAX	64

static char line[DDB_LINE_MAX];

static const char *ddb_readline(void)
{
	unsigned n = 0;

	for (;;) {
		int c = cons_getc();

		if (c == '\r' || c == '\n') {
			cons_puts("\r\n");
			line[n] = '\0';
			return line;
		}

		if (c == 0x7F || c == '\b') {
			if (n > 0) {
				n--;
				cons_puts("\b \b");
			}
			continue;
		}

		if (c < ' ' || c > '~' || n + 1 >= DDB_LINE_MAX)
			continue;

		line[n++] = (char)c;
		cons_putc((char)c);
	}
}

static const char *skip_spaces(const char *p)
{
	while (*p == ' ')
		p++;
	return p;
}

/*
 * A hexadecimal number, with or without the 0x. Returns the address of the
 * first character it did not use, so the caller can tell "nothing was there"
 * from "zero was there" — a distinction that matters when zero is a plausible
 * thing to ask about.
 */
static const char *parse_hex(const char *p, uint64_t *out)
{
	uint64_t v = 0;
	int any = 0;

	p = skip_spaces(p);
	if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
		p += 2;

	for (;; p++) {
		int d;

		if (*p >= '0' && *p <= '9')
			d = *p - '0';
		else if (*p >= 'a' && *p <= 'f')
			d = *p - 'a' + 10;
		else if (*p >= 'A' && *p <= 'F')
			d = *p - 'A' + 10;
		else
			break;

		v = v * 16 + (uint64_t)d;
		any = 1;
	}

	if (any)
		*out = v;
	return any ? p : 0;
}

/* ------------------------------------------------------------------ */
/*  What the commands say                                               */
/* ------------------------------------------------------------------ */
static void put_symbol(uint64_t addr)
{
	uint64_t off = 0;
	const char *name = ksym_lookup(addr, &off);

	if (name == 0)
		return;

	cons_puts(" <");
	cons_puts(name);
	if (off != 0) {
		cons_puts("+");
		cons_puthex64(off);
	}
	cons_puts(">");
}

static void put_reg(const char *name, uint64_t v, int newline)
{
	cons_puts(newline ? "\r\n  " : "  ");
	cons_puts(name);
	cons_putc(' ');
	cons_puthex64(v);
}

static void show_registers(const struct trap_frame *f)
{
	put_reg("rip   ", f->rip, 1);
	put_symbol(f->rip);
	put_reg("rsp   ", f->rsp, 1);
	put_reg("rflags", f->rflags, 0);
	put_reg("rax   ", f->rax, 1);
	put_reg("rbx   ", f->rbx, 0);
	put_reg("rcx   ", f->rcx, 0);
	put_reg("rdx   ", f->rdx, 1);
	put_reg("rsi   ", f->rsi, 0);
	put_reg("rdi   ", f->rdi, 0);
	put_reg("rbp   ", f->rbp, 1);
	put_reg("r8    ", f->r8, 0);
	put_reg("r9    ", f->r9, 0);
	put_reg("r10   ", f->r10, 1);
	put_reg("r11   ", f->r11, 0);
	put_reg("r12   ", f->r12, 0);
	put_reg("r13   ", f->r13, 1);
	put_reg("r14   ", f->r14, 0);
	put_reg("r15   ", f->r15, 0);
	put_reg("cs    ", f->cs, 1);
	put_reg("ss    ", f->ss, 0);
	cons_puts("\r\n");
}

/*
 * Every read is checked against the page tables first.
 *
 * A debugger is asked about addresses precisely because they are suspect, so
 * faulting on one would replace the answer with a second fault — and this
 * one would arrive from inside the handler for the first.
 */
#define DDB_DUMP_WORDS	8

static void examine(uint64_t addr)
{
	pmap_t kernel = pmap_kernel();

	for (unsigned i = 0; i < DDB_DUMP_WORDS; i++) {
		uint64_t at = addr + i * 8;

		cons_puts("  ");
		cons_puthex64(at);
		cons_puts(": ");

		if (!va_is_canonical(at) || pmap_extract(kernel, at) == 0) {
			cons_puts("unmapped\r\n");
			return;
		}

		cons_puthex64(*(const volatile uint64_t *)(uintptr_t)at);
		cons_puts("\r\n");
	}
}

/*
 * The same frame-pointer walk the fault report uses, with the same
 * suspicion: alignment, canonical form and a mapping are checked before
 * anything is read, and a frame that does not move upward is not a frame.
 */
#define DDB_TRACE_MAX	24

static void trace(uint64_t rbp)
{
	pmap_t kernel = pmap_kernel();

	for (unsigned depth = 0; depth < DDB_TRACE_MAX; depth++) {
		const uint64_t *frame;
		uint64_t next, ret;

		if ((rbp & 7) != 0 || !va_is_canonical(rbp))
			return;
		if (pmap_extract(kernel, rbp) == 0
		    || pmap_extract(kernel, rbp + 8) == 0)
			return;

		frame = (const uint64_t *)(uintptr_t)rbp;
		next = frame[0];
		ret = frame[1];

		if (ret == 0)
			return;

		cons_puts("  ");
		cons_puthex64(ret);
		put_symbol(ret);
		cons_puts("\r\n");

		if (next <= rbp)
			return;
		rbp = next;
	}
}

static void usage(void)
{
	cons_puts("  r            registers\r\n"
		  "  t [rbp]      backtrace, from rbp or from the frame\r\n"
		  "  x <addr>     eight words at addr\r\n"
		  "  s <addr>     which function contains addr\r\n"
		  "  c            continue\r\n"
		  "  h            halt\r\n");
}

/*
 * ── Debugger(), and why it goes through a breakpoint ──────────────────
 *
 * This is the machine-independent kernel's own way in.  Every assert() that
 * fails reaches it (kern/debug.c), and so does panic() -- twice, because on a
 * double panic it deliberately unwinds, restores the level and RETURNS, so
 * that an operator can carry on from the prompt.  Until now this machine
 * answered all of that with a panic saying there was no debugger, which was
 * true when it was written and stopped being true in the same issue.
 *
 * ⚠️ It raises a breakpoint rather than calling ddb_enter() directly, and the
 * reason is the frame.  ddb_enter() shows registers and walks a stack, and an
 * assert() does not arrive from a trap -- so a frame built here would be
 * THIS function's registers, not the ones the caller died with.  A debugger
 * that answers confidently about the wrong context is worse than none: the
 * numbers look like an answer.
 *
 * `int3' costs nothing and solves it exactly: the entry stubs save the real
 * register file into a real trap frame, which is the machinery that exists
 * for precisely this.  i386's Debugger() is one `int3' and nothing else, and
 * that part of it is right.
 *
 * What is added here is the two things it drops.  The message is kept, so the
 * prompt says WHY it opened instead of only "breakpoint".  And the flag is
 * per-processor: two processors failing an assertion in the same microsecond
 * is not hypothetical -- it happened twice while #463 was being chased -- and
 * a single global would have one of them reading the other's reason.
 */
static const char *debugger_why[NCPUS];

const char *ddb_debugger_taken(void)
{
	unsigned	cpu = cpu_number();
	const char	*why = debugger_why[cpu];

	debugger_why[cpu] = (const char *) 0;
	return why;
}

void Debugger(const char *message)
{
	/*
	 * ⚠️ Still a panic when the debugger was not asked for, and that is
	 * the property every unattended boot depends on: a run that stopped at
	 * a prompt nobody is there to answer would hang the harness instead of
	 * failing it.  What changed is that the message no longer says there
	 * is no debugger -- there is one, and this says how to get it.
	 */
	if (!ddb_enabled())
		panic("Debugger(\"%s\"): the debugger is present but was not "
		      "asked for — boot with -r to stop here instead (#428)",
		      message ? message : "");

	debugger_why[cpu_number()] = message && *message ? message : "Debugger";

	__asm__ volatile ("int3");
}

/*
 * ── A way in on a machine that is already RUNNING (#428) ──────────────
 *
 * Until now the prompt opened at a fault or at a deliberate Debugger(), which
 * covers a machine that has died and not one that has stopped answering.
 * That second case is the expensive one: three boot entries wedged repeatedly
 * while #463 was being found, and the only recourse was reading the log
 * afterwards and guessing.
 *
 * So: a character on the console opens it.  Polled from the timer tick rather
 * than driven by a UART interrupt, deliberately -- the machine this is for is
 * one that is not servicing things properly any more, and a door that needs
 * the interrupt controller to be in good order is a door that is shut exactly
 * when it is wanted.  One `inb' every ten milliseconds is the whole cost.
 *
 * ⚠️ ONE processor polls, and it has to be so: the UART is a single device,
 * and two processors reading its data register would take each other's
 * characters -- with the loser reporting that nothing was typed.
 *
 * ⚠️ And this does NOT reach a processor spinning with interrupts disabled,
 * which is what a real wedge often is (#461's deadlock was four of them at
 * once).  No tick arrives there, so no poll happens.  That case needs an NMI
 * from another processor and is the next piece of this issue; saying so here
 * because a door that quietly does not work in the case it was built for is
 * worse than an absent one.
 *
 * The character is Ctrl-\.  Something that is never part of ordinary output
 * and never typed by accident, and a single byte rather than a sequence: a
 * machine in trouble is not the place for a state machine, and i386's own
 * serial entry is broken (#382) partly for having one.
 *
 * ⚠️ Anything else typed is CONSUMED and discarded.  Nothing outside the
 * debugger reads the console on this target, so there is nobody to take it
 * from -- but that stops being true the day there is a console driver, and
 * this is the line that will need to change.
 */
/*
 * ── ONE break character, and ONE place that knows it (#428) ───────────
 *
 * The kernel will have several ways in: the serial line today, the frame
 * buffer console with a PS/2 keyboard next, and a gpu_server in USER SPACE
 * after that.  Every one of them must open the debugger the same way, and the
 * only arrangement that keeps that true is one function that decides, fed by
 * all of them.
 *
 * Not because three copies would be untidy -- because they would DRIFT.  This
 * tree has produced that failure twice in two days: two parsers for one
 * command line, agreeing on the simple case and disagreeing on the useful one
 * (#428, fixed this morning), and a harness rule restated per test so that it
 * was missing for the newest one (#408).  Each source translates its own input
 * to a character; this decides what a character means.
 *
 * ⚠️ It does NOT enter the debugger -- it RECORDS that somebody asked, and the
 * next return from a trap opens the prompt with its own real frame.  That is
 * what lets a door exist without being able to produce a trap frame, which is
 * exactly the position a user-space gpu_server will be in, and it is the same
 * deferral the AST path uses for the same reason.
 *
 * ── The character ─────────────────────────────────────────────────────
 *
 * A single byte and not a sequence: a machine in trouble is not the place for
 * a state machine, and i386's serial door is broken (#382) partly for having
 * one.
 *
 * Ctrl-\ by default, and settable at boot with -k<char>.  i386 uses Ctrl-D,
 * which is a crowded choice -- in its own keyboard map CTRL('d') is also
 * delete-char and the `~' escape lands on it (i386/AT386/ddb_kbd.c:123,220),
 * and on a host terminal it is end-of-file.  Settable because the right key
 * depends on what sits between the operator and the machine: a terminal
 * server, telnet, screen and tmux each steal different ones, and a constant
 * would make that an argument instead of a flag.
 */
#define DDB_BREAK_DEFAULT	0x1C		/* Ctrl-\ */

static int	in_ddb[NCPUS];
static int	break_char = DDB_BREAK_DEFAULT;
static int	break_requested;

static void ddb_enter_body(struct trap_frame *frame, const char *why);

/*
 * A character from any console.  Answers TRUE when it was the break and has
 * been taken, so the caller drops it; FALSE when it belongs to whoever was
 * reading.
 */
boolean_t ddb_break_char(int c)
{
	if (!enabled || c != break_char)
		return FALSE;

	break_requested = 1;
	return TRUE;
}

/*
 * The serial line's own caller: read what is waiting and offer it.
 *
 * Polled from the timer tick rather than driven by a UART interrupt on
 * purpose -- the machine this is for has stopped servicing things properly,
 * and a door that needs the interrupt controller in good order is shut
 * exactly when it is wanted.  One `inb' every ten milliseconds.
 *
 * ⚠️ ONE processor may call this: the UART is a single device, and two
 * readers would take each other's characters, with the loser reporting that
 * nothing was typed.
 *
 * ⚠️ And it reaches only a processor whose interrupts are on.  A wedge with
 * IF clear takes no tick and so is never asked; that case needs an NMI from
 * another processor and is not this door.
 */
void ddb_poll_console(struct trap_frame *frame)
{
	int c;

	if (!enabled)
		return;

	c = cons_getc_nowait();
	if (c >= 0)
		(void) ddb_break_char(c);

	ddb_take_break(frame);
}

/*
 * Open the prompt if somebody asked for it.  Called from a trap return, which
 * is where a real frame exists.
 */
void ddb_take_break(struct trap_frame *frame)
{
	if (!break_requested || in_ddb[cpu_number()])
		return;

	break_requested = 0;
	ddb_enter(frame, "break requested on the console");
}

/*
 * ── Breakpoints, in the debug registers and not in the code (#428) ────
 *
 * The classic way is to write 0xCC over the instruction and put the byte back
 * afterwards.  Not here, and for two reasons that are facts about this kernel
 * rather than preferences:
 *
 *   CR0.WP is on (x86_64/pmap/pmap.c), so ring 0 cannot write a page marked
 *   read-only and .text is read-only.  A software breakpoint would have to go
 *   in through the direct map's writable alias of the same physical page --
 *   possible, and a second way to reach memory that the rest of the debugger
 *   does not need.
 *
 *   And on four processors the byte is shared.  Another processor executing
 *   that instruction between the write and the restore takes a breakpoint
 *   nobody set.  The parking makes that safe here, but only by accident of
 *   when breakpoints happen to be edited.
 *
 * The debug registers have neither problem: nothing is modified, so W^X is
 * untouched, and DR0-DR3 are PER-PROCESSOR state -- which is the catch, and
 * the reason for the arming rule below.
 *
 * ⚠️ FOUR, because the hardware has four.  A fifth is refused rather than
 * accepted and quietly dropped.
 *
 * ── Armed by each processor on itself ─────────────────────────────────
 *
 * A breakpoint set at the prompt would otherwise exist only on the processor
 * that set it, and the operator would watch code run on the other three
 * without stopping -- which reads as a breakpoint that does not work.
 *
 * So the table is shared and every processor programs its own registers from
 * it on the way out of the debugger: the one at the prompt when it continues,
 * and each parked one when it is released.  They cannot disagree, because
 * neither of them decides anything.
 */
struct ddb_breakpoint {
	uint64_t	addr;
	int		set;
};

static struct ddb_breakpoint	breakpoints[DR_COUNT];

/*
 * Program this processor's debug registers from the shared table.
 *
 * ⚠️ DR7 is written last and from scratch.  Enabling a slot before its
 * address register holds the address means a breakpoint at whatever was
 * there -- which on this machine is zero, and a breakpoint at zero fires on
 * the first null call and reports itself as the operator's.
 */
static void arm_breakpoints(void)
{
	uint64_t dr7 = DR7_RESERVED_ONE;

	for (unsigned i = 0; i < DR_COUNT; i++) {
		if (!breakpoints[i].set)
			continue;
		write_dr(i, breakpoints[i].addr);
		dr7 |= DR7_LOCAL(i);
	}

	write_dr6(0);
	write_dr7(dr7);
}

static void show_breakpoints(void)
{
	int any = 0;

	for (unsigned i = 0; i < DR_COUNT; i++) {
		if (!breakpoints[i].set)
			continue;
		any = 1;
		cons_puts("  ");
		cons_putdec(i);
		cons_puts("  ");
		cons_puthex64(breakpoints[i].addr);
		put_symbol(breakpoints[i].addr);
		cons_puts("\r\n");
	}
	if (!any)
		cons_puts("  no breakpoints; b <addr> sets one, and there are "
			  "four\r\n");

	/*
	 * And what this processor's registers actually hold.  Printed rather
	 * than assumed: a table that says a breakpoint is set and a DR7 that
	 * says nothing is armed are two different claims, and only the second
	 * one is what the hardware will act on.
	 */
	cons_puts("  dr7 ");
	cons_puthex64(read_dr7());
	cons_puts("  dr6 ");
	cons_puthex64(read_dr6());
	cons_puts("  dr0 ");
	cons_puthex64(read_dr0());
	cons_puts("\r\n");
}

static void set_breakpoint(uint64_t addr)
{
	if (span_is_code(addr) == 0) {
		cons_puts("  that address is not in any executable section — "
			  "a breakpoint there would never fire\r\n");
		return;
	}

	for (unsigned i = 0; i < DR_COUNT; i++) {
		if (breakpoints[i].set && breakpoints[i].addr == addr) {
			cons_puts("  already set as ");
			cons_putdec(i);
			cons_puts("\r\n");
			return;
		}
	}

	for (unsigned i = 0; i < DR_COUNT; i++) {
		if (breakpoints[i].set)
			continue;
		breakpoints[i].addr = addr;
		breakpoints[i].set = 1;
		cons_puts("  breakpoint ");
		cons_putdec(i);
		cons_puts(" at ");
		cons_puthex64(addr);
		put_symbol(addr);
		cons_puts("\r\n");
		return;
	}

	cons_puts("  all four debug registers are in use — the processor has "
		  "no more\r\n");
}

static void clear_breakpoint(uint64_t n)
{
	if (n >= DR_COUNT || !breakpoints[n].set) {
		cons_puts("  no such breakpoint — try b\r\n");
		return;
	}
	breakpoints[n].set = 0;
	cons_puts("  breakpoint ");
	cons_putdec(n);
	cons_puts(" removed\r\n");
}

/*
 * A debug exception arrived.  Answers TRUE when it was one of ours, in which
 * case the prompt has been opened and the frame is ready to resume.
 */
boolean_t ddb_breakpoint_hit(struct trap_frame *frame)
{
	uint64_t dr6 = read_dr6();

	for (unsigned i = 0; i < DR_COUNT; i++) {
		if ((dr6 & DR6_HIT(i)) == 0 || !breakpoints[i].set)
			continue;

		/*
		 * ⚠️ DR6 is sticky -- the processor sets the bit and never
		 * clears it -- so a handler that leaves it makes the next
		 * debug exception describe this one as well.
		 */
		write_dr6(0);

		ddb_enter(frame, "breakpoint");

		/*
		 * ⚠️ RESUME FLAG, set here rather than trusted.  An execution
		 * breakpoint faults BEFORE the instruction runs, so returning
		 * without it walks straight back into the same breakpoint and
		 * stays there.  The processor is supposed to set it; the
		 * emulator this is developed on arrives with rflags 0x2 and
		 * does not, which trap_dispatch already records one screen
		 * below for the same reason.
		 */
		frame->rflags |= RFLAGS_RF;
		return TRUE;
	}

	return FALSE;
}

/*
 * ── The other processors, while one is at the prompt (#428) ───────────
 *
 * Not a refinement: without it the prompt is unusable, and the log said so
 * before the code did.  The first panic that opened the prompt had three
 * other processors still running, and they wrote their clock reports across
 * the operator's typing.  Worse than the mess is what it means -- what you
 * are looking at is not the machine, it is a quarter of a machine moving
 * underneath you, and every answer is about a moment that has passed.
 *
 * ⚠️ STOPPED WITH AN NMI, and nothing else would do.  An ordinary IPI is
 * delivered only to a processor whose interrupts are on, and the processors
 * worth stopping are precisely the ones that are not taking interrupts --
 * #461's deadlock was four of them spinning with IF clear.  The NMI is the
 * only thing that reaches those, which is also why this same mechanism is
 * the door onto a wedged machine: the operator's processor is at the prompt,
 * and the wedged one is parked where it can be examined.
 *
 * Each parked processor leaves its frame behind before it spins, so `p' can
 * say where every one of them is and `P n' can walk any of them.  That is the
 * question this port has asked most often and answered with print statements.
 */
#define DDB_NOBODY		(-1L)

static volatile long		ddb_owner = DDB_NOBODY;
static volatile int		ddb_parked[NCPUS];
static volatile int		ddb_stop_sent[NCPUS];
static struct trap_frame	*ddb_frame[NCPUS];

/*
 * Called from the NMI arm of trap_dispatch().  Answers TRUE when this NMI was
 * the debugger asking, and the processor has parked and been released; FALSE
 * when it is a real one and belongs to whoever reports them.
 */
/*
 * Hold this processor until whoever owns the debugger lets it go.
 *
 * Shared by the two ways a processor can arrive here: an NMI sent by the
 * owner, and reaching ddb_enter() on its own while somebody else already has
 * the machine.
 */
static void park_until_released(struct trap_frame *frame)
{
	unsigned me = cpu_number();

	ddb_frame[me] = frame;
	ddb_parked[me] = 1;

	while (ddb_owner >= 0 && ddb_owner != (int) me)
		cpu_pause();

	ddb_parked[me] = 0;
	ddb_frame[me] = (struct trap_frame *) 0;

	/* Whatever the operator set while we were held, on this processor. */
	arm_breakpoints();
}

boolean_t ddb_park_here(struct trap_frame *frame)
{
	unsigned me = cpu_number();

	/*
	 * ⚠️ Decided by whether a stop was SENT to this processor, and not by
	 * whether the debugger still owns the machine (#428).
	 *
	 * The broadcast is asynchronous.  A processor that takes its NMI after
	 * the operator has already typed `c' would, on the second test, find
	 * ddb_owner cleared, answer "not mine", and fall through to the fault
	 * report -- which opened a prompt reading `ddb: non-maskable
	 * interrupt' on a machine where nothing had gone wrong.  Two runs in
	 * five, and the log named it exactly.
	 *
	 * The flag is per-processor and consumed once, so a stop that was sent
	 * is always absorbed by the processor it was sent to, whenever it
	 * arrives, and a genuine NMI is still reported as one.
	 */
	if (!ddb_stop_sent[me])
		return FALSE;

	ddb_stop_sent[me] = 0;

	if (ddb_owner >= 0 && ddb_owner != (int) me)
		park_until_released(frame);
	else
		arm_breakpoints();	/* the stop was lifted before it landed */

	return TRUE;
}

/*
 * Claim the machine, or answer that somebody else has it (#428).
 *
 * ⚠️ ATOMIC, and it has to be.  A processor reaches ddb_enter() by four
 * routes now, and three of them can happen on any processor at any moment: a
 * fault, a panic, and a BREAKPOINT.  A breakpoint on code every processor
 * runs -- the timer tick, say -- fires on several of them within microseconds
 * of each other, and without a claim they all walk into the prompt together.
 *
 * That was not hypothetical.  It is what made the first breakpoint flaky: one
 * boot in five worked, and breaking back in afterwards found
 *
 *	cpu 1  parked at <ddb_enter+0x650>
 *	cpu 2  at the prompt
 *
 * -- two processors inside the debugger, reading the same UART and taking
 * each other's characters, with the second parked halfway through the first's
 * entry.  The registers were right the whole time; what was wrong was that
 * there were two debuggers.
 */
static boolean_t claim_machine(void)
{
	unsigned me = cpu_number();

	return atomic_cmpxchg64((volatile uint64_t *) &ddb_owner,
				(uint64_t) DDB_NOBODY, (uint64_t) me)
	       ? TRUE : FALSE;
}

static void ddb_stop_others(void)
{
	unsigned	me = cpu_number();
	uint64_t	spins;

	for (int i = 0; i < NCPUS; i++) {
		if (i == (int) me || !machine_slot[i].is_cpu
		    || !machine_slot[i].running)
			continue;
		/*
		 * The slot number IS the APIC id on this target --
		 * cause_ast_check() targets an IPI the same way
		 * (x86_64/cpu/machdep.c).  Said here because the two would
		 * have to change together.
		 */
		ddb_stop_sent[i] = 1;
		lapic_send_nmi((uint32_t) i);
	}

	/*
	 * Bounded, and the count is reported rather than waited on for ever.
	 * A processor that does not park is a fact about this machine worth
	 * knowing -- and a debugger that hung waiting for one would be the
	 * failure it exists to diagnose.
	 */
	for (spins = 0; spins < 200000000ULL; spins++) {
		int missing = 0;

		for (int i = 0; i < NCPUS; i++) {
			if (i == (int) me || !machine_slot[i].is_cpu
			    || !machine_slot[i].running)
				continue;
			if (!ddb_parked[i])
				missing++;
		}
		if (missing == 0)
			return;
		cpu_pause();
	}
}

static void ddb_release_others(void)
{
	ddb_owner = DDB_NOBODY;
}

/*
 * ── What a thread is doing, and the one case that must not be walked ──
 *
 * "Which thread is on which processor and what is it waiting for" is the
 * question this port has asked most often, and it has been answered with
 * print statements every time.
 *
 * ⚠️ A THREAD BLOCKED WITH A CONTINUATION HAS NO STACK TO WALK, and that is
 * the whole reason this prints rather than traces.  A continuation is the
 * thread declaring it does not need its stack kept; machine_kernel_stack_init()
 * then RESETS that stack to point at the trampoline.  The memory is still
 * mapped and still walkable, which is what makes it dangerous: a walk does not
 * come back empty, it comes back with two or three frames of
 * context_thread_start and thread_begin_trampoline -- confident, well-formed,
 * and silent about why the thread blocked.  The same failure as a
 * disassembler that guesses, one level up.
 *
 * What is left instead is better than a stack, and is what the operator
 * actually wants: where it will RESUME (a function pointer, so a symbol) and
 * what it is waiting ON.
 *
 * ⚠️ The `continuation' field alone does not mean "blocked with one" -- every
 * thread that has never run carries one too, because thread_start() sets it.
 * The state flags are what separate them, and they are printed rather than
 * interpreted so the reader can disagree.
 */
static int readable(uint64_t va, uint64_t bytes)
{
	pmap_t kernel = pmap_kernel();

	for (uint64_t off = 0; off < bytes; off += PAGE_SIZE_4K) {
		if (!va_is_canonical(va + off)
		    || pmap_extract(kernel, va + off) == 0)
			return 0;
	}
	return va_is_canonical(va + bytes - 1)
	    && pmap_extract(kernel, va + bytes - 1) != 0;
}

/*
 * A thread's state in the seven columns the machine-independent debugger has
 * always used, in its order, with its letters (ddb/db_print.c:328).
 *
 *	R  running or on a run queue	O  swapped out
 *	W  waiting			N  waiting uninterruptibly
 *	S  asked to stop		C  has a continuation
 *	I  the idle thread
 *
 * Deliberately not a set of words of our own.  An operator who knows the
 * 32-bit debugger must be able to read this one without relearning it, and
 * the letters are the part of DDB that is worth keeping unchanged -- they
 * cost nothing to match and everything to diverge from.
 *
 * ⚠️ Two differences, and both are stated rather than silent.  The sixth
 * column there is `F', whether the activation has a floating-point state,
 * which is asked through db_act_fp_used() and has no answer here yet -- so
 * this prints `I' for the idle thread in a column of its own instead of
 * claiming an answer it has not got.  And `C' means the field is set, which
 * is not the same as "blocked with one": a thread that has never run carries
 * a continuation too, because thread_start() sets it.  The line below the
 * list says which case it is.
 */
static void put_state(const struct thread_shuttle *t)
{
	char	col[8];
	int	state = t->state;

	col[0] = (state & TH_RUN)   ? 'R' : '.';
	col[1] = (state & TH_WAIT)  ? 'W' : '.';
	col[2] = (state & TH_SUSP)  ? 'S' : '.';
	col[3] = (state & TH_SWAPPED_OUT) ? 'O' : '.';
	col[4] = (state & TH_UNINT) ? 'N' : '.';
	col[5] = (state & TH_IDLE)  ? 'I' : '.';
	col[6] = t->continuation    ? 'C' : '.';
	col[7] = 0;

	cons_puts(" ");
	cons_puts(col);
}

static void describe_thread(uint64_t addr)
{
	const struct thread_shuttle *t;

	if (!readable(addr, sizeof *t)) {
		cons_puts("  no thread readable there\r\n");
		return;
	}

	t = (const struct thread_shuttle *)(uintptr_t) addr;

	cons_puts("  thread ");
	cons_puthex64(addr);
	cons_puts("  state");
	put_state(t);
	cons_puts("\r\n");

	cons_puts("    waiting on ");
	if (t->wait_event == (event_t) 0) {
		cons_puts("nothing");
	} else {
		cons_puthex64((uint64_t)(uintptr_t) t->wait_event);
		put_symbol((uint64_t)(uintptr_t) t->wait_event);
	}
	cons_puts("\r\n");

	cons_puts("    resumes at ");
	if (t->continuation == 0) {
		cons_puts("nowhere — it keeps its stack, so `t' on it is real");
	} else {
		cons_puthex64((uint64_t)(uintptr_t) t->continuation);
		put_symbol((uint64_t)(uintptr_t) t->continuation);
	}
	cons_puts("\r\n");

	/*
	 * Said out loud rather than left for the operator to remember.  A
	 * thread that is not running and has a continuation gave up its stack
	 * contents, and walking it would produce a plausible answer about
	 * nothing.
	 */
	if ((t->state & TH_RUN) == 0 && t->continuation != 0)
		cons_puts("    its stack was reset when it blocked: a "
			  "backtrace would show the trampoline and not why "
			  "it stopped\r\n");
}

/*
 * Every thread the default processor set knows about, one line each.
 *
 * ⚠️ MACH'S TYPED QUEUES DO NOT HOLD LINK ADDRESSES, THEY HOLD ELEMENTS, and
 * the first version of this walked them as though they did.  kern/queue.h's
 * queue_enter() is explicit about it:
 *
 *	(head)->next = (queue_entry_t) (elt);
 *	((type)prev)->field.next = (queue_entry_t)(elt);
 *
 * -- the head and every `next' carry the ELEMENT pointer, and the chaining
 * goes through the element's field.  Reading them as intrusive links and
 * subtracting offsetof(thread_shuttle, pset_threads) landed 0x20 below a real
 * thread, read `state' and `wait_event' from the wrong place, and wandered
 * off into another queue inside default_pset itself.  The output looked like
 * a thread list -- addresses, flag names, wait events -- and was noise.  It
 * was caught because the same run's `p' printed the real thread addresses one
 * screen above, and they were 0x20 apart.
 *
 * So there is no arithmetic here at all: the pointers are threads.
 *
 * ⚠️ Not locked, and cannot be: the other processors are parked in an NMI and
 * one of them may hold the set's lock.  Waiting for it would hang the
 * debugger on the machine it exists to examine.  What makes that acceptable
 * is that everything is READ, bounded, and checked for readability before it
 * is dereferenced -- the worst outcome is a stale or truncated list, and the
 * list says so rather than hiding it.
 */
#define DDB_THREADS_MAX		256

static void list_threads(void)
{
	const queue_head_t	*q = &default_pset.threads;
	const queue_entry_t	head = (const queue_entry_t) q;
	queue_entry_t		e = (queue_entry_t) q->next;
	unsigned		n = 0;

	cons_puts("  ");
	cons_putdec((uint64_t) default_pset.thread_count);
	cons_puts(" threads in the default set\r\n");

	while (e != head) {
		const struct thread_shuttle *t;
		uint64_t addr = (uint64_t)(uintptr_t) e;

		if (n++ >= DDB_THREADS_MAX) {
			cons_puts("  ... stopped after ");
			cons_putdec(DDB_THREADS_MAX);
			cons_puts(" — the list does not end where it should"
				  "\r\n");
			return;
		}

		if (!readable(addr, sizeof *t)) {
			cons_puts("  ");
			cons_puthex64(addr);
			cons_puts(" is not readable — the list stops here\r\n");
			return;
		}

		t = (const struct thread_shuttle *)(uintptr_t) addr;

		cons_puts("  ");
		cons_puthex64(addr);
		put_state(t);
		if (t->continuation != 0 && (t->state & TH_RUN) == 0) {
			cons_puts("  no stack, resumes at ");
			put_symbol((uint64_t)(uintptr_t) t->continuation);
		} else if (t->wait_event != (event_t) 0) {
			cons_puts("  waiting on ");
			cons_puthex64((uint64_t)(uintptr_t) t->wait_event);
			put_symbol((uint64_t)(uintptr_t) t->wait_event);
		}
		cons_puts("\r\n");

		e = (queue_entry_t) t->pset_threads.next;
	}
}

static void show_processors(void)
{
	unsigned me = cpu_number();

	for (int i = 0; i < NCPUS; i++) {
		if (!machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;

		cons_puts("  cpu ");
		cons_putdec((uint64_t) i);
		if (i == (int) me) {
			cons_puts("  at the prompt\r\n");
			if (cpu_data[i].active_thread != 0)
				describe_thread((uint64_t)(uintptr_t)
						cpu_data[i].active_thread);
			continue;
		}
		if (!ddb_parked[i]) {
			cons_puts("  DID NOT PARK — it is not taking NMIs, "
				  "and nothing here describes it\r\n");
			continue;
		}
		cons_puts("  parked at ");
		cons_puthex64(ddb_frame[i]->rip);
		put_symbol(ddb_frame[i]->rip);
		cons_puts("\r\n");
		if (cpu_data[i].active_thread != 0)
			describe_thread((uint64_t)(uintptr_t)
					cpu_data[i].active_thread);
	}
}

void ddb_enter(struct trap_frame *frame, const char *why)
{
	unsigned	me = cpu_number();
	int		was_in = in_ddb[me];

	/*
	 * Marked before anything is printed, so that a tick arriving while the
	 * prompt is open cannot poll its way into a second one underneath it.
	 * Restored rather than cleared, because a fault taken INSIDE the
	 * debugger legitimately opens a nested prompt and the outer one is
	 * still there when it leaves.
	 */
	/*
	 * ⚠️ One debugger at a time.  A processor that arrives while another
	 * has the machine does NOT open a second prompt -- it parks exactly
	 * where an NMI would have put it, and carries on when released.  Its
	 * reason is dropped, which is right: the operator is already looking
	 * at the machine, and this processor's state is in `p'.
	 */
	if (!was_in && !claim_machine()) {
		park_until_released(frame);
		return;
	}

	in_ddb[me] = 1;

	/*
	 * ⚠️ Only the outermost entry stops the others.  A fault taken INSIDE
	 * the debugger opens a nested prompt, and a nested stop would send
	 * NMIs to processors already parked in one -- which they cannot take,
	 * because an NMI is blocked until the first one returns.
	 */
	if (!was_in)
		ddb_stop_others();

	ddb_enter_body(frame, why);

	arm_breakpoints();

	if (!was_in)
		ddb_release_others();

	in_ddb[me] = was_in;
}

static void ddb_enter_body(struct trap_frame *frame, const char *why)
{
	cons_puts("\r\nddb: ");
	cons_puts(why);
	cons_puts("\r\n");
	show_registers(frame);

	for (;;) {
		const char *p;
		uint64_t arg = 0;

		cons_puts("ddb> ");
		p = skip_spaces(ddb_readline());

		switch (*p) {
		case '\0':
			break;

		case 'r':
			show_registers(frame);
			break;

		case 't':
			trace(parse_hex(p + 1, &arg) ? arg : frame->rbp);
			break;

		case 'x':
			if (parse_hex(p + 1, &arg))
				examine(arg);
			else
				cons_puts("  x needs an address\r\n");
			break;

		case 's':
			if (parse_hex(p + 1, &arg)) {
				cons_puts("  ");
				cons_puthex64(arg);
				put_symbol(arg);
				cons_puts("\r\n");
			} else {
				cons_puts("  s needs an address\r\n");
			}
			break;

		case 'p':
			show_processors();
			break;

		case 'b':
			if (parse_hex(p + 1, &arg))
				set_breakpoint(arg);
			else
				show_breakpoints();
			break;

		case 'd':
			if (parse_hex(p + 1, &arg))
				clear_breakpoint(arg);
			else
				cons_puts("  d needs a breakpoint number — "
					  "try b\r\n");
			break;

		case 'l':
			list_threads();
			break;

		case 'T':
			if (parse_hex(p + 1, &arg))
				describe_thread(arg);
			else
				cons_puts("  T needs a thread address — "
					  "try p\r\n");
			break;

		case 'P':
			if (parse_hex(p + 1, &arg) && arg < NCPUS
			    && ddb_parked[arg]) {
				show_registers(ddb_frame[arg]);
				trace(ddb_frame[arg]->rbp);
			} else {
				cons_puts("  P needs the number of a parked "
					  "processor — try p\r\n");
			}
			break;

		case 'c':
			cons_puts("ddb: continuing\r\n");
			return;

		case 'h':
			cons_puts("ddb: halted\r\n");
			for (;;)
				__asm__ volatile("cli; hlt");

		default:
			usage();
			break;
		}
	}
}
