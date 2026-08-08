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
#include <kern/cpu_number.h>
#include <mach/boolean.h>
#include <kern/misc_protos.h>	/* panic */
#include <mach/machine.h>	/* machine_slot[] */
#include <mach/machine/vm_types.h>
#include <cpu/lapic.h>		/* lapic_send_nmi */
#include <cpu/regs.h>		/* cpu_pause */
#include <pmap/layout.h>
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
static volatile int		ddb_owner = -1;
static volatile int		ddb_parked[NCPUS];
static struct trap_frame	*ddb_frame[NCPUS];

/*
 * Called from the NMI arm of trap_dispatch().  Answers TRUE when this NMI was
 * the debugger asking, and the processor has parked and been released; FALSE
 * when it is a real one and belongs to whoever reports them.
 */
boolean_t ddb_park_here(struct trap_frame *frame)
{
	unsigned me = cpu_number();

	if (ddb_owner < 0 || ddb_owner == (int) me)
		return FALSE;

	ddb_frame[me] = frame;
	ddb_parked[me] = 1;

	while (ddb_owner >= 0 && ddb_owner != (int) me)
		cpu_pause();

	ddb_parked[me] = 0;
	ddb_frame[me] = (struct trap_frame *) 0;
	return TRUE;
}

static void ddb_stop_others(void)
{
	unsigned	me = cpu_number();
	uint64_t	spins;

	ddb_owner = (int) me;

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
	ddb_owner = -1;
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
