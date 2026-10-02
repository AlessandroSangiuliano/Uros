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
 *   - Shared with ps2.so (userspace).  DDB's own polled reads happen in
 *     panic / breakpoint context with the other processors parked, and
 *     are the stated exception: a debugger entered in the middle of a
 *     driver's command can eat an answer, and the driver's bounded waits
 *     turn that into a line, not a hang.  The break-key reader on IRQ 1
 *     is NOT that context: it runs whenever the line fires, and until
 *     ps2.so registers IRQ 1 it would read the answers to ps2.so's own
 *     commands.  #599: it stands back while a task holds the 8042's
 *     ports (ddb_kbd_8042_recompute below).
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
#include <i386/AT386/ddb_kbd.h>
#include <device/device_machdep.h>	/* device_io_port_held (#599) */

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
	 * Now ps2.so owns the keyboard in userspace, so there is nothing to
	 * preserve.  #599: DDB's polled reads of the 8042 are the stated
	 * exception -- see the note at the top of this file.
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
 * interrupted frame.  Opt-in via the -K boot flag.  It claims IRQ 1 with
 * take_irq(); if the userspace char_server/ps2.so later registers IRQ 1,
 * device_intr_register() does a reset_irq() before its take_irq(), so it
 * takes the line over (and restores this handler on unregister).  That is
 * clean for the LINE and says nothing about the ports: until the line moves,
 * this reader and ps2.so's commands share 0x60/0x64, which is what #599's
 * claim below is for.
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
 * #599: the reader and a task's claim on the 8042, under one leaf lock.
 *
 * A mask set on another processor must exclude a reader that has already
 * passed its test, so the test and the reads are one hold: pushfl; cli;
 * xchgb, the shape of 77ae0c7d.  Nothing else is taken inside, and
 * kdb_kintr() is called after the hold ends.
 *
 * ddb_kbd_armed: the reader is installed on IRQ 1 -- whether or not the 8042
 * answered at boot, it reads the ports whenever the line fires, so that is
 * when there is a reader to stand back.  The two counters are printed when
 * IRQ 1 goes to a driver and when a claim goes.
 */
static volatile unsigned char	i8042_lock;
static volatile int		i8042_claimed;
static int			ddb_kbd_armed;
unsigned int			ddb_kbd_stood_back;
unsigned int			ddb_kbd_bytes_taken;

static unsigned int
i8042_enter(void)
{
	unsigned int	flags;
	unsigned char	busy;

	__asm__ volatile("pushfl; popl %0; cli" : "=r" (flags) : : "memory");
	for (;;) {
		busy = 1;
		__asm__ volatile("xchgb %0, %1"
				 : "+q" (busy), "+m" (i8042_lock)
				 : : "memory");
		if (busy == 0)
			break;
		__asm__ volatile("pause");
	}
	return flags;
}

static void
i8042_leave(unsigned int flags)
{
	__asm__ volatile("" : : : "memory");
	i8042_lock = 0;
	__asm__ volatile("pushl %0; popfl" : : "r" (flags) : "memory", "cc");
}

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
	unsigned int flags;
	int fire = 0;
	(void)unit;

	flags = i8042_enter();
	if (i8042_claimed) {
		/* #599: a task holds the 8042; its bytes are not ours */
		ddb_kbd_stood_back++;
		i8042_leave(flags);
		return;
	}
	status = inb(KBD_STATUS);
	if ((status & KBD_STAT_OBF) == 0) {
		i8042_leave(flags);	/* nothing pending (shared / spurious) */
		return;
	}
	sc = inb(KBD_DATA);
	ddb_kbd_bytes_taken++;
	if (status & KBD_STAT_AUX)
		;			/* mouse byte — drained and ignored */
	else if (sc == 0xE0)
		;			/* extended prefix; next byte stands alone */
	else if (sc == KBD_SC_CTRL)
		ddb_brk_ctrl = 1;
	else if (sc == (KBD_SC_CTRL|0x80))
		ddb_brk_ctrl = 0;
	else if (ddb_brk_ctrl && sc == KBD_SC_D) {
		ddb_brk_ctrl = 0;	/* one-shot: don't re-fire on key repeat */
		fire = 1;
	}
	i8042_leave(flags);

#if	MACH_KDB || MACH_KGDB
	if (fire)
		kdb_kintr();
#else
	(void)fire;
#endif
}

