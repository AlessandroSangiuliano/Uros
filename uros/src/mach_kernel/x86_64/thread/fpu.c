/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Floating-point and vector state (#408, MD contract 3/6).
 *
 * The decision — eager rather than lazy — and why, is in <thread/fpu.h>.
 */

#include <stdint.h>

#include <cpu/regs.h>
#include <thread/fpu.h>
#include <trap/trap.h>

/*
 * Decided once by the boot processor and read by every other: the
 * processors in a machine agree about this, and a per-CPU answer would be a
 * per-CPU way to disagree.
 */
static uint64_t area_size = 512;	/* the legacy form, until asked */
static int use_xsave;
static int use_xsaveopt;

uint64_t fpu_area_size(void)
{
	return area_size;
}

int fpu_uses_xsave(void)
{
	return use_xsave;
}

const char *fpu_save_instruction(void)
{
	if (use_xsaveopt)
		return "XSAVEOPT";
	return use_xsave ? "XSAVE" : "FXSAVE";
}

void fpu_init(void)
{
	uint64_t cr0 = read_cr0();
	uint64_t cr4 = read_cr4();
	uint32_t a, b, c, d;

	/*
	 * EM says "there is no unit, emulate it" and must be clear or every
	 * instruction faults.  MP pairs with TS, which is exactly the
	 * mechanism this design does not use — so TS is cleared and stays
	 * cleared, and the register file is simply always live.
	 */
	cr0 &= ~(CR0_EM | CR0_TS);
	cr0 |= CR0_MP;
	write_cr0(cr0);

	/*
	 * OSFXSR is the kernel telling the processor it will save and restore
	 * the SSE registers; without it SSE instructions raise an invalid
	 * opcode, which is a confusing way to learn about a missing bit.
	 * OSXMMEXCPT makes a SIMD arithmetic error arrive as itself rather
	 * than as that same invalid opcode.
	 */
	cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
	write_cr4(cr4);

	cpuid(1, &a, &b, &c, &d);
	if (!(c & (1U << 26)))
		return;			/* no XSAVE; the legacy form it is */

	write_cr4(read_cr4() | CR4_OSXSAVE);

	/*
	 * Enable the components this kernel is prepared to carry, and only
	 * those the processor offers.  Asking for one it does not have is a
	 * general protection fault, so the request is the intersection.
	 *
	 * The x87 bit is not optional — the instruction rejects a value with
	 * it clear — which is the architecture agreeing that there is no such
	 * thing as a thread without floating-point state.
	 */
	cpuid_count(0xD, 0, &a, &b, &c, &d);
	{
		uint64_t offered = ((uint64_t)d << 32) | a;
		uint64_t wanted = XCR0_X87 | XCR0_SSE | XCR0_AVX;

		xsetbv(0, offered & wanted);
	}

	/*
	 * And now ask how much room that costs.  After the enable, not
	 * before: EBX reports the size for what is *currently* enabled, so
	 * asking first would size the area for the components we had not
	 * turned on yet and the first save would write past the end.
	 */
	cpuid_count(0xD, 0, &a, &b, &c, &d);
	if (b < 512)
		panic("fpu: the processor reports a save area smaller than the legacy one");

	area_size = b;
	use_xsave = 1;

	/*
	 * And the optimised form, if the processor has it.
	 *
	 * XSAVEOPT writes only the components that have actually changed
	 * since the last restore from this same address, and skips entirely
	 * any that are still in their initial state.  Which is precisely the
	 * laziness the eager decision gave up — recovered here, done by the
	 * processor from what it knows, with no trap to take and nothing left
	 * in the registers for a speculative read to find.
	 *
	 * It is safe to skip a write only because memory already holds the
	 * right values in that case: the component was loaded from this area
	 * and has not been touched since.  So the saved image is always
	 * consistent to read, which matters for the state a debugger will
	 * eventually ask for.
	 *
	 * The standard format, deliberately — XSAVEC's compacted one saves
	 * space in the area and costs a different restore path, and space is
	 * not what is scarce here.
	 */
	cpuid_count(0xD, 1, &a, &b, &c, &d);
	if (a & (1U << 0))
		use_xsaveopt = 1;
}

/*
 * Move the whole of it, in one instruction.
 *
 * The feature mask in EDX:EAX is all ones: save everything XCR0 has
 * enabled.  Narrowing it would be choosing which components a thread is
 * allowed to have, and the kernel is in no position to know — that is the
 * thread's business, and the processor already skips what has not changed.
 *
 * Inline rather than a called function because there is nothing to call:
 * one instruction, and a call would cost more than the work.
 */
void fpu_save(void *area)
{
	if (use_xsaveopt)
		__asm__ volatile("xsaveopt (%0)"
				 : : "r"(area), "a"(0xFFFFFFFFU), "d"(0xFFFFFFFFU)
				 : "memory");
	else if (use_xsave)
		__asm__ volatile("xsave (%0)"
				 : : "r"(area), "a"(0xFFFFFFFFU), "d"(0xFFFFFFFFU)
				 : "memory");
	else
		__asm__ volatile("fxsave (%0)" : : "r"(area) : "memory");
}

void fpu_restore(const void *area)
{
	if (use_xsave)
		__asm__ volatile("xrstor (%0)"
				 : : "r"(area), "a"(0xFFFFFFFFU), "d"(0xFFFFFFFFU)
				 : "memory");
	else
		__asm__ volatile("fxrstor (%0)" : : "r"(area) : "memory");
}

/*
 * The state a thread starts with.
 *
 * Built by asking the processor for its own initial state rather than by
 * writing a constant: the layout is the processor's, the control and tag
 * words have values that mean "nothing pending", and a hand-written pattern
 * would be a guess that the restore instruction is entitled to reject.
 */
void fpu_area_init(void *area)
{
	uint8_t *bytes = area;

	for (uint64_t i = 0; i < area_size; i++)
		bytes[i] = 0;

	/*
	 * A zeroed area is not a valid legacy image: the control word must
	 * mask the exceptions and the tag word must say the stack is empty,
	 * and zero says the opposite of both — every exception unmasked, every
	 * register in use.  The first arithmetic a thread did would raise
	 * something it never asked for.
	 */
	*(uint16_t *)(bytes + 0) = 0x037F;	/* x87 control: all masked  */
	*(uint16_t *)(bytes + 4) = 0xFFFF;	/* tag: every register free */
	*(uint32_t *)(bytes + 24) = 0x1F80;	/* MXCSR: all masked        */

	/*
	 * With XSAVE the header says which components the image actually
	 * carries.  Zero means "none of them", and the restore then loads
	 * every component's *initial* state — which is exactly what a new
	 * thread wants, and cheaper than carrying a copy of it.
	 */
}
