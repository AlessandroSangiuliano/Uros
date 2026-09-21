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
static int use_xsavec;
static int use_xsaves;
static int have_xsaves_unused;	/* offered by the processor, not taken */

/*
 * 🔴 THE SAVE AND THE RESTORE ARE ONE DECISION, NOT TWO (#561).
 *
 * XSAVEC and XSAVES write the COMPACTED format; XSAVE and XSAVEOPT write the
 * standard one.  XRSTOR reads either -- it looks at XCOMP_BV -- but XRSTORS
 * reads ONLY the compacted one and faults on a standard image.  So choosing
 * the save chooses the restore, and it also chooses what fpu_area_init() must
 * leave behind: a thread's FIRST restore reads an area no save has ever
 * written, and an image in the wrong format faults on the first switch to
 * every new thread.
 */
#define	XSTATE_BV_OFFSET	512	/* which components the image carries */
#define	XCOMP_BV_OFFSET		520	/* bit 63: the image is compacted     */
#define	XCOMP_BV_COMPACTED	(1ULL << 63)

#define	MSR_IA32_XSS		0xDA0

/*
 * 🔴 XSAVES HAS NOW BEEN RUN, AND IT IS DECLINED FOR A MEASURED REASON RATHER
 * THAN FOR AN UNEXERCISED ONE (#561).
 *
 * It is the best rung on paper: the compacted format AND the modified
 * optimisation, where XSAVEC has only the first and XSAVEOPT only the second.
 * The five steps written here used to end "until step 3 has happened this
 * stays 0", and they have now all happened -- not on OMEGA, which was the only
 * candidate known when they were written, but on a machine that arrived after:
 *
 *	simo-victus, AMD Ryzen 5 5600H (Zen 3), which offers xsaves where
 *	pavillion's Ryzen 5 4600H does not.  qemu passes it through under KVM
 *	(`-cpu max' and `-cpu host' both), and still does not offer it under
 *	TCG -- asked, both ways, rather than assumed.
 *
 * 🔑 THE RESULT IS THE ONE "SHOULD" DID NOT PREDICT, FOR THE SECOND TIME.
 *
 * Same kernel, the feature offered or withheld by qemu so that only the rung
 * differs -- the method XSAVEC was measured with, below -- median of 5 boots
 * each, -smp 1, KVM, bench-only bundle, the #554 futex ping-pong:
 *
 *	XSAVES/XRSTORS    579.7 ns    spread 545.6..597.6
 *	XSAVEOPT/XRSTOR   575.9 ns    spread 559.7..594.9
 *
 * 3.8 ns apart with the spreads lying on top of one another.  Having BOTH
 * properties bought nothing measurable over having the modified optimisation
 * alone, for the same reason compaction bought nothing for XSAVEC: these
 * threads use the components, so there is little to compact, and what is left
 * is the save the modified optimisation already skips.
 *
 * So the rule that settled XSAVEC settles this one the same way --
 * INDISTINGUISHABLE IS NOT A REASON TO CHANGE A DEFAULT -- and XSAVEOPT keeps
 * the place it had.
 *
 * ⚠️ What was verified, rather than what was hoped:
 *	fpu_stress PASSES on XSAVES/XRSTORS (-F, -smp 4) -- the test that a
 *	thread's sixteen vector registers survive a switch, and the only thing
 *	that tells a correct compacted header from a lucky one;
 *	the whole ladder was re-walked with it at 1 -- six rungs, each
 *	announcing its own and each passing.
 *
 * 🔑 AND THIS KNOB IS WHY THE RUNG CAN BE WALKED AGAIN.  It stays, and it
 * stays a knob rather than becoming a deletion, because the objection that
 * opened this was never "XSAVES is slow" -- it was that a branch nobody has
 * executed is not support.  Set it to 1 and scripts/xsave-ladder.sh walks the
 * whole ladder from the top -- it moved into the tree with #563, so it travels
 * to every machine instead of to whoever happened to have a copy.  Leave it 0
 * and the processor's offer is announced and declined, which is what a machine
 * that has XSAVES now says.
 *
 * ⚠️ AND THE CLOCK THESE NUMBERS WERE TAKEN AT IS NOT WHAT THE HARNESS SAID.
 * simo-victus drives amd-pstate-epp, where `powersave' is the ACTIVE mode and
 * scales up -- sampled at 3.7-3.9 GHz during the runs -- and not the passive
 * governor that pins to the floor.  scripts/run-conditions.sh exempts
 * intel_pstate from that reading and not amd-pstate, so it reported
 * `effective=1108MHz' for a processor running at nearly four times that.  It
 * is its own defect and it is not this one; recorded here because a number is
 * only as good as the condition written beside it.
 */
