/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The kernel's PS/2 break-key reader, and the 8042 it shares with ps2.so
 * (#335, #599).  On -K boots ddb_kbd_intr() owns IRQ 1 until a driver
 * registers it, and reads 0x64/0x60 on every interrupt.  A task that claims
 * the 8042's ports makes it stand back -- it touches neither port while the
 * claim lasts -- and gets it back when the claim goes.
 */

#ifndef	_I386_AT386_DDB_KBD_H_
#define	_I386_AT386_DDB_KBD_H_

extern int	ddb_kbd_break_enabled;		/* -K */

extern void	ddb_kbd_break_init(void);

/*
 * A task claimed ports of the 8042 (0x60 or 0x64).  Answers 1 when the
 * reader was installed and stood back, 0 when there is no reader to stand
 * back (no -K, or the 8042 did not answer at boot).
 */
extern int	ddb_kbd_8042_claimed(void);

/* The claim went: the reader reads again. */
extern void	ddb_kbd_8042_unclaimed(void);

/* IRQ 1 goes to a driver: say what the reader did while it was ours. */
extern void	ddb_kbd_irq_handed_over(void);

#endif	/* _I386_AT386_DDB_KBD_H_ */
