/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 */

/*
 * ddb_kbd.c — Minimal polled PS/2 keyboard for the kernel debugger.
 *
 * Replaces kd.c's cngetc/cnpollc/cnmaygetc for DDB and kgdb after
 * #208 retired kd.c.  Scope is deliberately small:
 *
 *   - Polled inb(0x60) reads — no IRQ wiring, no event queue, no
 *     line discipline, no Caps Lock / Num Lock state, no ANSI escape
 *     sequences.  DDB needs single characters with backspace and
 *     enter; everything else is luxury.
 *   - Scancode set 1 translation table covers letters, digits,
 *     punctuation, space, backspace, enter, and Shift modifiers.
 *     Ctrl/Alt are reported as no-op (DDB doesn't need them).
 *   - Compatible with ps2.so (userspace): DDB only runs in panic /
 *     breakpoint context where IRQs are masked, so polled reads from
 *     port 0x60 don't race with ps2.so's IRQ-driven reads.  On DDB
 *     exit, IRQs come back on and ps2.so resumes normally.
 *
 * Serial console (cons_is_com1) path is preserved: when the boot
 * picked COM1 as the console, cngetc/cnmaygetc forward to com_getc.
 * cnpollc is a no-op now that no in-kernel keyboard state needs
 * preservation across DDB entry.
 */

#include <mach/boolean.h>
#include <kern/misc_protos.h>
#include <kern/spl.h>		/* spl_t, splhigh, splx */
#include <i386/pio.h>
#include <i386/ipl.h>		/* SPL6 */
#include <chips/busses.h>	/* take_irq / reset_irq / intr_t */

extern int com_getc(boolean_t wait);
extern int cons_is_com1;

/* ============================================================
 * 8042 / scancode-set-1 polled reader.
 * ============================================================ */

#define KBD_DATA	0x60
#define KBD_STATUS	0x64
#define KBD_STAT_OBF	0x01	/* output buffer full = byte available */
#define KBD_STAT_AUX	0x20	/* byte is from mouse port (drop it)  */

/* Scancode set 1 — keysym for press codes 0x00..0x53.  Release codes
 * are press|0x80 and we drop them.  Only the keys DDB plausibly
 * needs are populated; gaps are 0 (= no character). */

#define KEY_BS		'\b'
#define KEY_TAB		'\t'
#define KEY_ENTER	'\r'
#define KEY_SHIFT	0xFE	/* internal sentinel, not emitted */

static const unsigned char scset1_plain[128] = {
	[0x01] = 0,	/* ESC — leave for DDB shortcut handling later */
	[0x02] = '1',	[0x03] = '2',	[0x04] = '3',	[0x05] = '4',
	[0x06] = '5',	[0x07] = '6',	[0x08] = '7',	[0x09] = '8',
	[0x0A] = '9',	[0x0B] = '0',	[0x0C] = '-',	[0x0D] = '=',
	[0x0E] = KEY_BS, [0x0F] = KEY_TAB,
	[0x10] = 'q',	[0x11] = 'w',	[0x12] = 'e',	[0x13] = 'r',
	[0x14] = 't',	[0x15] = 'y',	[0x16] = 'u',	[0x17] = 'i',
	[0x18] = 'o',	[0x19] = 'p',	[0x1A] = '[',	[0x1B] = ']',
	[0x1C] = KEY_ENTER,
	[0x1E] = 'a',	[0x1F] = 's',	[0x20] = 'd',	[0x21] = 'f',
	[0x22] = 'g',	[0x23] = 'h',	[0x24] = 'j',	[0x25] = 'k',
	[0x26] = 'l',	[0x27] = ';',	[0x28] = '\'',	[0x29] = '`',
	[0x2A] = KEY_SHIFT,
	[0x2B] = '\\',
	[0x2C] = 'z',	[0x2D] = 'x',	[0x2E] = 'c',	[0x2F] = 'v',
	[0x30] = 'b',	[0x31] = 'n',	[0x32] = 'm',	[0x33] = ',',
	[0x34] = '.',	[0x35] = '/',	[0x36] = KEY_SHIFT,
	[0x39] = ' ',
};