/*
 * #599: the claim as device_master's table has it.  Called after every claim
 * or unclaim of a range touching 0x60 or 0x64 (device_md_io_claimed/unclaimed
 * in i386/device_machdep.c).  The mask is recomputed from the table under
 * i8042_lock -- the table read inside the hold -- so an identical re-claim
 * counts once, and hooks that run in another order than their table changes
 * (they run after device_table_lock is let go, on any processor) still leave
 * the mask as the table is: whichever runs last reads the last change.  Found
 * in review: a counter here was bumped twice by a re-claim and left the
 * reader standing back with nothing claimed.
 *
 * When the claim goes and IRQ 1 is the reader's, a byte the reader stood back
 * from is still in the output buffer, holding the edge-triggered line up, and
 * no interrupt would ever come again: it is drained, and counted.  If IRQ 1
 * is not the reader's -- a driver took it, and a driver that dies has its line
 * masked rather than given back -- the reader does not read again, and the
 * line says so.
 *
 * Answers whether the reader stands back now.  Under ABLATE_599_I8042_SHARED
 * it never does: the claim is answered 0, the reader goes on reading the
 * task's bytes, and the line says the ablation is on.
 */
unsigned int	ddb_kbd_bytes_dropped;	/* left by a claim, drained after */

int
ddb_kbd_8042_recompute(void)
{
	unsigned int flags;
	int held, was, now, reader, drained = 0;
	static int ablation_said;

	if (!ddb_kbd_break_enabled || !ddb_kbd_armed)
		return 0;
	flags = i8042_enter();
	held = device_io_port_held(KBD_DATA) || device_io_port_held(KBD_STATUS);
	was = i8042_claimed;
#ifndef	ABLATE_599_I8042_SHARED
	i8042_claimed = held;
#endif
	now = i8042_claimed;
	reader = ivect[1] == (intr_t)ddb_kbd_intr;
	if (was && !now) {
		ddb_brk_ctrl = 0;	/* a Ctrl release it never saw */
		if (reader && (inb(KBD_STATUS) & KBD_STAT_OBF)) {
			(void)inb(KBD_DATA);
			ddb_kbd_bytes_dropped++;
			drained = 1;
		}
	}
	i8042_leave(flags);

	if (!was && now)
		printf("DDB: the 8042 is claimed by a task — the break-key "
		       "reader stands back (it had taken %u bytes) (#599)\n",
		       ddb_kbd_bytes_taken);
	else if (was && !now && reader)
		printf("DDB: the 8042 is the kernel's again — while it was "
		       "claimed the break-key reader stood back %u times; it "
		       "has taken %u bytes%s (#599)\n", ddb_kbd_stood_back,
		       ddb_kbd_bytes_taken, drained ?
		       ", and drained one the claim left pending" : "");
	else if (was && !now)
		printf("DDB: no task claims the 8042 now, but IRQ 1 is not the "
		       "break-key reader's -- a driver took it and did not give "
		       "it back -- so the reader does not read again this boot "
		       "(#599)\n");
	else if (!now && held && !ablation_said) {
		ablation_said = 1;
		printf("DDB: ABLATE_599_I8042_SHARED -- the 8042 is claimed and "
		       "the break-key reader goes on reading it (#599)\n");
	}
	return now;
}

/*
 * #599: IRQ 1 goes to a driver.  The window in which this reader and the
 * driver's commands shared the 8042 ends here, so what it did in it is said
 * here, in a line a boot's log keeps.
 */
void
ddb_kbd_irq_handed_over(void)
{
	if (!ddb_kbd_break_enabled || !ddb_kbd_armed)
		return;
	ddb_brk_ctrl = 0;	/* the driver sees the releases from here */
	printf("DDB: IRQ 1 goes to a driver — until now the break-key reader "
	       "stood back %u times and took %u bytes (#599)\n",
	       ddb_kbd_stood_back, ddb_kbd_bytes_taken);
}

