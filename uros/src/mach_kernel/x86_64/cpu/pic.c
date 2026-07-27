/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The legacy interrupt controller, silenced (#438).
 */

#include <stdint.h>

#include <cpu/pic.h>
#include <cpu/regs.h>

#define PIC1_COMMAND	0x20
#define PIC1_DATA	0x21
#define PIC2_COMMAND	0xA0
#define PIC2_DATA	0xA1

/*
 * The initialisation sequence.  Writing the first word puts a controller in
 * a state where the next three writes to its data port are taken as the
 * vector base, the wiring, and the mode — in that order and only in that
 * order, which is why the two controllers are programmed interleaved below
 * rather than one after the other.
 */
#define ICW1_INIT	0x11	/* begin, and expect a fourth word */
#define ICW3_PRIMARY	0x04	/* the secondary is on line 2      */
#define ICW3_SECONDARY	0x02	/* ...and this is which one it is  */
#define ICW4_8086	0x01	/* 8086 mode, not the 8080 one     */

/*
 * A port write to a controller this old is not finished when the
 * instruction is.  Writing to an unused port is the conventional way to
 * wait, and costs the bus cycle that is the actual delay.
 */
static void io_wait(void)
{
	outb(0x80, 0);
}

void pic_disable(void)
{
	outb(PIC1_COMMAND, ICW1_INIT);
	io_wait();
	outb(PIC2_COMMAND, ICW1_INIT);
	io_wait();

	outb(PIC1_DATA, PIC_VECTOR_BASE);
	io_wait();
	outb(PIC2_DATA, PIC_VECTOR_BASE + 8);
	io_wait();

	outb(PIC1_DATA, ICW3_PRIMARY);
	io_wait();
	outb(PIC2_DATA, ICW3_SECONDARY);
	io_wait();

	outb(PIC1_DATA, ICW4_8086);
	io_wait();
	outb(PIC2_DATA, ICW4_8086);
	io_wait();

	/*
	 * Every line masked.  The move above was so that what gets through
	 * anyway lands somewhere harmless; this is so that nothing should.
	 */
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);
}