static const unsigned char scset1_shift[128] = {
	[0x02] = '!',	[0x03] = '@',	[0x04] = '#',	[0x05] = '$',
	[0x06] = '%',	[0x07] = '^',	[0x08] = '&',	[0x09] = '*',
	[0x0A] = '(',	[0x0B] = ')',	[0x0C] = '_',	[0x0D] = '+',
	[0x0E] = KEY_BS, [0x0F] = KEY_TAB,
	[0x10] = 'Q',	[0x11] = 'W',	[0x12] = 'E',	[0x13] = 'R',
	[0x14] = 'T',	[0x15] = 'Y',	[0x16] = 'U',	[0x17] = 'I',
	[0x18] = 'O',	[0x19] = 'P',	[0x1A] = '{',	[0x1B] = '}',
	[0x1C] = KEY_ENTER,
	[0x1E] = 'A',	[0x1F] = 'S',	[0x20] = 'D',	[0x21] = 'F',
	[0x22] = 'G',	[0x23] = 'H',	[0x24] = 'J',	[0x25] = 'K',
	[0x26] = 'L',	[0x27] = ':',	[0x28] = '"',	[0x29] = '~',
	[0x2A] = KEY_SHIFT,
	[0x2B] = '|',
	[0x2C] = 'Z',	[0x2D] = 'X',	[0x2E] = 'C',	[0x2F] = 'V',
	[0x30] = 'B',	[0x31] = 'N',	[0x32] = 'M',	[0x33] = '<',
	[0x34] = '>',	[0x35] = '?',	[0x36] = KEY_SHIFT,
	[0x39] = ' ',
};

static int ddb_kbd_shift;	/* 1 while either Shift held */
static int ddb_kbd_e0;		/* 1 while the next byte completes an
				 * E0-prefixed extended scancode */

/*
 * E0-prefixed extended keys we care about — translated into the
 * Ctrl-letter sequences DDB's line editor already understands
 * (see db_input.c: Ctrl-P/N history, Ctrl-B/F cursor, Ctrl-A/E
 * begin/end of line, Ctrl-D delete-char).  Anything else extended
 * is dropped — DDB's prompt isn't a full readline, and the basic
 * arrow + home/end + del set covers the practical needs.
 */
#define CTRL(c)	((c) - 'a' + 1)

static int
ddb_kbd_e0_map(unsigned char sc)
{
	switch (sc) {
	case 0x48:	return CTRL('p');	/* Up    → previous history */
	case 0x50:	return CTRL('n');	/* Down  → next history     */
	case 0x4B:	return CTRL('b');	/* Left  → cursor back      */
	case 0x4D:	return CTRL('f');	/* Right → cursor forward   */
	case 0x47:	return CTRL('a');	/* Home  → start of line    */
	case 0x4F:	return CTRL('e');	/* End   → end of line      */
	case 0x53:	return CTRL('d');	/* Delete→ delete-char      */
	default:	return 0;		/* drop */
	}
}

static int
ddb_kbd_poll(boolean_t wait)
{
	for (;;) {
		unsigned char status;
		unsigned char sc;
		unsigned char ch;
		int pressed;

		status = inb(KBD_STATUS);
		if ((status & KBD_STAT_OBF) == 0) {
			if (!wait)
				return -1;
			continue;
		}
		if (status & KBD_STAT_AUX) {
			/* Mouse byte — drain and ignore. */
			(void)inb(KBD_DATA);
			continue;
		}

		sc = inb(KBD_DATA);

		if (sc == 0xE0) {
			ddb_kbd_e0 = 1;
			continue;
		}

		pressed = (sc & 0x80) == 0;
		sc &= 0x7F;

		if (ddb_kbd_e0) {
			ddb_kbd_e0 = 0;
			if (!pressed)
				continue;	/* drop release halves */
			ch = (unsigned char)ddb_kbd_e0_map(sc);
			if (ch == 0)
				continue;
			return (int)ch;
		}

		ch = ddb_kbd_shift ? scset1_shift[sc] : scset1_plain[sc];

		if (ch == KEY_SHIFT) {
			ddb_kbd_shift = pressed ? 1 : 0;
			continue;
		}
		if (!pressed || ch == 0)
			continue;	/* release / unmapped */
		return (int)(unsigned char)ch;
	}
}

/* ============================================================
 * Public hooks consumed by DDB / kgdb (db_machdep.h, misc_protos.h).
 * ============================================================ */

/*
 * Serial terminals (-nographic + -serial mon:stdio) send VT100 escape
 * sequences for arrows / Home / End / Del.  DDB's line editor doesn't
 * speak ESC, so without this parser pressing Up just inserts "^[[A".
 *
 * We translate the canonical 3-byte sequences into the same Ctrl-letter
 * shortcuts the PS/2 path emits (see ddb_kbd_e0_map above) so history
 * recall works uniformly across graphic and serial consoles (#211).
 *
 * ESC alone (no follow-up) is rare in DDB usage; we fall through to
 * delivering the literal 0x1B which db_input.c just ignores.
 */