#define	FPU_ALLOW_XSAVES	0

uint64_t fpu_area_size(void)
{
	return area_size;
}

int fpu_uses_xsave(void)
{
	return use_xsave;
}

/*
 * Whether the restore will be XRSTORS, which is the question fpu_area_init()
 * has to answer before it writes a header (#561).
 */
int fpu_uses_xsaves(void)
{
	return use_xsaves;
}

const char *fpu_save_instruction(void)
{
	if (use_xsaves)
		return "XSAVES/XRSTORS";
	/*
	 * ⚠️ Said out loud when the processor offers a rung this kernel is not
	 * taking, so that a log from a machine with XSAVES does not look like a
	 * machine without it.
	 */
	if (use_xsaveopt)
		return have_xsaves_unused ? "XSAVEOPT/XRSTOR (XSAVES present, "
					    "measured and not chosen — see fpu.c)"
					  : "XSAVEOPT/XRSTOR";
	if (use_xsavec)
		return "XSAVEC/XRSTOR";
	return use_xsave ? "XSAVE/XRSTOR" : "FXSAVE/FXRSTOR";
}

/*
 * See the comment on the declaration in fpu.h: read the status word that
 * faulted, then clear the condition so that the thread this fault belongs to
 * can be resumed at the instruction that reported it (#515).
 *
 * ⚠️ FNSTSW and FNCLEX are the non-waiting forms deliberately.  The waiting
 * ones check for a pending unmasked exception before executing, which here is
 * precisely the exception being handled -- so FSTSW would raise #MF again,
 * inside the handler for #MF.
 */
