/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The 8254 interval timer, used only as a ruler (#409).
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <time/pit.h>

#define PIT_CHANNEL2	0x42
#define PIT_COMMAND	0x43

/*
 * The port that gates channel 2 and connects it to the speaker.  Bit 0 is
 * the gate — the counter does not count while it is clear — and bit 1 is the
 * speaker itself.  Bit 5 is the counter's output, which is what says the
 * countdown has finished, and it is read-only.
 */
#define PIT_GATE_PORT	0x61
#define PIT_GATE_ENABLE	0x01
#define PIT_SPEAKER_ON	0x02
#define PIT_OUTPUT	0x20

/*
 * Channel 2, access both bytes, mode 0, binary.
 *
 * Mode 0 is "interrupt on terminal count", which despite the name raises no
 * interrupt here — channel 2 has no interrupt line.  What it does is hold the
 * output low while counting and take it high when the count reaches zero,
 * which is precisely the one-shot edge this needs and is the only mode that
 * gives it.  The periodic modes keep reloading and never produce a lasting
 * edge to poll for.
 */
#define PIT_MODE0_CH2	0xB0

int pit_delay_us(unsigned us)
{
	uint32_t count = ((uint64_t)PIT_HZ * us) / 1000000u;
	uint8_t gate;

	/*
	 * Sixteen bits, and a zero count means 65536 rather than nothing — so
	 * both ends are refused rather than wrapped into a plausible answer.
	 */
	if (us == 0 || us > PIT_MAX_US || count == 0 || count > 0xFFFF)
		return 0;

	/*
	 * Gate off first, so the counter is not running while it is being
	 * loaded, and the speaker off with it: a calibration has no business
	 * being audible.
	 */
	gate = inb(PIT_GATE_PORT);
	gate = (gate & ~(PIT_GATE_ENABLE | PIT_SPEAKER_ON));
	outb(PIT_GATE_PORT, gate);

	outb(PIT_COMMAND, PIT_MODE0_CH2);
	outb(PIT_CHANNEL2, (uint8_t)count);
	outb(PIT_CHANNEL2, (uint8_t)(count >> 8));

	/*
	 * And now the gate, which is the start.  Everything above happens with
	 * the counter stopped, so the interval being measured begins at this
	 * store and not at some point during the setup.
	 */
	outb(PIT_GATE_PORT, gate | PIT_GATE_ENABLE);

	/*
	 * Mode 0 holds the output low from the moment the count is loaded and
	 * raises it at terminal count, so this waits for the rising edge.
	 *
	 * No bound on the loop, deliberately.  A bound would need a clock, and
	 * a clock is what this exists to build; a bound in iterations would be
	 * a number that means something different on every machine.  If the
	 * 8254 does not count, the boot stops here with the last line on the
	 * wire naming the calibration — which is a diagnosis, where a timeout
	 * returning a wrong frequency would be a lie propagated into every
	 * measurement afterwards.
	 */
	while ((inb(PIT_GATE_PORT) & PIT_OUTPUT) == 0)
		;

	outb(PIT_GATE_PORT, gate);
	return 1;
}