/* ------------------------------------------------------------------
 * Minimal 8042 controller bring-up.
 *
 * Without char_server, nothing programs the 8042: on bare metal the
 * firmware may hand us the keyboard port with its IRQ and/or clock
 * disabled, so IRQ 1 never fires and ddb_kbd_intr() never runs (this is
 * why Ctrl+D worked under QEMU — SeaBIOS pre-enables the controller —
 * but not on real hardware).  After char_server/modules/ps2.c's
 * ps2_attach(): drain, set the config byte (port-1 IRQ + set-1
 * translation; the keyboard's clock stays off across the read-back,
 * #599), read it back, re-enable port 1 -- which turns the clock on --
 * and enable scanning.
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

static int
ddb_8042_cmd(unsigned char c)
{
	if (ddb_8042_in_empty() < 0)
		return -1;
	outb(KBD_STATUS, c);
	return 0;
}

static int
ddb_8042_data(unsigned char v)
{
	if (ddb_8042_in_empty() < 0)
		return -1;
	outb(KBD_DATA, v);
	return 0;
}

/*
 * #599: answers the configuration byte READ BACK after it was written, or -1
 * when the 8042 did not answer; *ack is the keyboard's answer to
 * enable-scan, or -1 when none came.  It gave up in silence, and the boot
 * line promised a door the controller may not have opened.
 */
static int
ddb_8042_kbd_enable(int *ack)
{
	unsigned char cfg;
	int i;

	*ack = -1;
	if (ddb_8042_cmd(I8042_DISABLE_P1) < 0 ||
	    ddb_8042_cmd(I8042_DISABLE_P2) < 0)
		return -1;
	for (i = 0; i < 16; i++) {		/* drain stale OBF bytes */
		if ((inb(KBD_STATUS) & KBD_STAT_OBF) == 0)
			break;
		(void)inb(KBD_DATA);
	}

	if (ddb_8042_cmd(I8042_READ_CFG) < 0 || ddb_8042_out_full() < 0)
		return -1;
	cfg = inb(KBD_DATA);
	cfg |= 0x01;	/* enable port-1 (keyboard) interrupt -> IRQ 1 */
	cfg |= 0x40;	/* translate to scancode set 1 (our tables) */
	/*
	 * #599: the port-1 clock stays off (bit 4, as DISABLE_P1 left it)
	 * across the read-back below, so the byte that answers it is the
	 * controller's and not a key the keyboard sent in between (found in
	 * review).  ENABLE_P1 turns the clock on after it.
	 */
	if (ddb_8042_cmd(I8042_WRITE_CFG) < 0 || ddb_8042_data(cfg) < 0)
		return -1;
	if (ddb_8042_cmd(I8042_READ_CFG) < 0 || ddb_8042_out_full() < 0)
		return -1;
	cfg = inb(KBD_DATA);			/* what it holds now */

	if (ddb_8042_cmd(I8042_ENABLE_P1) < 0 ||
	    ddb_8042_data(KBD_ENABLE_SCAN) < 0)	/* 0xF4 */
		return -1;
	if (ddb_8042_out_full() == 0)
		*ack = inb(KBD_DATA);		/* 0xFA, as read */
	return cfg;
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
	int	o_unit, o_spl, cfg, ack;
	intr_t	o_handler;

	if (!ddb_kbd_break_enabled)
		return;
	/*
	 * #382: do NOT skip when the console is serial.  This used to bail
	 * out with "serial already has com_halt_char", but the serial
	 * Ctrl-_ break is broken (kdb_kintr's frame walk no longer matches
	 * the reworked interrupt path), so a -r boot ended up with no
	 * working DDB door at all.  Arm the PS/2 Ctrl+D break whenever -K
	 * asks for it; with cons_is_com1 the DDB session I/O still goes to
	 * the serial console, which is exactly the headless-QEMU debug
	 * flow (sendkey ctrl-d from the monitor).
	 */

	s = splhigh();
	cfg = ddb_8042_kbd_enable(&ack);	/* make the 8042 deliver IRQ 1 */
	reset_irq(1, &o_unit, &o_spl, &o_handler);
	take_irq(1, 1, SPL6, (intr_t)ddb_kbd_intr);
	ddb_kbd_armed = 1;		/* installed, answer or not */
	splx(s);

	if (cfg < 0) {
		printf("DDB: the 8042 did not answer — the PS/2 break key will "
		       "not work (#599)\n");
		return;
	}
	if (ack >= 0)
		printf("DDB: press Ctrl+D on the PS/2 keyboard to enter the "
		       "debugger (config 0x%02x read back, the keyboard answered "
		       "0x%02x)\n", (unsigned)cfg, (unsigned)ack);
	else
		printf("DDB: press Ctrl+D on the PS/2 keyboard to enter the "
		       "debugger (config 0x%02x read back; the keyboard did not "
		       "answer enable-scan)\n", (unsigned)cfg);
}