static int
ddb_com_getc(boolean_t wait)
{
	int c = com_getc(wait);
	if (c != 0x1B)
		return c;
	{
		int c2 = com_getc(TRUE);
		if (c2 != '[')
			return c2;	/* alt-key or stray ESC; eat it */
	}
	{
		int c3 = com_getc(TRUE);
		switch (c3) {
		case 'A': return CTRL('p');	/* Up    */
		case 'B': return CTRL('n');	/* Down  */
		case 'C': return CTRL('f');	/* Right */
		case 'D': return CTRL('b');	/* Left  */
		case 'H': return CTRL('a');	/* Home  */
		case 'F': return CTRL('e');	/* End   */
		case '3': {
			/* xterm Delete: ESC [ 3 ~ */
			int t = com_getc(TRUE);
			if (t == '~') return CTRL('d');
			return 0;
		}
		default:
			return 0;		/* drop unknown CSI */
		}
	}
}

int
cngetc(void)
{
	if (cons_is_com1)
		return ddb_com_getc(TRUE);
	return ddb_kbd_poll(TRUE);
}

int
cnmaygetc(void)
{
	if (cons_is_com1) {
		int c = com_getc(FALSE);
		if (c == 0x1B) {
			/* Don't block waiting for the rest of the CSI in
			 * polling mode — the bytes will arrive on the next
			 * poll if they're real escape sequences. */
			return c;
		}
		return c;
	}
	return ddb_kbd_poll(FALSE);
}

void
cnpollc(boolean_t on)
{
	/*
	 * Pre-#208 this saved/restored kd.c's kb_mode and mouse_in_use
	 * state so the in-kernel ANSI tty wouldn't be disturbed by DDB.
	 * Now ps2.so owns the keyboard in userspace and is suspended
	 * during DDB anyway (we're in panic / breakpoint context with
	 * IRQs masked), so there is nothing to preserve.
	 */
	(void)on;
}

/* ============================================================
 * #335 — PS/2 break-key: enter the kernel debugger from the keyboard.
 *
 * On a serial console (cons_is_com1) com.c already turns com_halt_char
 * into a kdb_kintr() (check_debugger() in comintr()).  A machine with no
 * serial port (omen, UEFI) has no such path, so a wedge is undebuggable.
 *
 * This installs a minimal IRQ-1 top half that watches for Ctrl+D and
 * calls kdb_kintr() — the exact entry comintr() uses — landing DDB on the
 * interrupted frame.  Opt-in via the -K boot flag, and only when the
 * console is not COM1.  It claims IRQ 1 with take_irq(); if the userspace
 * char_server/ps2.so later registers IRQ 1, device_intr_register() does a
 * reset_irq() before its take_irq(), so it transparently takes the line
 * over (and restores this handler on unregister) — no clobber, no hang.
 * ============================================================ */

#if	MACH_KDB || MACH_KGDB
extern void kdb_kintr(void);	/* locore.S — break into the debugger */
#endif

/* Set by the -K boot flag in parse_arguments(); .data so it survives the
 * BSS clear, like boot_cpu_cap / cons_is_com1 (#337). */
int ddb_kbd_break_enabled __attribute__((section(".data"))) = 0;

#define KBD_SC_CTRL	0x1D	/* Ctrl make; release = | 0x80 */
#define KBD_SC_D	0x20	/* 'd' make */

static int ddb_brk_ctrl;	/* Ctrl currently held */

/*
 * IRQ-1 top half.  Reached through the ivect[] dispatch (interrupt.S)
 * with interrupts enabled at SPL6 — the same context comintr() runs in,
 * so kdb_kintr() from here is safe.  We drain port 0x60 to ack the 8042;
 * the PIC / I-O APIC EOI is done by the dispatch wrapper.
 */
void
ddb_kbd_intr(int unit)
{
	unsigned char status, sc;
	(void)unit;

	status = inb(KBD_STATUS);
	if ((status & KBD_STAT_OBF) == 0)
		return;			/* nothing pending (shared / spurious) */
	if (status & KBD_STAT_AUX) {
		(void)inb(KBD_DATA);	/* mouse byte — drain and ignore */
		return;
	}
	sc = inb(KBD_DATA);

	if (sc == 0xE0)			/* extended prefix; next byte stands alone */
		return;
	if (sc == KBD_SC_CTRL)        { ddb_brk_ctrl = 1; return; }
	if (sc == (KBD_SC_CTRL|0x80)) { ddb_brk_ctrl = 0; return; }

	if (ddb_brk_ctrl && sc == KBD_SC_D) {
		ddb_brk_ctrl = 0;	/* one-shot: don't re-fire on key repeat */
#if	MACH_KDB || MACH_KGDB
		kdb_kintr();
#endif
	}
}

