/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Copy-on-write, asked of a real task (#407).
 *
 * #407's done-when has five clauses and four of them already have something
 * that exercises them: the VM boots, faults have fault_test, the shootdown has
 * tlb_shootdown_selftest(), and the pmap test paths are eighteen self-tests in
 * boot_c.c.  The fifth is "forks with COW", and nothing on this target has ever
 * gone near it.
 *
 * pmap_protect() exists and protect_unmap_selftest() exercises it, but that is
 * not the same claim.  A fork with copy-on-write puts four mechanisms in a row
 * -- vm_map_fork building the shadow objects, pmap_protect downgrading the
 * parent's mappings, the shootdown that makes the downgrade real on every
 * processor, and vm_fault walking the shadow chain to make the private copy --
 * and the value of asking for it here is that a break anywhere along that row
 * shows up as one wrong number.
 *
 * ⚠️ No fork(2) and no exec.  Neither exists on this target and neither is
 * needed: a fork IS task_create with inherit_memory, and a task with no thread
 * in it is the cleanest possible subject, because nothing inside it can move
 * while it is being read.
 */

#include <mach.h>
#include <mach/mach_traps.h>
#include <mach/message.h>
#include <mach/vm_prot.h>
#include <mach/vm_inherit.h>
#include <mach/mach_interface.h>
#include <mach/mach_syscalls.h>

#include <stdio.h>

/*
 * Eight pages for the copied region, one for the shared control.
 *
 * Eight rather than one because arm four times each first store separately and
 * prints all eight: the first is a warm-up whatever the mechanism costs, and an
 * average would hide that instead of showing it.
 */
#define COW_PAGES	8

/*
 * How many times arm five forks, and therefore how deep it drives the chain.
 *
 * Six is chosen against the page budget rather than for a round number: the
 * arm needs one virgin page per level twice over -- once with the children
 * alive and once after they are gone -- so it costs 2 * DEPTH_STEPS pages, and
 * every one of them must be a page no earlier level has already copied.
 */
#define DEPTH_STEPS	6

/*
 * The mirror the child is read into, sized for the largest page this target
 * can have rather than for the page it does have.  main() refuses to run if
 * vm_page_size exceeds it, which is a check and not a comment: a bigger page
 * would silently write past this array.
 */
#define MIRROR_PAGE_MAX	4096

/*
 * A pattern per page, not one pattern for the region.
 *
 * Two pages that both hold the same witness cannot tell a copy that worked from
 * a copy that duplicated the wrong page eight times, and the second is exactly
 * what a shadow chain walked from the wrong offset would produce.  The page
 * index in the low byte means every page names itself.
 */
#define PAT_A(i)	(0xA0A0A0A0A0A00000ULL + (unsigned long long) (i))
#define PAT_B(i)	(0xB0B0B0B0B0B00000ULL + (unsigned long long) (i))

#define SHARE_A		0x5A5A5A5A5A5A0001ULL
#define SHARE_B		0x5B5B5B5B5B5B0001ULL

/*
 * The last word of every page carries the complement of the first.
 *
 * A copy that runs out early leaves a page whose head is new and whose tail is
 * old, which is a defect with no symptom if only the head is read -- and the
 * head is the word every naive test reads.
 */
#define TAIL(v)		(~(unsigned long long) (v))

typedef volatile unsigned long long	*word_t;

static vm_address_t	cow_region;
static vm_address_t	share_region;
static vm_size_t	cow_size;
static mach_port_t	child = MACH_PORT_NULL;

static unsigned char	mirror[COW_PAGES * MIRROR_PAGE_MAX];

static unsigned long long fault_cycles[COW_PAGES];
static unsigned long long copy_cycles;

/*
 * ⚠️ rdtsc and not a clock call.  What is being timed is one store, and any
 * measurement that costs a message is larger than its own subject.
 *
 * lfence first because the counter must not be read before the store ahead of
 * it has issued.  This does not make rdtsc exact -- nothing does -- but the
 * quantity being measured here is a page fault, which is thousands of cycles,
 * and the slop is tens.
 */
static inline unsigned long long
cycles(void)
{
	unsigned int lo, hi;

	__asm__ __volatile__("lfence; rdtsc" : "=a" (lo), "=d" (hi) :: "memory");
	return ((unsigned long long) hi << 32) | lo;
}

static word_t
head_of(vm_address_t base, int page)
{
	return (word_t) (base + (unsigned long) page * vm_page_size);
}

static word_t
tail_of(vm_address_t base, int page)
{
	return (word_t) (base + (unsigned long) (page + 1) * vm_page_size
			      - sizeof(unsigned long long));
}

static unsigned long long
mirror_head(int page)
{
	unsigned long long v;

	__builtin_memcpy(&v, mirror + (unsigned long) page * vm_page_size,
			 sizeof v);
	return v;
}

static unsigned long long
mirror_tail(int page)
{
	unsigned long long v;

	__builtin_memcpy(&v, mirror + (unsigned long) (page + 1) * vm_page_size
			       - sizeof v, sizeof v);
	return v;
}

/*
 * Reading the child.
 *
 * ⚠️ read-overwrite and NOT vm_read, and the reason is that this test would
 * otherwise measure itself.  vm_read hands its answer back as out-of-line
 * memory, and out-of-line memory in Mach is delivered by the very copy-on-write
 * machinery under examination: a broken COW would break the instrument and the
 * test would report a wrong value for the subject when the fault was in the
 * ruler.  The overwrite form writes into a buffer this task already owns and
 * has already touched, so nothing is mapped and nothing is copied on write.
 *
 * ⚠️ Writing this test is what found that vm_read_overwrite had no definition
 * on either target, despite <mach.h> having declared it all along: libmach
 * drops the ms_*.c wrappers because the MIG stubs supply their public names
 * instead, and this call is in no .defs at all, so nothing supplied it.  It
 * exists now, in libmach/syscall_wrappers.c, and this is its first caller --
 * which is the only reason to prefer it here over the raw trap it wraps.  A
 * wrapper nothing calls is a wrapper nothing has tested.
 */
static int
read_child(vm_address_t addr, vm_size_t size)
{
	vm_size_t	got = 0;
	kern_return_t	kr;

	kr = vm_read_overwrite(child, addr, size, (vm_address_t) mirror, &got);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: vm_read_overwrite(0x%lx) failed (%d) — WRONG\n",
		       (unsigned long) addr, kr);
		return 0;
	}
	if (got != size) {
		printf("cow_test: asked the child for %lu bytes and got %lu "
		       "— WRONG\n", (unsigned long) size, (unsigned long) got);
		return 0;
	}
	return 1;
}

