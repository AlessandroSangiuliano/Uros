/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The layer between thread_get_state() and the converters (#408).
 *
 * x86_64/boot/boot_c.c already checks the conversions themselves — a trap
 * frame out to a state structure and back, the selectors imposed rather than
 * copied, a segment base in the kernel half refused.  That is the bottom of
 * the stack and it is tested.
 *
 * This is the part above it, and none of it had ever executed.
 * act_machine_get_state() and act_machine_set_state() are where a flavour
 * number becomes a conversion and where a word count is either enough or is
 * not, and kern/thread_act.c checks NEITHER of those things before calling
 * them:
 *
 *	ret = act_machine_get_state(thr_act, flavor, state, state_count);
 *
 * is the whole of it.  The flavour arrives from whoever holds a port to the
 * thread, the count arrives with it, and this machine is the only thing
 * between them and a memcpy.  There has been no thread on this target that
 * has ever been to ring 3 (#422), so nothing has ever called either function.
 *
 * ── The one that is not about this machine at all ─────────────────────
 *
 * kern/exception.c delivers an exception with state like this:
 *
 *	natural_t		state[THREAD_MACHINE_STATE_MAX];
 *	state_cnt = state_count[flavor];
 *	kr = thread_getstatus(a_self, flavor, state, &state_cnt);
 *
 * — a buffer sized by one constant, a count taken from a table in
 * x86_64/thread/state.c, and a dispatch in x86_64/thread/machine.c that
 * decides whether that count is acceptable.  Three separate places, no
 * compiler able to compare them, and the failure if they disagree is not a
 * crash: exception delivery with state simply returns KERN_INVALID_ARGUMENT
 * for that flavour, for ever, and the handler is never told why.  So the
 * table is checked against the dispatch here, flavour by flavour, and the
 * buffer against the table at compile time in state.c.
 *
 * ── Why a fabricated activation ───────────────────────────────────────
 *
 * Everything below drives act_machine_[gs]et_state() directly rather than
 * thread_get_state(), and the difference is worth stating rather than
 * leaving to be discovered.  thread_get_state() refuses current_act(), holds
 * the target and waits for it to block interruptibly — which needs a second
 * thread and a scheduler, and neither exists at machine_kernel_ready().
 *
 * What that costs is the machine-independent hold-and-stop dance, which is
 * shared with i386 and runs there.  What it buys is that this runs on EVERY
 * boot instead of behind a flag, and the machine-dependent half is the half
 * that has never run.  The other half is #408's remaining question and is
 * asked separately.
 *
 * The activation is fabricated and not borrowed: act_machine_[gs]et_state()
 * read exactly one field of it, thr_act->mact.pcb, so a zeroed thread_act
 * with act_machine_create() run over it is not a stand-in for the real thing
 * — it is the whole of what they see.  Borrowing a live thread would mean
 * writing a user frame pointer into a thread that has none, which is a lie
 * that outlives the test.
 */

#include <string.h>

#include <kern/misc_protos.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/thread_act.h>
#include <mach/machine.h>	/* machine_slot[] */
#include <mach/machine/vm_types.h>
#include <mach/thread_status.h>
#include <mach/mach_server.h>	/* thread_get_state, thread_set_state (#448) */

#include <kern/processor.h>
#include <kern/sched_prim.h>
#include <kern/cpu_number.h>
#include <kern/thread_swap.h>

#include <cpu/desc.h>		/* USER_CS_RPL3, USER_DS_RPL3 */
#include <cpu/percpu.h>		/* percpu(), a kernel address to be refused */
#include <cpu/regs.h>		/* cpu_pause, rdtsc */
#include <time/tsc.h>		/* tsc_hz, to bound the waits */
#include <pmap/layout.h>	/* va_is_kernel */
#include <pmap/pte.h>		/* PAGE_SIZE_4K */
#include <thread/fpu.h>
#include <thread/state.h>
#include <thread/state_test.h>
#include <trap/trap.h>

/*
 * A segment base a thread could plausibly have been given: canonical, in the
 * lower half, and nowhere near anything this kernel maps.  The same value the
 * boot-time conversion test uses, and for the same reason — the base that has
 * to be REFUSED must be distinguished by where it points and not by being an
 * odd number.
 */
#define USER_BASE_PROBE		0x0000700000010000ULL

/*
 * What an untouched word of the caller's buffer looks like.  Distinct per
 * word, so that "nothing was written" and "the same thing was written twice"
 * are different observations.
 */
#define SENTINEL(i)		((natural_t) (0xBAD00000u + (i)))

static struct thread_activation	probe_act;
static struct trap_frame	probe_frame;
static struct trap_frame	frame_before;
static natural_t		buf[THREAD_STATE_MAX];

static int	checks;
static int	bad;

/*
 * One claim.  Silent when it holds — sixteen passing lines is how the one
 * that did not gets read past (#451) — and loud in the word the harness
 * greps for when it does not.
 */
static void
claim(int ok, const char *what)
{
	checks++;
	if (ok)
		return;

	bad++;
	printf("state_test: WRONG — %s (#408)\n", what);
}

static void
buf_fill(void)
{
	for (unsigned i = 0; i < THREAD_STATE_MAX; i++)
		buf[i] = SENTINEL(i);
}

/* Whether the words from `from' on are still the ones buf_fill() wrote. */
static int
buf_intact_from(unsigned from)
{
	for (unsigned i = from; i < THREAD_STATE_MAX; i++)
		if (buf[i] != SENTINEL(i))
			return 0;
	return 1;
}

static void
frame_snapshot(void)
{
	memcpy(&frame_before, &probe_frame, sizeof frame_before);
}

static int
frame_unchanged(void)
{
	return memcmp(&frame_before, &probe_frame, sizeof frame_before) == 0;
}

/*
 * A frame that looks like one trap entry left behind for a thread returning
 * to ring 3: every field a different value, so a field copied from its
 * neighbour shows up, and the three that decide privilege set to what a
 * thread is actually allowed to have.
 */
static void
frame_fabricate(uint64_t seed)
{
	for (unsigned i = 0; i < sizeof probe_frame / 8; i++)
		((uint64_t *) &probe_frame)[i] = seed + i;

	probe_frame.cs = USER_CS_RPL3;
	probe_frame.ss = USER_DS_RPL3;
	probe_frame.rflags = RFLAGS_IF | 2;
	probe_frame.vector = T_PAGE_FAULT;
	probe_frame.error = 0x7;
}

/*
 * The seventeen fields that are meant to survive a round trip unchanged.
 *
 * Not cs, ss or rflags: those are imposed rather than copied, deliberately,
 * and a test that demanded them back would be demanding the hole (the
 * reasoning is in <thread/state.h>).  Not the segment bases either, which the
 * read direction takes from the machine and the write direction has nowhere
 * to put.
 */
static int
regs_equal(const struct x86_64_thread_state *a,
	   const struct x86_64_thread_state *b)
{
	return a->rax == b->rax && a->rbx == b->rbx && a->rcx == b->rcx
	    && a->rdx == b->rdx && a->rdi == b->rdi && a->rsi == b->rsi
	    && a->rbp == b->rbp && a->rsp == b->rsp
	    && a->r8  == b->r8  && a->r9  == b->r9  && a->r10 == b->r10
	    && a->r11 == b->r11 && a->r12 == b->r12 && a->r13 == b->r13
	    && a->r14 == b->r14 && a->r15 == b->r15
	    && a->rip == b->rip;
}

/*
 * ── A: an activation with no user frame ───────────────────────────────
 *
 * Which is every thread this kernel has today.  The answer has to be a
 * refusal and not a zeroed state: a thread that has never been to ring 3 has
 * no registers to report, and a frame of zeros reads to a debugger as a
 * thread stopped at address zero — an answer that is wrong and looks right.
 */
static void
test_no_user_frame(void)
{
	mach_msg_type_number_t	count;

	probe_act.mact.pcb->user = (struct trap_frame *) 0;

	buf_fill();
	count = x86_64_THREAD_STATE_COUNT;
	claim(act_machine_get_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf, &count)
	      != KERN_SUCCESS,
	      "a thread that has never been to user mode reported registers");
	claim(buf_intact_from(0),
	      "the refused read wrote into the caller's buffer anyway");

	claim(act_machine_set_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf,
				    x86_64_THREAD_STATE_COUNT)
	      != KERN_SUCCESS,
	      "registers were set on a thread that has no frame to set them in");
}

