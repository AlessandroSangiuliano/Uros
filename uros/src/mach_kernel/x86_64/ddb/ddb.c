/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The debugger (#428).
 */

#include <stdint.h>

#include <boot/multiboot2.h>
#include <ddb/cons.h>
#include <ddb/ddb.h>
#include <ddb/ksym.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <trap/trap.h>

static int enabled;

void ddb_init(uint32_t info_pa)
{
	const struct mb2_tag_string *t;
	const char *p;

	t = (const struct mb2_tag_string *)mb2_find_tag(info_pa,
						        MB2_TAG_CMDLINE);
	if (t == 0)
		return;

	/*
	 * A flag is a `-r` standing on its own, not an `r` anywhere in the
	 * string: a path or a device name in the command line would otherwise
	 * turn the debugger on by containing the wrong letter.
	 */
	for (p = t->string; *p; p++) {
		if (p[0] != '-' || p[1] != 'r')
			continue;
		if (p[2] == '\0' || p[2] == ' ')
			enabled = 1;
	}
}

int ddb_enabled(void)
{
	return enabled;
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

void ddb_enter(struct trap_frame *frame, const char *why)
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