unsigned short fpu_take_x87_error(void)
{
	unsigned short status;

	__asm__ volatile ("fnstsw %0" : "=am" (status));
	__asm__ volatile ("fnclex");
	return status;
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
	 *
	 * 🔴 NE is set here, and was never set on this target either (#515).
	 * This kernel maps T_FPU_ERROR to EXC_ARITHMETIC/EXC_X86_64_EXTERRFLT
	 * and installs a gate at vector 16 — and with NE clear the processor
	 * never uses it: an x87 numeric error asserts FERR# instead, which is
	 * a 387-era signal meant for a PIC this kernel does not drive.  So the
	 * handler existed and could not fire, and a thread that took such an
	 * error stayed in the instruction that reported it.
	 *
	 * ⚠️ The boot processor inherits whatever the firmware left in CR0, so
	 * it was a measurement; the APs were a proof.  ap_trampoline.S builds
	 * CR0 from the reset value plus PE and then PG, and NE is 0 in the
	 * reset value — so every AP came up without it however the BSP arrived.
	 *
	 * 🔑 Set beside OSXMMEXCPT below for the reason that pairs them: the
	 * two bits say the same thing about the two halves of the unit, that
	 * an arithmetic error is a fault of this thread rather than an
	 * interrupt belonging to the machine.  x86-64 has always set one.
	 */
	cr0 &= ~(CR0_EM | CR0_TS);
	cr0 |= CR0_MP | CR0_NE;
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
	 * ── The ladder, and the processor decides which rung (#561) ───
	 *
	 * ❌ This file stopped at XSAVEOPT, and said why: "The standard format,
	 * deliberately -- XSAVEC's compacted one saves space in the area and
	 * costs a different restore path, and space is not what is scarce
	 * here."  That weighed a trade with one side missing.  #554 then
	 * measured the restore at 165 ns a round trip, and the compacted form
	 * is smaller to read -- so the question was never about space.
	 *
	 * 🔑 AND THE ORDER BETWEEN XSAVEC AND XSAVEOPT IS A MEASUREMENT, NOT A
	 * RANKING.  XSAVEC compacts the image, which shortens the restore;
	 * XSAVEOPT keeps the modified optimisation, which can skip the save
	 * almost entirely.  Neither dominates on paper.
	 *
	 * ❌ It was written the other way round first, on the reasoning that a
	 * smaller image is a shorter restore -- and the measurement refused it.
	 * #554's ping-pong, median of 5 boots each, same kernel with the
	 * feature offered or withheld by qemu so that only the rung differs:
	 *
	 *	XSAVEC    0.560 us
	 *	XSAVEOPT  0.555 us
	 *
	 * Five nanoseconds apart, inside a spread of 0.554..0.572 -- so
	 * compaction buys nothing here, because these threads use the
	 * components anyway and there is little to compact, while the modified
	 * optimisation does skip work on the save.  Indistinguishable is not a
	 * reason to change a default, so XSAVEOPT keeps the place it had and
	 * XSAVEC sits below it, ahead of plain XSAVE where it is unambiguously
	 * better.
	 *
	 * ⚠️ On a machine with wider state -- AVX-512, where the standard image
	 * is much larger than what a thread actually uses -- the answer could
	 * be the other way.  This order is this machine's, and the script that
	 * produced it is scripts/xsave-ladder.sh.
	 *
	 * ⚠️ A kernel does not get to assume the part it boots on.  Every rung
	 * is detected, and every rung is exercised: qemu can offer or withhold
	 * each of these features, so the path a processor without XSAVES takes
	 * is not a path nobody has run.
	 */
	cpuid_count(0xD, 1, &a, &b, &c, &d);
	if (a & (1U << 0))
		use_xsaveopt = 1;
	if (a & (1U << 1))
		use_xsavec = 1;
	if (FPU_ALLOW_XSAVES && (a & (1U << 3)))
		use_xsaves = 1;
	else if (a & (1U << 3))
		have_xsaves_unused = 1;

	if (use_xsaves) {
		/*
		 * XSAVES also carries SUPERVISOR components, chosen by
		 * IA32_XSS, and this kernel has none.  Written explicitly
		 * rather than trusted to be zero from reset: the size asked
		 * for below is the size for XCR0 AND XSS together, so a bit
		 * left set by firmware would size the area for state the
		 * kernel never saves.
		 */
		wrmsr(MSR_IA32_XSS, 0);
	}

	/*
	 * And the size again, because the compacted forms need a different one:
	 * CPUID.(EAX=0DH,ECX=1):EBX is the size of the COMPACTED layout, which
	 * is what XSAVEC and XSAVES write and is smaller than the standard one.
	 *
	 * ⚠️ Never below the header: fpu_area_init() leaves a legacy-plus-header
	 * image behind for the first restore, and an area sized for what the
	 * compacted form happens to need must still hold it.
	 */
	if (use_xsaves || (use_xsavec && !use_xsaveopt)) {
		cpuid_count(0xD, 1, &a, &b, &c, &d);
		if (b >= XCOMP_BV_OFFSET + 8)
			area_size = b;
	}
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
	if (use_xsaves)
		__asm__ volatile("xsaves (%0)"
				 : : "r"(area), "a"(0xFFFFFFFFU), "d"(0xFFFFFFFFU)
				 : "memory");
	else if (use_xsaveopt)
		__asm__ volatile("xsaveopt (%0)"
				 : : "r"(area), "a"(0xFFFFFFFFU), "d"(0xFFFFFFFFU)
				 : "memory");
	else if (use_xsavec)
		__asm__ volatile("xsavec (%0)"
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
	/*
	 * 🔑 XRSTORS FOR XSAVES AND XRSTOR FOR EVERYTHING ELSE, and the pair
	 * is not interchangeable in either direction (#561): XRSTOR reads
	 * whichever format XCOMP_BV says, XRSTORS reads only the compacted
	 * one.  Which is why fpu_area_init() below has to know which of these
	 * will read what it writes.
	 */
	if (use_xsaves)
		__asm__ volatile("xrstors (%0)"
				 : : "r"(area), "a"(0xFFFFFFFFU), "d"(0xFFFFFFFFU)
				 : "memory");
	else if (use_xsave)
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
	 *
	 * 🔴 AND THE FORMAT HAS TO BE THE ONE THE RESTORE WILL READ (#561).
	 *
	 * The zeroing above leaves XCOMP_BV clear, which means STANDARD
	 * format.  XRSTOR accepts that; XRSTORS does not -- it faults on
	 * anything but a compacted image -- and this area is read by the
	 * restore before any save has ever written it, on the first switch to
	 * every thread the system creates.  So on a processor where XSAVES was
	 * chosen, the header says compacted with no components: bit 63 set,
	 * the rest zero, XSTATE_BV still zero.
	 *
	 * ⚠️ Not set unconditionally.  A compacted header handed to a plain
	 * XRSTOR is equally wrong, and the wrongness would arrive as a fault
	 * in the switch rather than here.
	 */
	if (fpu_uses_xsaves())
		*(uint64_t *)(bytes + XCOMP_BV_OFFSET) = XCOMP_BV_COMPACTED;
}