/*
 * ── B: the round trip ─────────────────────────────────────────────────
 *
 * Out through the dispatch, back in through the dispatch, and out again.
 *
 * ⚠️ The first write-back is REFUSED, and that is the interesting half.  The
 * read direction reports the segment bases the machine actually holds, and
 * the machine here is the kernel — so gs_base comes back as this processor's
 * per-CPU block, which is a kernel address, which the write direction is
 * obliged to refuse (#440).  A debugger that reads a kernel thread's state
 * and writes it straight back is therefore told no, and that is correct.
 *
 * It is also this test's own control: the same call succeeds one line later
 * with nothing changed but the two bases, so "refused" is a property of the
 * bases and not of the call never having worked.
 */
static void
test_round_trip(void)
{
	struct x86_64_thread_state	*out = (struct x86_64_thread_state *) buf;
	struct x86_64_thread_state	first, second;
	mach_msg_type_number_t		count;
	kern_return_t			kr;

	frame_fabricate(0x5100000000000000ULL);
	probe_act.mact.pcb->user = &probe_frame;

	buf_fill();
	count = x86_64_THREAD_STATE_COUNT;
	kr = act_machine_get_state(&probe_act, x86_64_THREAD_STATE,
				   (thread_state_t) buf, &count);
	claim(kr == KERN_SUCCESS, "reading the registers of a thread with a "
				  "user frame failed");
	memcpy(&first, buf, sizeof first);

	claim(va_is_kernel(first.gs_base),
	      "the read did not report the machine's own gs base, so the "
	      "refusal below proves nothing");

	frame_snapshot();
	claim(act_machine_set_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf,
				    x86_64_THREAD_STATE_COUNT)
	      != KERN_SUCCESS,
	      "a state naming a kernel gs base was accepted");
	claim(frame_unchanged(),
	      "the refused write had already changed the frame");

	/* The same request with bases a thread could have. */
	out->fs_base = USER_BASE_PROBE;
	out->gs_base = USER_BASE_PROBE + PAGE_SIZE_4K;

	/*
	 * And a different register file, so that "came back unchanged" cannot
	 * be satisfied by a write that did nothing.
	 */
	out->rax = 0xFEEDFACE00000001ULL;
	out->r15 = 0xFEEDFACE0000000FULL;
	out->rip = 0x0000700000020000ULL;
	memcpy(&first, buf, sizeof first);

	claim(act_machine_set_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf,
				    x86_64_THREAD_STATE_COUNT)
	      == KERN_SUCCESS,
	      "a state with user segment bases was refused");

	count = x86_64_THREAD_STATE_COUNT;
	claim(act_machine_get_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) &second, &count)
	      == KERN_SUCCESS, "the second read failed");

	claim(regs_equal(&first, &second),
	      "the seventeen registers did not survive the round trip");

	/* And the three that must NOT survive it. */
	claim(probe_frame.cs == USER_CS_RPL3 && probe_frame.ss == USER_DS_RPL3
	      && (probe_frame.rflags & RFLAGS_IF) != 0,
	      "the round trip through the dispatch lost the imposed selectors");
}