/* ------------------------------------------------------------------
 * Minimal 8042 controller bring-up.
 *
 * Without char_server, nothing programs the 8042: on bare metal the
 * firmware may hand us the keyboard port with its IRQ and/or clock
 * disabled, so IRQ 1 never fires and ddb_kbd_intr() never runs (this is
 * why Ctrl+D worked under QEMU — SeaBIOS pre-enables the controller —
 * but not on real hardware).  Mirror char_server/modules/ps2.c's
 * ps2_attach(): drain, set the config byte (port-1 IRQ + set-1
 * translation + keyboard clock on), re-enable port 1, enable scanning.
 * All waits are bounded so a wedged controller can't hang the boot.
 * ------------------------------------------------------------------ */
#define KBD_STAT_IBF	0x02	/* input buffer full (cmd still in flight) */
#define I8042_DISABLE_P1 0xAD
#define I8042_DISABLE_P2 0xA7
#define I8042_ENABLE_P1	0xAE
#define I8042_READ_CFG	0x20
#define I8042_WRITE_CFG	0x60
#define KBD_ENABLE_SCAN	0xF4
#define I8042_SPINS	100000

static int
ddb_8042_in_empty(void)
{
	int i;
	for (i = 0; i < I8042_SPINS; i++)
		if ((inb(KBD_STATUS) & KBD_STAT_IBF) == 0)
			return 0;
	return -1;
}

static int
ddb_8042_out_full(void)
{
	int i;
	for (i = 0; i < I8042_SPINS; i++)
		if (inb(KBD_STATUS) & KBD_STAT_OBF)
			return 0;
	return -1;
}

static void
ddb_8042_cmd(unsigned char c)
{
	if (ddb_8042_in_empty() == 0)
		outb(KBD_STATUS, c);
}

static void
ddb_8042_data(unsigned char v)
{
	if (ddb_8042_in_empty() == 0)
		outb(KBD_DATA, v);
}

static void
ddb_8042_kbd_enable(void)
{
	unsigned char cfg;
	int i;

	ddb_8042_cmd(I8042_DISABLE_P1);
	ddb_8042_cmd(I8042_DISABLE_P2);
	for (i = 0; i < 16; i++) {		/* drain stale OBF bytes */
		if ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
			break;
		(void)inb(KBD_DATA);
	}

	ddb_8042_cmd(I8042_READ_CFG);
	if (ddb_8042_out_full() < 0)
		return;
	cfg = inb(KBD_DATA);
	cfg |= 0x01;	/* enable port-1 (keyboard) interrupt -> IRQ 1 */
	cfg |= 0x40;	/* translate to scancode set 1 (our tables) */
	cfg &= ~0x10;	/* clear "disable port-1 clock" -> keyboard on */
	ddb_8042_cmd(I8042_WRITE_CFG);
	ddb_8042_data(cfg);

	ddb_8042_cmd(I8042_ENABLE_P1);

	ddb_8042_data(KBD_ENABLE_SCAN);		/* 0xF4 */
	if (ddb_8042_out_full() == 0)
		(void)inb(KBD_DATA);		/* eat the 0xFA ACK */
}

/*
 * Arm the break-key.  Called once from machine_init() after probeio(), so
 * the PIC / I-O APIC and every device IRQ are configured and IRQ 1 is
 * still free.  IRQ 1 is seeded intnull@SPL6 in pic_isa.c, so reset_irq()
 * first clears intpri[1] — otherwise take_irq() sees SPL6 != 0 and spins
 * thinking two devices share the line.
 */
void
ddb_kbd_break_init(void)
{
	spl_t	s;
	int	o_unit, o_spl;
	intr_t	o_handler;

	if (!ddb_kbd_break_enabled)
		return;
	if (cons_is_com1)		/* serial already has com_halt_char */
		return;

	s = splhigh();
	ddb_8042_kbd_enable();		/* make the 8042 deliver IRQ 1 */
	reset_irq(1, &o_unit, &o_spl, &o_handler);
	take_irq(1, 1, SPL6, (intr_t)ddb_kbd_intr);
	splx(s);

	printf("DDB: press Ctrl+D on the PS/2 keyboard to enter the debugger\n");
}
