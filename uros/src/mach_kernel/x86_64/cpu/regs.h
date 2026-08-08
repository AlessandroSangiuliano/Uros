/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 machine register access: control registers, MSRs, CPUID.
 *
 * Shared plumbing rather than one contract's property — the pmap needs
 * EFER and CR3, the trap path will need CR2, the protection posture needs
 * CR4, and the KPTI decision of ch.11 §11.6 is a CPUID query.  It lives in
 * cpu/ so none of them owns it.
 *
 * Assembly includes this too, for the numbers rather than the accessors: an
 * entry path that reads an MSR before it has a stack cannot call anything,
 * and a register number written out a second time in a .S file is a number
 * that will disagree with this one eventually.  So the constants are outside
 * the __ASSEMBLER__ guards and everything that generates code is inside.
 */

#ifndef _X86_64_CPU_REGS_H_
#define _X86_64_CPU_REGS_H_

#ifndef __ASSEMBLER__

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Port I/O                                                            */
/* ------------------------------------------------------------------ */
static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t r;
	__asm__ volatile("inb %1, %0" : "=a"(r) : "Nd"(port));
	return r;
}

#endif	/* __ASSEMBLER__ */

/* ------------------------------------------------------------------ */
/*  Control registers                                                   */
/* ------------------------------------------------------------------ */
#define CR0_MP		(1UL << 1)	/* monitor coprocessor              */
#define CR0_EM		(1UL << 2)	/* set: no FPU, emulate it          */
#define CR0_TS		(1UL << 3)	/* the lazy-FPU trap; not used here */
#define CR0_WP		(1UL << 16)	/* kernel honours read-only pages   */
#define CR0_PG		(1UL << 31)	/* paging enabled                   */

#define CR4_PAE		(1UL << 5)	/* physical address extension       */
#define CR4_PGE		(1UL << 7)	/* global pages enabled             */
#define CR4_FSGSBASE	(1UL << 16)	/* ring 3 may write its own bases   */
#define CR4_OSFXSR	(1UL << 9)	/* FXSAVE/FXRSTOR and SSE usable    */
#define CR4_OSXMMEXCPT	(1UL << 10)	/* SIMD errors raise #XF, not #UD   */
#define CR4_PCIDE	(1UL << 17)	/* process-context identifiers      */
#define CR4_OSXSAVE	(1UL << 18)	/* XSAVE and XCR0 usable            */
#define CR4_SMEP	(1UL << 20)	/* no kernel execute of user pages  */
#define CR4_SMAP	(1UL << 21)	/* no kernel access of user pages   */

#ifndef __ASSEMBLER__

static inline uint64_t read_cr0(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr0, %0" : "=r"(v));
	return v;
}

static inline void write_cr0(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}

/* The faulting address, set by the CPU on a page fault. */
static inline uint64_t read_cr2(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr2, %0" : "=r"(v));
	return v;
}

/* Physical address of the PML4, plus flags in the low bits. */
static inline uint64_t read_cr3(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr3, %0" : "=r"(v));
	return v;
}

/*
 * Writing CR3 also flushes every non-global TLB entry, which is why it is
 * the blunt way to make paging changes visible.
 */
static inline void write_cr3(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr3" : : "r"(v) : "memory");
}

/*
 * Drop the TLB entry for one address — the surgical alternative to
 * reloading CR3.  Required whenever an existing entry changes; a mapping
 * that was not present before has nothing cached to drop, but doing it
 * unconditionally costs one instruction and removes a subtle case.
 *
 * This handles the local CPU only.  Making a change visible to the others
 * is a shootdown, which is its own problem later in this contract.
 */
static inline void invlpg(uint64_t va)
{
	__asm__ volatile("invlpg (%0)" : : "r"(va) : "memory");
}

/*
 * Tell the CPU it is spinning on a lock rather than doing work.
 *
 * It is a hint with real effects: it lets a hyperthreaded sibling have the
 * pipeline instead of losing it to a loop that computes nothing, and it
 * avoids the memory-order violation penalty a tight read loop pays when the
 * line it is watching finally changes under it.  Not decoration — a spin
 * loop without it is slower and steals from the thread doing the work the
 * spinner is waiting for.
 */