/*
 * ── C: the word count ─────────────────────────────────────────────────
 *
 * A count is an in-out parameter on the way out and an in parameter on the
 * way in, and both directions have a way to be wrong that is silent.
 *
 * Too generous a count on the way out is the dangerous one: the caller says
 * how much room it has, and if the machine answers with that number instead
 * of the flavour's, the words between the two travel out of the kernel to
 * whoever asked — and they are whatever the buffer held.
 */
static void
test_counts(void)
{
	mach_msg_type_number_t	count;

	probe_act.mact.pcb->user = &probe_frame;

	/* Room for the largest flavour, asking for the smallest. */
	buf_fill();
	count = THREAD_STATE_MAX;
	claim(act_machine_get_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf, &count)
	      == KERN_SUCCESS, "a generous count was refused");
	claim(count == x86_64_THREAD_STATE_COUNT,
	      "the count came back as the caller's and not the flavour's, so "
	      "the words past the end travel out of the kernel");
	claim(buf_intact_from(x86_64_THREAD_STATE_COUNT),
	      "the read wrote past the end of its own flavour");

	/* One word short, which is a refusal and must write nothing. */
	buf_fill();
	count = x86_64_THREAD_STATE_COUNT - 1;
	claim(act_machine_get_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf, &count)
	      != KERN_SUCCESS,
	      "a buffer one word short was filled anyway");
	claim(buf_intact_from(0),
	      "the short read wrote into the buffer before refusing");

	frame_snapshot();
	claim(act_machine_set_state(&probe_act, x86_64_THREAD_STATE,
				    (thread_state_t) buf,
				    x86_64_THREAD_STATE_COUNT - 1)
	      != KERN_SUCCESS,
	      "a state one word short was applied");
	claim(frame_unchanged(),
	      "the short write changed the frame before refusing");
}