/*
 * The baseline arm four compares the fault against.
 *
 * Returns cycles per page, or zero if it could not be measured -- and zero is
 * reported as "not measured" rather than folded into the ratio, because a
 * break-even computed from a missing denominator is a number that looks like
 * an answer.
 */
static unsigned long long
measure_page_copy(void)
{
	vm_address_t		scratch = 0;
	unsigned long long	*src, *dst;
	unsigned long long	t0, t1;
	unsigned		w, words;
	int			i;
	kern_return_t		kr;

	kr = vm_allocate(mach_task_self(), &scratch, 2 * vm_page_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: [4] no scratch for the copy baseline (%d)"
		       " — the cost is not reported\n", kr);
		return 0;
	}

	src = (unsigned long long *) scratch;
	dst = (unsigned long long *) (scratch + vm_page_size);
	words = (unsigned) (vm_page_size / sizeof(unsigned long long));

	/*
	 * Touch both first.  A fault taken inside the baseline would put the
	 * thing being compared into the comparison.
	 */
	for (w = 0; w < words; w++) {
		src[w] = 0x1111111111111111ULL + w;
		dst[w] = 0;
	}

	t0 = cycles();
	for (i = 0; i < COW_PAGES; i++)
		for (w = 0; w < words; w++)
			dst[w] = src[w];
	t1 = cycles();

	(void) vm_deallocate(mach_task_self(), scratch, 2 * vm_page_size);
	return (t1 - t0) / COW_PAGES;
}

