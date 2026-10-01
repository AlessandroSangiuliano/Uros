/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The kernel's PS/2 break-key reader, and the 8042 it shares with ps2.so
 * (#335, #599).  On -K boots ddb_kbd_intr() owns IRQ 1 until a driver
 * registers it, and reads 0x64/0x60 on every interrupt.  A task that claims
 * the 8042's ports makes it stand back -- it touches neither port while the
 * claim lasts.  When the claim goes the reader reads again only if IRQ 1 is
 * still its own: a driver that took the line and died has it masked, not
 * given back.
 */

#ifndef	_I386_AT386_DDB_KBD_H_
#define	_I386_AT386_DDB_KBD_H_

extern int	ddb_kbd_break_enabled;		/* -K */

extern void	ddb_kbd_break_init(void);

/*
 * A claim of 0x60 or 0x64 was made or given back: the reader stands back or
 * not as device_master's table now says.  Answers 1 when the reader is
 * installed (on -K boots, whether or not the 8042 answered at boot) and
 * stands back, 0 otherwise.  See ddb_kbd.c.
 */
extern int	ddb_kbd_8042_recompute(void);

/* IRQ 1 goes to a driver: say what the reader did while it was ours. */
extern void	ddb_kbd_irq_handed_over(void);

#endif	/* _I386_AT386_DDB_KBD_H_ */