/*
 * ── D: why it stopped is read-only ────────────────────────────────────
 *
 * The exception state is what the machine reported about a fault.  Writing it
 * would not change what happened, so the write direction has to refuse rather
 * than accept and discard — a caller told KERN_SUCCESS by a function that
 * threw the request away has been lied to.
 */
static void
test_exception_state(void)
{
	struct x86_64_exception_state	*es;
	mach_msg_type_number_t		count;

	probe_act.mact.pcb->user = &probe_frame;
	probe_frame.vector = T_PAGE_FAULT;
	probe_frame.error = 0x7;

	buf_fill();
	count = THREAD_STATE_MAX;
	claim(act_machine_get_state(&probe_act, x86_64_EXCEPTION_STATE,
				    (thread_state_t) buf, &count)
	      == KERN_SUCCESS, "the exception state could not be read");
	claim(count == x86_64_EXCEPTION_STATE_COUNT,
	      "the exception state's count did not come back as its own");
	claim(buf_intact_from(x86_64_EXCEPTION_STATE_COUNT),
	      "the exception state read past the end of its flavour");

	es = (struct x86_64_exception_state *) buf;
	claim(es->trapno == T_PAGE_FAULT && es->err == 0x7,
	      "the exception state does not report the frame it was read from");

	frame_snapshot();
	claim(act_machine_set_state(&probe_act, x86_64_EXCEPTION_STATE,
				    (thread_state_t) buf,
				    x86_64_EXCEPTION_STATE_COUNT)
	      != KERN_SUCCESS,
	      "the reason a thread faulted was accepted as something to set");
	claim(frame_unchanged(), "setting the exception state changed the frame");
}

/*
 * ── E: flavours that do not exist ─────────────────────────────────────
 *
 * The number arrives from outside and is used as a switch subject here and as
 * an ARRAY INDEX in kern/exception.c.  Zero is the hole at the bottom of
 * state_count[], THREAD_STATE_NONE is a real flavour name with no state
 * behind it, and 99 is past the end of everything.  All three have to be
 * refused by both directions, with the buffer untouched.
 */
static void
test_unknown_flavors(void)
{
	static const int	nonsense[] = { 0, THREAD_STATE_NONE, 99 };
	mach_msg_type_number_t	count;

	probe_act.mact.pcb->user = &probe_frame;

	for (unsigned i = 0; i < sizeof nonsense / sizeof nonsense[0]; i++) {
		buf_fill();
		count = THREAD_STATE_MAX;
		claim(act_machine_get_state(&probe_act, nonsense[i],
					    (thread_state_t) buf, &count)
		      != KERN_SUCCESS,
		      "a flavour that names no state was read anyway");
		claim(buf_intact_from(0),
		      "reading a flavour that does not exist wrote a buffer");

		frame_snapshot();
		claim(act_machine_set_state(&probe_act, nonsense[i],
					    (thread_state_t) buf,
					    THREAD_STATE_MAX)
		      != KERN_SUCCESS,
		      "a flavour that names no state was written anyway");
		claim(frame_unchanged(),
		      "writing a flavour that does not exist changed the frame");
	}
}

/*
 * ── F: the table and the dispatch have to agree ───────────────────────
 *
 * state_count[] is what kern/exception.c passes as the count, and this
 * machine's dispatch is what decides whether that count is enough.  They are
 * written in two files, compared by nothing, and if they disagree the symptom
 * is that exception delivery with state fails for that flavour and says
 * nothing about why.
 *
 * So: every flavour the table gives a size to must be readable with EXACTLY
 * that size.  Not more — that is the count the exception path will use.
 *
 * ⚠️ Which catches a table entry that is too SMALL, and only that.  An entry
 * larger than its flavour is absorbed: the dispatch accepts any count that is
 * big enough and then writes back the flavour's own, so the exception path
 * carries the right number regardless.  Found by breaking it the other way
 * first and watching the test pass.
 */