static inline void cpu_pause(void)
{
	__asm__ volatile("pause" ::: "memory");
}

/*
 * Whether this processor will take an interrupt, and the two ways to change
 * the answer.
 *
 * The memory clobbers are not decoration.  Enabling interrupts is a point
 * after which a handler may run and observe anything this code has written,
 * and disabling them is a point before which it must not — so neither may
 * have loads or stores moved across it by the compiler.  The processor's own
 * ordering is a separate question, and one x86 mostly answers for us.
 */
static inline void interrupts_enable(void)
{
	__asm__ volatile("sti" ::: "memory");
}

static inline void interrupts_disable(void)
{
	__asm__ volatile("cli" ::: "memory");
}

#endif	/* __ASSEMBLER__ */

#define RFLAGS_IF	(1UL << 9)

/*
 * The resume flag, which suppresses instruction breakpoints for exactly one
 * instruction and is then cleared by the processor.  It is what a debug
 * exception handler sets in the flags it is returning to when it means to
 * resume at the instruction that trapped, rather than trap on it again.
 */
#define RFLAGS_RF	(1UL << 16)

#ifndef __ASSEMBLER__

static inline int interrupts_enabled(void)
{
	uint64_t flags;

	__asm__ volatile("pushfq; popq %0" : "=r"(flags) :: "memory");
	return (flags & RFLAGS_IF) != 0;
}

static inline uint64_t read_cr4(void)
{
	uint64_t v;
	__asm__ volatile("mov %%cr4, %0" : "=r"(v));
	return v;
}

static inline void write_cr4(uint64_t v)
{
	__asm__ volatile("mov %0, %%cr4" : : "r"(v) : "memory");
}

#endif	/* __ASSEMBLER__ */

/* ------------------------------------------------------------------ */
/*  Debug registers                                                     */
/* ------------------------------------------------------------------ */
/*
 * DR0 holds an address and DR7 says what to do about it.  The one
 * combination used here is the simplest: break when the instruction at that
 * address is about to execute, which is a fault reported *before* it runs —
 * the only kind that can be aimed at an instruction and observe the state
 * that instruction was going to change.
 *
 * R/W0 and LEN0 are both zero for that case, which is why neither appears
 * below: an execution breakpoint has no length and no direction.  Bit 10 is
 * reserved and must be written as one.
 */
#define DR7_L0			(1UL << 0)	/* DR0 armed         */
#define DR7_RESERVED_ONE	(1UL << 10)
#define DR7_EXEC_DR0		(DR7_L0 | DR7_RESERVED_ONE)

/*
 * And the same for the other three, because a debugger wants more than one
 * (#428).  L<n> is bit 2n; the type and length fields for slot n sit at bits
 * 16+4n and 18+4n, and both are zero for an execution breakpoint -- which is
 * why the arming below writes only the enable bits.
 *
 * ⚠️ FOUR, and that is the hardware's number, not a policy.  A debugger that
 * silently accepted a fifth would be arming nothing and reporting success.
 */
#define DR_COUNT		4
#define DR7_LOCAL(n)		(1UL << (2 * (n)))

/* DR6 says which one fired.  Bit n for slot n. */
#define DR6_HIT(n)		(1UL << (n))

/*
 * ⚠️ DR6 is sticky: the processor sets the bit for the breakpoint that fired
 * and never clears it again.  A handler that does not clear it leaves the
 * next debug exception describing this one as well.
 */
#ifndef __ASSEMBLER__

static inline void write_dr0(uint64_t v)
{
	__asm__ volatile("mov %0, %%dr0" : : "r"(v));
}

static inline void write_dr7(uint64_t v)
{
	__asm__ volatile("mov %0, %%dr7" : : "r"(v) : "memory");
}

static inline void write_dr6(uint64_t v)
{
	__asm__ volatile("mov %0, %%dr6" : : "r"(v));
}

