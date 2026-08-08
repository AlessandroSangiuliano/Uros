/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The frame each kind of entry builds, checked (#409, MD contract 4/6).
 * See <trap/entry_test.h> for why this is not "does the handler run".
 */

#include <stdint.h>

#include <cpu/acpi.h>
#include <cpu/desc.h>
#include <cpu/ioapic.h>
#include <cpu/lapic.h>
#include <cpu/regs.h>
#include <ddb/cons.h>
#include <time/pit.h>
#include <trap/entry_test.h>
#include <trap/trap.h>

/* The probes, and the two numbers they publish about themselves. */
extern void	 entry_probe_fault(void);
extern uint64_t	 entry_probe_spin(uint64_t turns);
extern uint64_t	 entry_probe_rsp;
extern uint64_t	 entry_probe_fault_rip;
extern void	 entry_probe_int2(void);
extern uint64_t	 entry_probe_int2_rip;
extern volatile uint64_t entry_probe_arrived;
extern char	 entry_probe_lo[], entry_probe_hi[];

/*
 * How many turns to wait.
 *
 * Two numbers because two waits: a tick at five hundred hertz is two
 * milliseconds away and an NMI already sent is microseconds away, and a loop
 * that spun for the longer of them every time would pay for the worst case on
 * every entry.  Both are bounds, not delays — the loop leaves as soon as its
 * handler says so, and reaching the end is the failure.
 */
#define SPIN_SLOW	400000000ULL
#define SPIN_FAST	4000000ULL

/*
 * What the entry path is required to have built, and where the numbers come
 * from — which is the whole point, so each is named after its source.
 */
struct entry_check {
	const char	*what;
	uint64_t	 rip;		/* the frame said the interrupted rip was */
	uint64_t	 rsp;		/* ... and the interrupted stack pointer  */
	uint64_t	 cs;
	uint64_t	 rflags;
	uint64_t	 frame;		/* where the frame itself was built       */
	uint64_t	 rbp;		/* and what a backtrace walks from        */
	uint64_t	 vector;
	uint64_t	 error;
	uint64_t	 handler_flags;	/* the flags the HANDLER ran with         */
	/*
	 * Whether the two above could be observed at all, because one path
	 * here cannot observe them and a field that reads as "zero, therefore
	 * correct" on that path would be a pass nobody earned.
	 */
	int		 flags_measured;
	/*
	 * The one address the saved rip must be, when there is one.  Zero
	 * means anywhere inside the probe, which is all an asynchronous entry
	 * can promise; a software interrupt promises the exact byte after
	 * itself, and checking the weaker thing there would miss a stub that
	 * saved the wrong one.
	 */
	uint64_t	 want_rip;
	int		 seen;
};

static struct entry_check timer_seen, device_seen;

static void note(struct entry_check *c, const struct trap_frame *f)
{
	/*
	 * ⚠️ Only a frame that points into the probe is recorded.
	 *
	 * The event is armed before the probe is entered, so it can arrive
	 * first — a tick that fires between `lapic_timer_start' and the `call'
	 * interrupts the test's own C code, and a frame from there is perfectly
	 * correct and describes somewhere this cannot check.  Recording it
	 * would fail the test for the one reason that is not a defect.
	 *
	 * So the handler selects, and the bound is what turns "no such frame
	 * ever arrived" into a failure rather than a wait.
	 */
	if (c->seen || f->rip < (uint64_t)(uintptr_t)entry_probe_lo
	    || f->rip >= (uint64_t)(uintptr_t)entry_probe_hi)
		return;

	c->rip = f->rip;
	c->rsp = f->rsp;
	c->cs = f->cs;
	c->rflags = f->rflags;
	c->frame = (uint64_t)(uintptr_t)f;
	c->rbp = f->rbp;
	c->vector = f->vector;
	c->error = f->error;
	c->handler_flags = interrupts_enabled();
	c->flags_measured = 1;
	c->seen = 1;

	entry_probe_arrived = 1;
}

static void timer_entry(struct trap_frame *f)
{
	note(&timer_seen, f);
	lapic_eoi();
}

static void device_entry(struct trap_frame *f)
{
	note(&device_seen, f);
	lapic_eoi();
}

/*
 * Where a frame must be, given the stack the interrupted code was on.
 *
 * The processor pushes five words, the stub pushes the vector and the error
 * code, and SAVE_REGS pushes fifteen: 176 bytes below where it started.  Where
 * it started is not %rsp, though — in long mode an interrupt aligns the stack
 * pointer down to sixteen before pushing anything, and trap/entry.S depends on
 * that in writing to have a 16-byte aligned frame without a fixup.
 *
 * ⚠️ Which is why this is worth checking rather than assuming: the two
 * formulas differ by exactly eight bytes when the interrupted code was in a
 * function called from C, and every probe here is.  So the alignment is
 * observable, and a comment that turned out to be wrong about it would be a
 * frame at the wrong address rather than a paragraph.
 */
static uint64_t frame_should_be(uint64_t interrupted_rsp)
{
	return (interrupted_rsp & ~15ULL) - 176;
}

static void report(const struct entry_check *c, uint64_t want_vector,
		   int on_own_stack, unsigned ist_slot)
{
	uint64_t here = entry_probe_rsp;
	int rip_ok, rsp_ok, ring_ok, frame_ok, flags_ok;

	cons_puts("UrMach x86-64: ");
	cons_puts(c->what);

	if (!c->seen) {
		cons_puts(" never reached the probe — WRONG\r\n");
		return;
	}

	rip_ok = c->want_rip != 0
	       ? c->rip == c->want_rip
	       : c->rip >= (uint64_t)(uintptr_t)entry_probe_lo
		 && c->rip < (uint64_t)(uintptr_t)entry_probe_hi;
	rsp_ok = c->rsp == here;
	ring_ok = (c->cs & 3) == 0;

	/*
	 * A gate naming an IST slot switches stacks and one that does not
	 * builds its frame under the interrupted one.  Both are checked
	 * against where the frame actually is, which is the only evidence
	 * either way — the gate itself says nothing at the time it is used.
	 */
	frame_ok = on_own_stack
		 ? desc_on_ist_stack(cpu_apic_id(), ist_slot, c->frame)
		   && !desc_on_ist_stack(cpu_apic_id(), ist_slot, c->rsp)
		 : c->frame == frame_should_be(c->rsp);

	/*
	 * And an interrupt gate clears IF on the way in.  A trap gate would
	 * not, and the difference is invisible until a handler is interrupted
	 * by the thing it is handling.
	 */
	flags_ok = !c->flags_measured || c->handler_flags == 0;

	cons_puts(" at rip ");
	cons_puthex64(c->rip);
	cons_puts(" rsp ");
	cons_puthex64(c->rsp);
	cons_puts(" frame ");
	cons_puthex64(c->frame);

	cons_puts(rip_ok && rsp_ok && ring_ok && frame_ok && flags_ok
		  && c->vector == want_vector
		  ? " — the frame is the one the interrupted code had\r\n"
		  : " — WRONG, the frame does not describe the probe\r\n");

	/*
	 * And say WHICH field, when one of them is out.  A line that only says
	 * the frame is wrong sends the reader back to the source to work out
	 * which of six things it meant.
	 */
	if (rip_ok && rsp_ok && ring_ok && frame_ok && flags_ok
	    && c->vector == want_vector)
		return;

	cons_puts("               ");
	if (!rip_ok && c->want_rip != 0) {
		cons_puts("rip should be ");
		cons_puthex64(c->want_rip);
		cons_puts("; ");
	} else if (!rip_ok) {
		cons_puts("rip is not in the probe; ");
	}
	if (!rsp_ok) {
		cons_puts("rsp should be ");
		cons_puthex64(here);
		cons_puts("; ");
	}
	if (!ring_ok)
		cons_puts("the saved cs is not ring 0; ");
	if (!frame_ok) {
		cons_puts("the frame should be ");
		if (on_own_stack)
			cons_puts("on its interrupt stack");
		else
			cons_puthex64(frame_should_be(c->rsp));
		cons_puts("; ");
	}
	if (!flags_ok)
		cons_puts("the handler ran with interrupts enabled; ");
	if (c->vector != want_vector)
		cons_puts("the vector is not the one asked for; ");
	cons_puts("\r\n");
}

/*
 * And the other half of the issue's requirement: a correct backtrace from each.
 *
 * ⚠️ Correct is not the same as non-empty, which is why this asks for a name at
 * the far end rather than a depth.  A chain that breaks halfway still returns
 * frames, they still resolve to real functions, and nothing about the result
 * says the bottom is missing — the reader sees a short stack and assumes the
 * fault happened near the top.  Reaching x86_64_boot is the only thing that
 * distinguishes a whole walk from a plausible fragment.
 *
 * The probes are hand-written assembly and set up no frame of their own, so the
 * first name is the C function that called them.  That is not a shortcoming:
 * a frame pointer describes where its function returns to, so the interrupted
 * function is named by the saved rip and is never in the chain.
 *
 * ⚠️ And the depth is reported, not required.  The first version of this asked
 * for at least two frames and every entry failed with one — because the static
 * functions below had all been inlined into x86_64_boot, so the chain really
 * was one frame long and really was complete.  The test was measuring the
 * optimiser.  They are noinline now so the walk has something to walk, but what
 * is CHECKED is the name at the far end, which no inlining decision can move.
 */
static void report_backtrace(const char *what, uint64_t rbp)
{
	const char *first = 0;
	int found = 0;
	unsigned depth = x86_64_backtrace_probe(rbp, &first, "x86_64_boot",
						&found);

	cons_puts("UrMach x86-64: the backtrace from ");
	cons_puts(what);
	cons_puts(" is ");
	cons_putdec(depth);
	cons_puts(" frames from ");
	cons_puts(first != 0 ? first : "(no name)");
	cons_puts(found
		  ? " down to x86_64_boot — the chain is whole\r\n"
		  : " — WRONG, the chain does not reach the bottom\r\n");
}

/*
 * A kernel fault, at an instruction whose address the probe wrote down before
 * executing it.
 */
__attribute__((noinline)) static void kernel_fault_entry(void)
{
	const struct trap_record *t;
	int rip_ok, rsp_ok, frame_ok;

	trap_expect(T_INVALID_OPCODE, (uint64_t)(uintptr_t)trap_probe_faulted);
	entry_probe_fault();
	t = trap_last();

	rip_ok = t->rip == entry_probe_fault_rip;
	rsp_ok = t->rsp == entry_probe_rsp;
	frame_ok = t->frame == frame_should_be(t->rsp);

	cons_puts("UrMach x86-64: a kernel fault at rip ");
	cons_puthex64(t->rip);
	cons_puts(" rsp ");
	cons_puthex64(t->rsp);
	cons_puts(" frame ");
	cons_puthex64(t->frame);
	cons_puts(t->caught && t->vector == T_INVALID_OPCODE && t->error == 0
		  && (t->cs & 3) == 0 && rip_ok && rsp_ok && frame_ok
		  ? " — exactly the instruction and stack it was raised on\r\n"
		  : " — WRONG, the frame does not describe the probe\r\n");

	if (!rip_ok) {
		cons_puts("               the faulting instruction was at ");
		cons_puthex64(entry_probe_fault_rip);
		cons_puts("\r\n");
	}
	if (!rsp_ok) {
		cons_puts("               the stack pointer was ");
		cons_puthex64(entry_probe_rsp);
		cons_puts("\r\n");
	}
	if (!frame_ok) {
		cons_puts("               the frame should be at ");
		cons_puthex64(frame_should_be(t->rsp));
		cons_puts("\r\n");
	}

	report_backtrace("a kernel fault", t->rbp);
}

__attribute__((noinline)) static void timer_entry_test(void)
{
	int had_interrupts;

	if (!lapic_timer_calibrate()) {
		cons_puts("UrMach x86-64: no timer to enter from — WRONG\r\n");
		return;
	}

	timer_seen.what = "a timer interrupt";
	trap_set_handler(LAPIC_TIMER_VECTOR, timer_entry);

	had_interrupts = interrupts_enabled();
	interrupts_enable();

	if (lapic_timer_start(500, LAPIC_TIMER_VECTOR))
		entry_probe_spin(SPIN_SLOW);

	lapic_timer_stop();
	if (!had_interrupts)
		interrupts_disable();

	report(&timer_seen, LAPIC_TIMER_VECTOR, 0, 0);
	report_backtrace("a timer interrupt", timer_seen.rbp);
}

__attribute__((noinline)) static void device_entry_test(void)
{
	uint32_t gsi = acpi_irq_to_gsi(0);
	int had_interrupts;

	if (!ioapic_init()) {
		cons_puts("UrMach x86-64: no I/O APIC to enter from — WRONG\r\n");
		return;
	}

	device_seen.what = "a device interrupt";
	trap_set_handler(IOAPIC_ISA_VECTOR_BASE, device_entry);

	had_interrupts = interrupts_enabled();
	interrupts_enable();

	if (pit_periodic_start(1000)) {
		ioapic_route(gsi, IOAPIC_ISA_VECTOR_BASE, lapic_id(),
			     acpi_irq_flags(0));
		entry_probe_spin(SPIN_SLOW);
		ioapic_mask(gsi);
	}

	pit_periodic_stop();
	if (!had_interrupts)
		interrupts_disable();

	report(&device_seen, IOAPIC_ISA_VECTOR_BASE, 0, 0);
	report_backtrace("a device interrupt", device_seen.rbp);
}

/*
 * An NMI, in two halves, because no one experiment can show both things.
 *
 * ⚠️ A genuine NMI cannot be aimed.  It is sent from here and delivered when
 * the controller gets to it, which on this emulator is inside the store to the
 * interrupt command register — so it lands in lapic.c, every time, eight times
 * out of eight when that was tried.  A frame from there is a perfectly correct
 * frame describing an address this test knows nothing about, so requiring it
 * to be in the probe fails for the one reason that is not a defect.
 *
 * So the real one is asked only what it alone can answer — that the message
 * arrives, and that the gate switched to the stack the interrupt stack table
 * names — and the frame itself is checked through IDT[2] reached by `int $2',
 * which is the same gate, the same slot, the same stub, and can be put at an
 * instruction chosen in advance.
 *
 * Neither half claims the other's ground: a software interrupt is not an NMI,
 * and where a real one landed is not a check of the fields it carried.
 */
__attribute__((noinline)) static void nmi_delivery_test(void)
{
	const struct trap_record *t;
	int on_ist, rsp_elsewhere;

	trap_expect(T_NMI, TRAP_RESUME_HERE);
	lapic_send_nmi(cpu_apic_id());

	/*
	 * A bound rather than a bare read: the delivery is asynchronous by
	 * definition, and reading the record on the next instruction would be
	 * asking before the answer can exist.
	 */
	entry_probe_spin(SPIN_FAST);

	t = trap_last();
	on_ist = desc_on_ist_stack(cpu_apic_id(), IST_NMI, t->frame);
	rsp_elsewhere = !desc_on_ist_stack(cpu_apic_id(), IST_NMI, t->rsp);

	cons_puts("UrMach x86-64: an NMI arrived at rip ");
	cons_puthex64(t->rip);
	cons_puts(", frame ");
	cons_puthex64(t->frame);
	cons_puts(" on its own stack, interrupted stack ");
	cons_puthex64(t->rsp);
	cons_puts(t->caught && t->vector == T_NMI && (t->cs & 3) == 0
		  && on_ist && rsp_elsewhere
		  ? " — the gate switched stacks, which is what the slot is for\r\n"
		  : " — WRONG, it did not run on the stack the table names\r\n");

	report_backtrace("an NMI", t->rbp);
}

__attribute__((noinline)) static void nmi_gate_test(void)
{
	struct entry_check c = { 0 };
	const struct trap_record *t;

	c.what = "vector 2 through its gate";

	trap_expect(T_NMI, TRAP_RESUME_HERE);
	entry_probe_int2();
	t = trap_last();

	c.rip = t->rip;
	c.rsp = t->rsp;
	c.cs = t->cs;
	c.frame = t->frame;
	c.vector = t->vector;
	c.error = t->error;
	c.seen = t->caught;

	/*
	 * ⚠️ One exact address, not the probe's range.  A software interrupt
	 * saves the address of the NEXT instruction, where `ud2' above saves
	 * its own, and that difference is real: a stub that saved the wrong one
	 * would make a handler which steps over an instruction step over two.
	 */
	c.want_rip = entry_probe_int2_rip;
	c.rbp = t->rbp;

	report(&c, T_NMI, 1, IST_NMI);
	report_backtrace("vector 2", c.rbp);
}

void trap_entry_test(void)
{
	kernel_fault_entry();
	timer_entry_test();
	device_entry_test();
	nmi_delivery_test();
	nmi_gate_test();
}