static void
test_table_agrees(void)
{
	mach_msg_type_number_t	count;

	probe_act.mact.pcb->user = &probe_frame;

	for (int flavor = 1; flavor <= THREAD_STATE_NONE; flavor++) {
		if (state_count[flavor] == 0)
			continue;

		buf_fill();
		count = state_count[flavor];
		if (act_machine_get_state(&probe_act, flavor,
					  (thread_state_t) buf, &count)
		    == KERN_SUCCESS) {
			checks++;
			continue;
		}

		checks++;
		bad++;
		printf("state_test: WRONG — flavour %d is %u words in "
		       "state_count[] and the dispatch refuses that count, so "
		       "kern/exception.c can never deliver it (#408)\n",
		       flavor, state_count[flavor]);
	}
}

/*
 * ── G: the floating-point image, through the dispatch ─────────────────
 *
 * The conversion itself is checked at boot.  What is checked here is that the
 * flavour reaches it: a save area that belongs to the activation rather than
 * to the caller, and a count that is the flavour's.
 */
static void
test_float_state(void)
{
	struct x86_64_float_state	*fs;
	uint8_t				*area;
	mach_msg_type_number_t		count;
	int				same = 1;

	area = probe_act.mact.pcb->ctx.fpu_area;
	if (area == (void *) 0) {
		printf("state_test: WRONG — the probe activation has no "
		       "floating-point area, so the flavour was not tested "
		       "(#408)\n");
		bad++;
		return;
	}

	/* Something recognisable in the area, past the header the init writes. */
	for (unsigned i = 32; i < 512; i++)
		area[i] = (uint8_t) (i * 7 + 3);

	buf_fill();
	count = THREAD_STATE_MAX;
	claim(act_machine_get_state(&probe_act, x86_64_FLOAT_STATE,
				    (thread_state_t) buf, &count)
	      == KERN_SUCCESS, "the floating-point state could not be read");
	claim(count == x86_64_FLOAT_STATE_COUNT,
	      "the floating-point count did not come back as its own");

	fs = (struct x86_64_float_state *) buf;
	for (unsigned i = 32; i < 512; i++)
		if (fs->fx_image[i] != area[i])
			same = 0;
	claim(same, "the floating-point image read is not the one in the "
		    "activation's save area");

	/* Back in, over a wiped area. */
	for (unsigned i = 0; i < 512; i++)
		area[i] = 0;

	claim(act_machine_set_state(&probe_act, x86_64_FLOAT_STATE,
				    (thread_state_t) buf,
				    x86_64_FLOAT_STATE_COUNT)
	      == KERN_SUCCESS, "the floating-point state could not be written");

	same = 1;
	for (unsigned i = 32; i < 512; i++)
		if (area[i] != (uint8_t) (i * 7 + 3))
			same = 0;
	claim(same, "the floating-point image did not survive the round trip "
		    "through the dispatch");

	buf_fill();
	count = x86_64_FLOAT_STATE_COUNT - 1;
	claim(act_machine_get_state(&probe_act, x86_64_FLOAT_STATE,
				    (thread_state_t) buf, &count)
	      != KERN_SUCCESS,
	      "a floating-point buffer one word short was filled anyway");
	claim(buf_intact_from(0),
	      "the short floating-point read wrote before refusing");
}

void
thread_state_dispatch_test(void)
{
	memset(&probe_act, 0, sizeof probe_act);

	if (act_machine_create(kernel_task, &probe_act) != KERN_SUCCESS) {
		printf("state_test: WRONG — could not build a probe "
		       "activation, so nothing was tested (#408)\n");
		return;
	}

	/*
	 * A save area of its own.  act_machine_create() does not allocate one
	 * — thread_machine_create() does, and that needs a shuttle and a
	 * kernel stack this probe has no use for.
	 */
	probe_act.mact.pcb->ctx.fpu_area = fpu_area_alloc();

	test_no_user_frame();
	test_round_trip();
	test_counts();
	test_exception_state();
	test_unknown_flavors();
	test_table_agrees();
	test_float_state();

	/*
	 * ⚠️ Cleared before anything else can see this activation.  It points
	 * at a static frame that is about to stop meaning anything, and a
	 * dangling pcb->user is the one field on this machine that says "this
	 * thread has been to ring 3".
	 */
	probe_act.mact.pcb->user = (struct trap_frame *) 0;
	fpu_area_free(probe_act.mact.pcb->ctx.fpu_area);
	probe_act.mact.pcb->ctx.fpu_area = (void *) 0;
	act_machine_destroy(&probe_act);

	if (bad == 0)
		printf("state_test: PASS — %d checks on the flavour dispatch: "
		       "counts, refusals, and %u words of registers out and "
		       "back (#408)\n", checks,
		       (unsigned) x86_64_THREAD_STATE_COUNT);
	else
		printf("state_test: %d of %d checks failed\n", bad, checks);
}