static inline uint64_t read_dr7(void)
{
	uint64_t v;

	__asm__ volatile("mov %%dr7, %0" : "=r"(v));
	return v;
}

static inline uint64_t read_dr0(void)
{
	uint64_t v;

	__asm__ volatile("mov %%dr0, %0" : "=r"(v));
	return v;
}

static inline uint64_t read_dr6(void)
{
	uint64_t v;

	__asm__ volatile("mov %%dr6, %0" : "=r"(v));
	return v;
}

static inline void write_dr(unsigned n, uint64_t v)
{
	switch (n) {
	case 0: __asm__ volatile("mov %0, %%dr0" : : "r"(v)); break;
	case 1: __asm__ volatile("mov %0, %%dr1" : : "r"(v)); break;
	case 2: __asm__ volatile("mov %0, %%dr2" : : "r"(v)); break;
	case 3: __asm__ volatile("mov %0, %%dr3" : : "r"(v)); break;
	default: break;
	}
}

#endif	/* __ASSEMBLER__ */

/* ------------------------------------------------------------------ */
/*  Model-specific registers                                            */
/* ------------------------------------------------------------------ */
#define MSR_EFER		0xC0000080

/*
 * The segment bases that long mode keeps in MSRs rather than in descriptors.
 *
 * ⚠️ Loading %fs or %gs as a segment register zeroes the corresponding base,
 * so these must be written after any such load, not before.
 *
 * IA32_KERNEL_GS_BASE is the other half of the pair swapgs exchanges with
 * IA32_GS_BASE: the kernel's per-CPU pointer is parked there while user code
 * runs, and one instruction on entry brings it back.
 */
#define MSR_FS_BASE		0xC0000100
#define MSR_GS_BASE		0xC0000101
#define MSR_KERNEL_GS_BASE	0xC0000102

#define EFER_SCE	(1UL << 0)	/* SYSCALL/SYSRET enabled (#411)     */
#define EFER_LME	(1UL << 8)	/* long mode enabled  (boot.S sets)  */
#define EFER_LMA	(1UL << 10)	/* long mode active   (read-only)    */
#define EFER_NXE	(1UL << 11)	/* execute-disable bit is usable     */

#ifndef __ASSEMBLER__

static inline uint64_t rdmsr(uint32_t msr)
{
	uint32_t lo, hi;

	__asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
	return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
	__asm__ volatile("wrmsr"
			 : : "c"(msr), "a"((uint32_t)val),
			     "d"((uint32_t)(val >> 32)));
}

/* ------------------------------------------------------------------ */
/*  CPUID                                                               */
/* ------------------------------------------------------------------ */
static inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
			       uint32_t *a, uint32_t *b,
			       uint32_t *c, uint32_t *d)
{
	__asm__ volatile("cpuid"
			 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
			 : "a"(leaf), "c"(subleaf));
}

static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
			 uint32_t *c, uint32_t *d)
{
	cpuid_count(leaf, 0, a, b, c, d);
}

/*
 * 1 GiB pages are an *optional* feature — unlike 2 MiB pages, which every
 * x86-64 part has.  Every modern CPU reports it, but the direct map has to
 * ask rather than assume: discovering its absence by triple-fault on an old
 * machine is not a diagnosis.
 *
 * The leaf itself is optional too, so check the highest extended leaf the
 * CPU implements before reading it.
 */
static inline int cpu_has_1gb_pages(void)
{
	uint32_t a, b, c, d;

	cpuid(0x80000000, &a, &b, &c, &d);
	if (a < 0x80000001)
		return 0;

	cpuid(0x80000001, &a, &b, &c, &d);
	return (d & (1U << 26)) != 0;		/* PDPE1GB */
}

/*
 * A 128-bit compare-exchange, which i386 has no equivalent of at all: it is
 * what lets a pointer and a counter be swapped as one word, and so what
 * makes an ABA-safe lock-free structure possible without packing the two
 * into a single 64-bit value.
 *
 * Optional, and genuinely absent on the earliest AMD64 parts — so it is
 * asked for rather than assumed, like the 1 GiB pages.  Anything relying on
 * it needs a path for when the answer is no.
 */
