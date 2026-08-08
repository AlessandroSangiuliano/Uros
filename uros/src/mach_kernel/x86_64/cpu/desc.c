/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * Descriptor tables, one processor's worth at a time (#438).
 */

#include <stdint.h>

#include <cpu/desc.h>
#include <cpu/regs.h>
#include <cpu/smp.h>
#include <cpu/tss.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <trap/trap.h>

struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

/*
 * Three eight-byte slots — null, kernel code, kernel data — and then two
 * slots per processor, because a task-state segment descriptor is sixteen
 * bytes wide in long mode.
 *
 * Sized for every processor the kernel will start rather than for the ones
 * that turned out to exist: the table has to be laid out before the count is
 * known, since building it is part of finding out.  A kilobyte of table to
 * avoid every processor needing one of its own is a trade worth making once.
 */
#define GDT_FIXED_SLOTS	6
#define GDT_SLOTS	(GDT_FIXED_SLOTS + 2 * SMP_MAX_CPUS)

/*
 * The six are fixed by <cpu/desc.h>: null, kernel code, kernel data, and the
 * three the SYSRET arithmetic reaches.  Checked here rather than trusted,
 * because the failure is silent at setup and loud in the wrong place — a
 * return to user mode loading whatever descriptor sits at the computed
 * offset.
 */
_Static_assert(KERNEL_DS_SELECTOR == KERNEL_CS_SELECTOR + 8,
	       "SYSCALL loads SS from kernel CS + 8");
_Static_assert(USER_DS_SELECTOR == SYSRET_SELECTOR_BASE + 8,
	       "SYSRET loads SS from the STAR base + 8");
_Static_assert(USER_CS_SELECTOR == SYSRET_SELECTOR_BASE + 16,
	       "SYSRET loads CS from the STAR base + 16");
_Static_assert(GDT_FIXED_SLOTS * 8 > USER_CS_SELECTOR,
	       "the fixed slots do not cover the selectors they must hold");

static uint64_t gdt[GDT_SLOTS];
static struct tss64 tss[SMP_MAX_CPUS];

/* Which selector loads a given processor's task-state segment. */
static uint16_t tss_selector(uint32_t cpu_id)
{
	return (uint16_t)((GDT_FIXED_SLOTS + 2 * cpu_id) * 8);
}

/*
 * The stacks a processor needs before it can be trusted with a fault: one
 * per interrupt-stack-table slot, plus the one a privilege transition will
 * land on once there is a ring 3 to transition from.
 */
#define DESC_STACK_SIZE	4096
#define DESC_STACKS	(IST_COUNT + 1)

/*
 * The boot processor's, in the image.  It needs somewhere to take a double
 * fault from the first instruction of C, which is a long time before there
 * is an allocator to ask — and a kernel that cannot report a fault until it
 * has finished starting up has no way to report the ones that stop it
 * starting up.
 *
 * The others are allocated, because sixty-four processors' worth of static
 * stacks is a megabyte of image spent on processors that mostly do not
 * exist.
 */
static uint8_t bsp_stacks[DESC_STACKS][DESC_STACK_SIZE]
	__attribute__((aligned(16)));

/* Write a 64-bit available-TSS descriptor into gdt[idx], gdt[idx+1]. */
static void gdt_set_tss(unsigned idx, uint64_t base, uint32_t limit)
{
	uint64_t lo = 0;

	lo |= (uint64_t)(limit & 0xFFFF);		/* limit 15:0  */
	lo |= (base & 0xFFFFFFULL) << 16;		/* base 23:0   */
	lo |= (uint64_t)0x9 << 40;			/* type: available 64-bit TSS */
	lo |= (uint64_t)1 << 47;			/* present     */
	lo |= (uint64_t)((limit >> 16) & 0xF) << 48;	/* limit 19:16 */
	lo |= ((base >> 24) & 0xFFULL) << 56;		/* base 31:24  */

	gdt[idx] = lo;
	gdt[idx + 1] = (base >> 32) & 0xFFFFFFFFULL;	/* base 63:32  */
}

/*
 * Point a processor's task-state segment at a block of DESC_STACKS
 * consecutive stacks, and put its descriptor in the table.
 */
static void tss_build(uint32_t cpu_id, uint64_t block)
{
	struct tss64 *t = &tss[cpu_id];

	/* The last of the block; the interrupt stacks are the ones before it. */
	t->rsp0 = block + DESC_STACKS * DESC_STACK_SIZE;
	t->iomap_base = sizeof(*t);		/* no I/O permission bitmap */
	trap_ist_setup(t, block, DESC_STACK_SIZE);

	gdt_set_tss(GDT_FIXED_SLOTS + 2 * cpu_id,
		    (uint64_t)(uintptr_t)t, sizeof(*t) - 1);
}