/*
 * ══ The entry points themselves (-G) ═══════════════════════════════════
 *
 * Everything above drives act_machine_[gs]et_state() directly.  #408's
 * done-when names thread_get_state() and thread_set_state(), which are the
 * machine-independent functions on top, and they do three things this machine
 * never sees: they refuse the calling activation, they hold the target, and
 * they wait for it to block interruptibly before asking anything.
 *
 * ⚠️ Why this needs a whole boot of its own.  The target must be a thread
 * that is not the caller and that is genuinely stopped — thread_stop_wait()
 * waits for TH_RUN to clear and will wait for ever otherwise — so the test
 * costs a thread parked in a wait nobody will ever signal.  That is fine for
 * a boot that exists to answer this question and not for the ordinary one.
 *
 * 🔑 And it is why the target parks in assert_wait() rather than spinning: a
 * kernel thread in a loop never clears TH_RUN, so thread_get_state() on one
 * would hang the caller.  Worth knowing beyond this test — it is what a
 * debugger attaching to a kernel thread on this machine would do.
 */

static volatile int	victim_parked;
static int		victim_event;
static thread_t		victim_thread;
static struct trap_frame victim_frame;

static void
victim_body(void)
{
	victim_parked = 1;

	for (;;) {
		assert_wait((event_t) &victim_event, TRUE);
		thread_block((void (*)(void)) 0);
	}
}

/*
 * A processor that is not this one, for the same reason fpu_stress does it:
 * the driver below spins waiting for the target to park, and on one processor
 * a spinning caller and a target that has not started yet is a deadlock
 * dressed as a timeout.
 */
static processor_t
another_processor(void)
{
	int	me = cpu_number();

	for (int i = 0; i < NCPUS; i++) {
		if (i == me)
			continue;
		if (!machine_slot[i].is_cpu || !machine_slot[i].running)
			continue;
		return cpu_to_processor(i);
	}
	return PROCESSOR_NULL;
}