/*
 * ── Arm five: what a shadow chain costs, one level at a time ─────────────
 *
 * Mach's copy-on-write is not one indirection, it is a CHAIN.  Every fork of an
 * address space that has already been forked stacks another shadow object over
 * the last, and a fault on a page none of them has copied yet has to walk down
 * the whole stack to find where the page really lives.  vm_object_collapse()
 * exists to fold those away again, and the historic complaint about Mach's VM
 * is precisely that the chain outruns the collapse.
 *
 * 🔑 The children have to be kept ALIVE for the chain to exist at all.  A
 * shadow whose only other reference has gone is exactly what collapse is
 * allowed to fold, so a version of this arm that tidied up after each fork
 * would measure a chain of depth one, six times, and report a flat line as
 * evidence that there is no problem.
 *
 * Which is also what makes the second half a real control rather than a
 * postscript: the same measurement is taken again on fresh pages once all but
 * one of the children are gone.  If the first half climbs and the second is
 * flat, the climb was the chain and collapse works.  If the second half climbs
 * too, the chain is not being folded, and that is a finding rather than a slow
 * test.
 *
 * ⚠️ All but ONE, and the survivor is load-bearing -- see the comment at the
 * termination below.  With nobody left the second half stops being a
 * copy-on-write fault at all, and comparing it to the first would be comparing
 * two different events.
 */
static void
measure_chain_depth(void)
{
	static mach_port_t	kids[DEPTH_STEPS];
	static unsigned long long deep[DEPTH_STEPS];
	static unsigned long long shallow[DEPTH_STEPS];
	vm_address_t		region = 0;
	vm_size_t		size;
	unsigned long long	t0, t1;
	kern_return_t		kr;
	int			i;
	int			forked = 0;

	size = (vm_size_t) (2 * DEPTH_STEPS) * vm_page_size;
	kr = vm_allocate(mach_task_self(), &region, size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: [5] vm_allocate of the depth region failed "
		       "(%d) — the chain is not measured\n", kr);
		return;
	}

	/*
	 * Every page touched before the first fork, for the reason arm four
	 * needed the same thing: an untouched page faults because it is not
	 * resident, which is a different fault at a different price, and the
	 * difference would be read as depth.
	 */
	for (i = 0; i < 2 * DEPTH_STEPS; i++)
		*head_of(region, i) = PAT_A(i);

	for (i = 0; i < DEPTH_STEPS; i++) {
		mach_port_t	kid = MACH_PORT_NULL;
		word_t		p;

		kr = task_create(mach_task_self(), (ledger_port_array_t) 0, 0,
				 TRUE, &kid);
		if (kr != KERN_SUCCESS) {
			printf("cow_test: [5] fork %d failed (%d) — the depths "
			       "past here are not measured\n", i + 1, kr);
			break;
		}
		kids[forked++] = kid;

		p = head_of(region, i);
		t0 = cycles();
		*p = PAT_B(i);
		t1 = cycles();
		deep[i] = t1 - t0;
	}

	printf("cow_test: [5] fault cycles by chain depth, children alive:");
	for (i = 0; i < forked; i++)
		printf(" %llu", deep[i]);
	printf("\n");

	/*
	 * And now take the references away -- all but ONE.
	 *
	 * 🔑 Keeping the first child alive is what makes the two halves
	 * comparable, and the first version of this arm got it wrong by
	 * terminating every one of them.  With no child left at all, the
	 * collapse is free to fold the whole chain away, and a write to a page
	 * afterwards need not copy anything: it can be a plain protection
	 * upgrade.  That is a DIFFERENT EVENT, not the same event made faster,
	 * so the two halves would have been measuring two things and the
	 * difference between them would have looked like a speed-up.
	 *
	 * One survivor forces at least one shadow to stay, so the second half
	 * is still a genuine copy-on-write fault and the only thing that has
	 * changed between the halves is how deep the chain is.
	 *
	 * Nothing here waits for the collapse -- there is nothing to wait on --
	 * so what the second half measures is whatever the kernel has managed
	 * by the time the next fault asks, which is the honest question anyway.
	 */
	for (i = 1; i < forked; i++)
		(void) task_terminate(kids[i]);

	for (i = 0; i < forked; i++) {
		word_t	p = head_of(region, DEPTH_STEPS + i);

		t0 = cycles();
		*p = PAT_B(DEPTH_STEPS + i);
		t1 = cycles();
		shallow[i] = t1 - t0;
	}

	printf("cow_test: [5] the same pages with one child left, chain shallow:");
	for (i = 0; i < forked; i++)
		printf(" %llu", shallow[i]);
	printf("\n");

	if (forked > 0)
		(void) task_terminate(kids[0]);

	(void) vm_deallocate(mach_task_self(), region, size);
}