static void load_gdt(void)
{
	struct gdt_ptr gp = { sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt };

	__asm__ volatile("lgdt %0" : : "m"(gp) : "memory");

	/* Reload the data segment registers from the new table. */
	__asm__ volatile(
		"movw %0, %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%ss\n\t"
		"movw %%ax, %%fs\n\t"
		"movw %%ax, %%gs\n\t"
		: : "i"(KERNEL_DS_SELECTOR) : "rax");

	/* Reload CS with a far return to the new code selector. */
	__asm__ volatile(
		"pushq %0\n\t"
		"leaq 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		: : "i"(KERNEL_CS_SELECTOR) : "rax", "memory");
}

void desc_init_bsp(void)
{
	uint32_t self = cpu_apic_id();

	if (self >= SMP_MAX_CPUS)
		panic("desc: the boot processor's APIC id is past the tables");

	gdt[0] = 0;
	gdt[1] = 0x00209A0000000000ULL;		/* code: L, present, DPL0, R/X */
	gdt[2] = 0x0000920000000000ULL;		/* data: present, DPL0, R/W    */

	/*
	 * Ring 3.  Nothing runs there yet — that is #411's next step — but the
	 * descriptors belong here, with the kernel's, because where they sit
	 * is dictated by the same arithmetic and putting them anywhere else
	 * would be putting them in the wrong place.
	 *
	 * The 32-bit one is a real descriptor for the slot the 64-bit return
	 * steps over; only the compatibility form of SYSRET would load it.
	 */
	gdt[3] = 0x00CFFA000000FFFFULL;		/* code32: G, D, DPL3, R/X     */
	gdt[4] = 0x0000F20000000000ULL;		/* data:   present, DPL3, R/W  */
	gdt[5] = 0x0020FA0000000000ULL;		/* code:   L, present, DPL3    */

	tss_build(self, (uint64_t)(uintptr_t)bsp_stacks);
	desc_activate(self);
}

void desc_alloc(uint32_t cpu_id)
{
	uint64_t frames;

	if (cpu_id >= SMP_MAX_CPUS)
		panic("desc: an APIC id past the descriptor tables");

	/*
	 * Contiguous, because the stacks are addressed as one block and a
	 * fault that ran off the end of one into the next would at least be
	 * writing into memory this processor owns rather than into whatever
	 * the allocator handed out in between.
	 */
	frames = boot_frames_alloc(DESC_STACKS);
	if (frames == 0)
		panic("desc: no memory for a processor's fault stacks");

	tss_build(cpu_id, phys_to_direct(frames));
}

uint64_t desc_gdt_entry(unsigned selector)
{
	unsigned idx = selector / 8;

	if (idx >= GDT_SLOTS)
		panic("desc: asked for a selector past the table");

	return gdt[idx];
}

uint64_t desc_rsp0(uint32_t cpu_id)
{
	if (cpu_id >= SMP_MAX_CPUS)
		panic("desc: asked for the stack of a processor past the tables");

	return tss[cpu_id].rsp0;
}

/*
 * Whether `addr` is on the interrupt stack that `slot` names (#409).
 *
 * A predicate rather than the bounds, because the bounds are two numbers and a
 * caller given two numbers has to get the comparison right; and because the
 * size is this file's arrangement, not a fact anybody else should repeat.
 *
 * What it is for: a gate that names an IST slot is a promise the processor
 * switches stacks, and the only way to see whether it kept it is where the
 * frame ended up.  A double fault whose frame is on the stack that overflowed
 * is the failure this exists to prevent, and it looks exactly like success
 * until the stack is the problem.
 */
int desc_on_ist_stack(uint32_t cpu_id, unsigned slot, uint64_t addr)
{
	uint64_t top;

	if (cpu_id >= SMP_MAX_CPUS || slot == 0 || slot > IST_COUNT)
		return 0;

	top = tss[cpu_id].ist[slot - 1];

	return addr < top && addr >= top - DESC_STACK_SIZE;
}

/*
 * Where a trap from ring 3 lands on this processor, from now on (#422).
 *
 * ⚠️ This was written ONCE, when the task-state segment was built, and pointed
 * at a four-kilobyte stack belonging to the processor rather than to any
 * thread.  That is correct exactly while no thread ever enters the kernel from
 * ring 3 — which was true until there was a user task, and stops being true on
 * the first one.
 *
 * Two things break without it.  A thread that faults and then BLOCKS leaves
 * its kernel state on a stack the next thread to enter on this processor will
 * push over; and the frame thread_set_state() wrote into a thread's own stack
 * is not where the processor will build the next one, so the two disagree
 * about where a thread's user registers live.  Neither has a symptom at the
 * point of the mistake.
 *
 * The switch calls this, so the answer is always the thread that is running.
 */
void desc_set_rsp0(uint32_t cpu_id, uint64_t rsp0)
{
	if (cpu_id >= SMP_MAX_CPUS)
		panic("desc: set the stack of a processor past the tables");

	tss[cpu_id].rsp0 = rsp0;
}

void desc_activate(uint32_t cpu_id)
{
	load_gdt();
	__asm__ volatile("ltr %w0" : : "r"(tss_selector(cpu_id)));

	trap_init();
}