void
thread_state_entry_test(void)
{
	struct x86_64_thread_state	*out = (struct x86_64_thread_state *) buf;
	struct x86_64_thread_state	first, second;
	mach_msg_type_number_t		count;
	processor_t			target;
	thread_act_t			act;
	uint64_t			t0, limit;
	spl_t				s;
	int				entry_checks = 0, entry_bad = 0;

	checks = 0;
	bad = 0;

	target = another_processor();
	if (target == PROCESSOR_NULL) {
		printf("state_test: WRONG — no processor other than this one is "
		       "running, so the target would have to share this one and "
		       "nothing was measured (#408)\n");
		return;
	}

	limit = tsc_hz();
	if (limit == 0) {
		printf("state_test: WRONG — no calibrated TSC, so the waits "
		       "below cannot be bounded and nothing is claimed (#408)\n");
		return;
	}

	if (thread_create_at(kernel_task, &victim_thread, victim_body)
	    != KERN_SUCCESS) {
		printf("state_test: WRONG — could not create the target thread "
		       "(#408)\n");
		return;
	}

	act = victim_thread->top_act;
	thread_swappable(act, FALSE);

	s = splsched();
	thread_lock(victim_thread);
	victim_thread->max_priority = BASEPRI_SYSTEM;
	victim_thread->priority = BASEPRI_SYSTEM;
	victim_thread->sched_pri = BASEPRI_SYSTEM;
	thread_bind_locked(victim_thread, target);
	victim_thread->state |= TH_RUN;
	thread_setrun(victim_thread, TRUE, TAIL_Q);
	thread_unlock(victim_thread);
	splx(s);

	act_deallocate(act);
	thread_resume(act);

	/*
	 * Parked, and TH_RUN clear — the second is the real condition and the
	 * first only says the thread got as far as asking.  Read without the
	 * lock on purpose: this is an observation of another processor's
	 * thread, and taking the scheduler lock here to look at it would be
	 * holding it across a wait.
	 */
	t0 = rdtsc();
	while (rdtsc() - t0 < limit * 5) {
		if (victim_parked
		    && (*(volatile int *) &victim_thread->state & TH_RUN) == 0)
			break;
		cpu_pause();
	}

	if (!victim_parked
	    || (*(volatile int *) &victim_thread->state & TH_RUN) != 0) {
		printf("state_test: WRONG — the target never parked in an "
		       "interruptible wait, so thread_get_state() would wait "
		       "for it for ever (#408)\n");
		return;
	}

	printf("state_test: the target is parked on processor %d with TH_RUN "
	       "clear; asking it for its registers\n", target->slot_num);

	/* You may not ask about yourself. */
	buf_fill();
	count = x86_64_THREAD_STATE_COUNT;
	claim(thread_get_state(current_act(), x86_64_THREAD_STATE,
			       (thread_state_t) buf, &count) != KERN_SUCCESS,
	      "thread_get_state answered questions about the caller, which it "
	      "cannot stop");

	/* A thread that has never been to ring 3 has no registers to report. */
	count = x86_64_THREAD_STATE_COUNT;
	claim(thread_get_state(act, x86_64_THREAD_STATE,
			       (thread_state_t) buf, &count) != KERN_SUCCESS,
	      "a kernel thread with no user frame reported registers through "
	      "thread_get_state");
	claim(buf_intact_from(0),
	      "the refused thread_get_state wrote into the caller's buffer");

	/*
	 * Now give it one.  Fabricated, because #422 means no thread on this
	 * target has ever been to user mode to leave a real one — and safe
	 * because the thread is stopped and this is the only field that says
	 * it has been.
	 */
	frame_fabricate(0x6200000000000000ULL);
	memcpy(&victim_frame, &probe_frame, sizeof victim_frame);
	act->mact.pcb->user = &victim_frame;

	count = THREAD_STATE_MAX;
	claim(thread_get_state(act, x86_64_THREAD_STATE,
			       (thread_state_t) buf, &count) == KERN_SUCCESS,
	      "thread_get_state failed on a stopped thread with a user frame");
	claim(count == x86_64_THREAD_STATE_COUNT,
	      "thread_get_state did not answer with the flavour's own count");
	memcpy(&first, buf, sizeof first);

	claim(first.rip == victim_frame.rip && first.rsp == victim_frame.rsp,
	      "thread_get_state reported registers that are not the target's");

	/* Out and back, through both entry points. */
	out->fs_base = USER_BASE_PROBE;
	out->gs_base = USER_BASE_PROBE + PAGE_SIZE_4K;
	out->rax = 0xC0FFEE0000000001ULL;
	out->r15 = 0xC0FFEE000000000FULL;
	memcpy(&first, buf, sizeof first);

	claim(thread_set_state(act, x86_64_THREAD_STATE,
			       (thread_state_t) buf,
			       x86_64_THREAD_STATE_COUNT) == KERN_SUCCESS,
	      "thread_set_state refused a state a thread is entitled to");

	count = THREAD_STATE_MAX;
	claim(thread_get_state(act, x86_64_THREAD_STATE,
			       (thread_state_t) &second, &count)
	      == KERN_SUCCESS, "the read after the write failed");

	claim(regs_equal(&first, &second),
	      "the registers did not survive thread_set_state followed by "
	      "thread_get_state");

	/* And the guard, through the real entry point this time. */
	out->gs_base = (uint64_t) (uintptr_t) percpu();
	frame_snapshot();
	memcpy(&frame_before, &victim_frame, sizeof frame_before);
	claim(thread_set_state(act, x86_64_THREAD_STATE,
			       (thread_state_t) buf,
			       x86_64_THREAD_STATE_COUNT) != KERN_SUCCESS,
	      "thread_set_state accepted a segment base in the kernel half");
	claim(memcmp(&frame_before, &victim_frame, sizeof frame_before) == 0,
	      "the refused thread_set_state changed the target's frame anyway");

	/*
	 * ⚠️ Cleared while the thread is still ours to reason about.  It points
	 * at a static frame, and a kernel thread whose pcb->user is not null is
	 * a thread the rest of the kernel believes has been to ring 3.
	 */
	act->mact.pcb->user = (struct trap_frame *) 0;

	entry_checks = checks;
	entry_bad = bad;

	if (entry_bad == 0)
		printf("state_test: PASS — %d checks through thread_get_state "
		       "and thread_set_state on a stopped thread: the caller "
		       "refused, the target held, and the registers out and "
		       "back (#408)\n", entry_checks);
	else
		printf("state_test: %d of %d checks through the entry points "
		       "failed\n", entry_bad, entry_checks);
}