/*
 * This processor's APIC id, from CPUID rather than from the APIC.
 *
 * The two agree, and this one can be asked earlier: it needs no mapping, no
 * enable bit and no memory, which is what makes it the identity a processor
 * can use before it has anything else.  The trampoline in boot/ already
 * relies on that — it is how an arriving processor finds its own stack
 * without being handed one.
 */
/*
 * The extended control register, which says which state components XSAVE
 * will move.  Reached by its own instruction pair rather than by an MSR,
 * and only once CR4.OSXSAVE says the kernel is prepared to manage them.
 */
static inline uint64_t xgetbv(uint32_t index)
{
	uint32_t lo, hi;

	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
	return ((uint64_t)hi << 32) | lo;
}

static inline void xsetbv(uint32_t index, uint64_t value)
{
	__asm__ volatile("xsetbv"
			 : : "a"((uint32_t)value), "d"((uint32_t)(value >> 32)),
			     "c"(index)
			 : "memory");
}

#endif	/* __ASSEMBLER__ */

#define XCR0_X87	(1UL << 0)	/* always set; the unit cannot be off */
#define XCR0_SSE	(1UL << 1)	/* the XMM registers                  */
#define XCR0_AVX	(1UL << 2)	/* their upper halves                 */

#ifndef __ASSEMBLER__

static inline uint32_t cpu_apic_id(void)
{
	uint32_t a, b, c, d;

	cpuid(1, &a, &b, &c, &d);
	return b >> 24;
}

static inline int cpu_has_cmpxchg16b(void)
{
	uint32_t a, b, c, d;

	cpuid(1, &a, &b, &c, &d);
	return (c & (1U << 13)) != 0;
}

/*
 * SMEP and SMAP: the kernel may not execute, respectively may not read or
 * write, a page marked user-accessible.  Both are optional features on a
 * leaf that is itself optional, so ask for the leaf before reading it.
 *
 * SMAP has a deliberate escape — the AC flag, set and cleared by stac and
 * clac — for the places the kernel really does mean to touch user memory.
 * Nothing needs it yet: no mapping this kernel makes sets the user bit.
 */
static inline int cpu_has_smep(void)
{
	uint32_t a, b, c, d;

	cpuid(0, &a, &b, &c, &d);
	if (a < 7)
		return 0;

	cpuid_count(7, 0, &a, &b, &c, &d);
	return (b & (1U << 7)) != 0;
}

/*
 * Whether this processor has RDFSBASE and its relatives — the instructions
 * that read and write the segment bases without an MSR access, and which
 * CR4.FSGSBASE makes available at ring 3 as well as ring 0.
 *
 * Asked rather than assumed for the usual reason, and asked at all because
 * the answer being yes is what makes the bit a decision: see
 * percpu_activate() for the one that was made and why.
 */
static inline int cpu_has_fsgsbase(void)
{
	uint32_t a, b, c, d;

	cpuid(0, &a, &b, &c, &d);
	if (a < 7)
		return 0;

	cpuid_count(7, 0, &a, &b, &c, &d);
	return (b & (1U << 0)) != 0;
}

static inline int cpu_has_smap(void)
{
	uint32_t a, b, c, d;

	cpuid(0, &a, &b, &c, &d);
	if (a < 7)
		return 0;

	cpuid_count(7, 0, &a, &b, &c, &d);
	return (b & (1U << 20)) != 0;
}

/* Fills a 13-byte buffer with the NUL-terminated vendor string. */
static inline void cpu_vendor(char *out13)
{
	uint32_t a, b, c, d;
	uint32_t words[3];

	cpuid(0, &a, &b, &c, &d);
	words[0] = b;				/* the order is ebx, edx, ecx */
	words[1] = d;
	words[2] = c;

	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 4; j++)
			out13[i * 4 + j] = (char)(words[i] >> (j * 8));
	out13[12] = '\0';
}

#endif	/* __ASSEMBLER__ */

#endif	/* _X86_64_CPU_REGS_H_ */