/*
 * Everything between the allocation and the verdict.  Split out so that a
 * failure part-way can return a count rather than jump, and so that the child
 * task is terminated on every path by the one caller that owns it.
 */
static int
run_the_arms(void)
{
	word_t		p;
	kern_return_t	kr;
	unsigned long long t0, t1;
	int		i;
	int		inherited = 1;
	int		copied = 1;
	int		control = 1;
	int		passed = 0;

	/*
	 * ── Setup ────────────────────────────────────────────────────────
	 *
	 * Both regions are written before the fork, and that is not tidiness:
	 * a page that has never been touched is not resident, so a parent that
	 * forked without touching them would take an ordinary not-resident
	 * fault on the first store afterwards rather than the protection fault
	 * copy-on-write is made of.  Arm four would then be timing the wrong
	 * fault, and would not say so.
	 */
	cow_size = (vm_size_t) COW_PAGES * vm_page_size;
	cow_region = 0;
	kr = vm_allocate(mach_task_self(), &cow_region, cow_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: vm_allocate of the copied region failed (%d)"
		       " — WRONG\n", kr);
		return 0;
	}

	share_region = 0;
	kr = vm_allocate(mach_task_self(), &share_region, vm_page_size, TRUE);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: vm_allocate of the shared region failed (%d)"
		       " — WRONG\n", kr);
		return 0;
	}

	for (i = 0; i < COW_PAGES; i++) {
		*head_of(cow_region, i) = PAT_A(i);
		*tail_of(cow_region, i) = TAIL(PAT_A(i));
	}
	*head_of(share_region, 0) = SHARE_A;
	*tail_of(share_region, 0) = TAIL(SHARE_A);

	/*
	 * The control has to be asked for.  VM_INHERIT_DEFAULT is
	 * VM_INHERIT_COPY, so a region that says nothing is a copied one --
	 * which means the sharing arm is the one that needs a call, and a test
	 * that forgot it would have two copied regions and a control that
	 * agreed with the subject for the wrong reason.
	 */
	kr = vm_inherit(mach_task_self(), share_region, vm_page_size,
			VM_INHERIT_SHARE);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: vm_inherit(SHARE) failed (%d) — WRONG\n", kr);
		return 0;
	}

	/*
	 * ── The fork ─────────────────────────────────────────────────────
	 */
	kr = task_create(mach_task_self(), (ledger_port_array_t) 0, 0,
			 TRUE, &child);
	if (kr != KERN_SUCCESS) {
		printf("cow_test: task_create(inherit_memory) failed (%d)"
		       " — WRONG\n", kr);
		return 0;
	}
	printf("cow_test: forked a task with inherit_memory, child port 0x%x\n",
	       (unsigned) child);

	/*
	 * ── Arm one: the child got the memory ────────────────────────────
	 *
	 * First, because the two arms after it are unreadable without it: a
	 * child whose region is absent or empty would satisfy arm two by
	 * having nothing to change, and would satisfy it for a reason that has
	 * nothing to do with copy-on-write.
	 */
	if (!read_child(cow_region, cow_size))
		return passed;

	for (i = 0; i < COW_PAGES; i++) {
		unsigned long long h = mirror_head(i);
		unsigned long long t = mirror_tail(i);

		if (h != PAT_A(i) || t != TAIL(PAT_A(i))) {
			printf("cow_test: [1] child page %d holds 0x%llx/0x%llx,"
			       " the parent wrote 0x%llx/0x%llx — WRONG\n",
			       i, h, t, PAT_A(i), TAIL(PAT_A(i)));
			inherited = 0;
		}
	}
	if (inherited) {
		printf("cow_test: [1] all %d pages arrived in the child with "
		       "their pre-fork contents\n", COW_PAGES);
		passed++;
	}

	/*
	 * ── Arm four's first half: the parent writes, and it is timed ────
	 *
	 * Done here rather than after arm two because the write IS what arm two
	 * asks about.  The timing is a second reading of the same event, not a
	 * second event.
	 */
	for (i = 0; i < COW_PAGES; i++) {
		p = head_of(cow_region, i);
		t0 = cycles();
		*p = PAT_B(i);
		t1 = cycles();
		fault_cycles[i] = t1 - t0;
		*tail_of(cow_region, i) = TAIL(PAT_B(i));
	}

	*head_of(share_region, 0) = SHARE_B;
	*tail_of(share_region, 0) = TAIL(SHARE_B);

	/*
	 * And the parent must see its own writes.  This looks too obvious to
	 * check and is not: a copy-on-write fault that resolved by mapping the
	 * ORIGINAL page read-write would leave the parent seeing its new value
	 * as well, and every other arm here would still pass.  What that
	 * kernel would have broken is the child, and arm two is where it shows.
	 */
	for (i = 0; i < COW_PAGES; i++) {
		if (*head_of(cow_region, i) != PAT_B(i)) {
			printf("cow_test: [2] the parent wrote 0x%llx to page %d"
			       " and reads 0x%llx back — WRONG\n",
			       PAT_B(i), i,
			       (unsigned long long) *head_of(cow_region, i));
			copied = 0;
		}
	}

	/*
	 * ── Arm three: the control, and it comes before arm two's verdict ─
	 *
	 * The shared page must now show the parent's new value in the child.
	 *
	 * 🔑 This is the arm that makes arm two mean anything.  Arm two is a
	 * proof by absence -- the child did NOT change -- and an absence is
	 * only evidence if the observation could have seen a presence.  A
	 * vm_read_overwrite that returned the pre-fork contents of everything,
	 * or that read the parent's own objects rather than the child's map,
	 * would satisfy arm two perfectly while proving nothing at all.  So the
	 * instrument is asked, on the same boot and through the same call, for
	 * a page where the answer must be different.
	 */
	if (!read_child(share_region, vm_page_size))
		return passed;

	if (mirror_head(0) != SHARE_B || mirror_tail(0) != TAIL(SHARE_B)) {
		printf("cow_test: [3] CONTROL FAILED: the shared page reads "
		       "0x%llx/0x%llx in the child, the parent wrote "
		       "0x%llx/0x%llx\n",
		       mirror_head(0), mirror_tail(0),
		       (unsigned long long) SHARE_B, TAIL(SHARE_B));
		control = 0;
	} else {
		printf("cow_test: [3] control: the parent's write to a "
		       "VM_INHERIT_SHARE page IS visible in the child\n");
		passed++;
	}

	/*
	 * ── Arm two: the copy ────────────────────────────────────────────
	 */
	if (!read_child(cow_region, cow_size))
		return passed;

	for (i = 0; i < COW_PAGES; i++) {
		unsigned long long h = mirror_head(i);
		unsigned long long t = mirror_tail(i);

		if (h != PAT_A(i) || t != TAIL(PAT_A(i))) {
			printf("cow_test: [2] child page %d became 0x%llx/0x%llx"
			       " when the parent wrote — WRONG, the write "
			       "reached the child\n", i, h, t);
			copied = 0;
		}
	}

	if (!control)
		printf("cow_test: [2] NOT READABLE — the control failed, so "
		       "\"the child did not change\" is not evidence of a "
		       "copy\n");
	else if (copied) {
		printf("cow_test: [2] the parent wrote every page and the "
		       "child kept all %d — the copy happened\n", COW_PAGES);
		passed++;
	}

	/*
	 * ── Arm four: what the copy cost ─────────────────────────────────
	 *
	 * The comparison that decides whether copy-on-write is worth having is
	 * not "fault versus store".  It is "fault versus copying the page you
	 * were going to copy anyway": eager copy pays one page copy per page,
	 * copy-on-write pays one fault PLUS that same page copy, for the pages
	 * that get written.  So copy-on-write wins only while the fraction of
	 * pages written stays under copy/(fault+copy), and that fraction is
	 * what this arm prints.
	 *
	 * The copy is timed here in ring 3 over memory this task owns, which is
	 * not the kernel's page copy through the direct map.  It is the right
	 * order of magnitude and it is honest about being a floor: the kernel's
	 * is not faster.
	 */
	copy_cycles = measure_page_copy();

	printf("cow_test: [4] copy-on-write fault, cycles per page:");
	for (i = 0; i < COW_PAGES; i++)
		printf(" %llu", fault_cycles[i]);
	printf("\n");

	if (copy_cycles) {
		unsigned long long f = 0;
		unsigned long long breakeven;

		/*
		 * The first page is dropped, and it is named rather than
		 * quietly excluded: it carries whatever this task pays once --
		 * the first shadow object, the first walk of a chain nothing
		 * has cached.  Seven and not eight, and the eight are printed
		 * above so the drop can be checked.
		 */
		for (i = 1; i < COW_PAGES; i++)
			f += fault_cycles[i];
		f /= (COW_PAGES - 1);

		breakeven = (copy_cycles * 100) / (f + copy_cycles);

		printf("cow_test: [4] fault %llu cycles/page (first dropped), "
		       "page copy %llu — copy-on-write is the cheaper choice "
		       "only while under %llu%% of the pages get written\n",
		       f, copy_cycles, breakeven);
	}

	/*
	 * ⚠️ Arm five has no verdict and is not counted.  It answers "how much"
	 * and not "is it right", and a measurement given a pass/fail would need
	 * a threshold that nothing here is entitled to choose.  It is last
	 * because it leaves six terminated tasks behind it.
	 */
	measure_chain_depth();

	return passed;
}

int
main(int argc, char **argv)
{
	mach_port_t	quiet = MACH_PORT_NULL;
	int		passed;

	(void) argc;
	(void) argv;

	printf("cow_test: started (#407)\n");

	/*
	 * The mirror is a fixed array and vm_page_size is a runtime value, so
	 * the relation between them is checked rather than assumed.  A page
	 * larger than MIRROR_PAGE_MAX would be written past the end of it, and
	 * the first symptom would be somewhere else entirely.
	 */
	if (vm_page_size > MIRROR_PAGE_MAX) {
		printf("cow_test: vm_page_size is %lu and this test's mirror "
		       "holds %d — not run\n",
		       (unsigned long) vm_page_size, MIRROR_PAGE_MAX);
		passed = 0;
	} else
		passed = run_the_arms();

	if (child != MACH_PORT_NULL)
		(void) task_terminate(child);

	printf("cow_test: %d of 3 arms passed\n", passed);

	/*
	 * ⚠️ Does not exit, for the same reason fault_test does not: there is
	 * no proc_server on this target to reap a task, and returning would
	 * take the only thread out from under a program whose output the boot
	 * log is still the record of.
	 *
	 * It blocks in the kernel on a port nothing holds a send right to,
	 * rather than spinning: a task that ends its life burning a processor
	 * would change every measurement taken after it on this boot.
	 */
	(void) mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
				  &quiet);

	for (;;) {
		mach_msg_header_t	junk;

		(void) mach_msg(&junk, MACH_RCV_MSG, 0, sizeof junk, quiet,
				MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	}

	return 0;
}
