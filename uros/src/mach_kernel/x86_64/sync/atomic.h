/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 atomic primitives (#410, MD contract 5/6).
 *
 * Written as explicit instructions with an explicit `lock`, the way the
 * i386 side is, rather than left to the compiler's atomic builtins.  The
 * contract asks for every primitive to be verified as a real atomic instead
 * of an optimistic sequence that happens to work, and an instruction that is
 * written down can be read back out of the disassembly and checked. One
 * that was requested cannot.
 *
 * Every locked instruction here is also a full barrier — see barrier.h — so
 * code that has just performed one does not additionally need smp_mb().
 * That redundancy is invisible at the call site, which is why it is said in
 * both places.
 */

#ifndef _X86_64_SYNC_ATOMIC_H_
#define _X86_64_SYNC_ATOMIC_H_

#include <stdint.h>

#include <sync/barrier.h>

/*
 * Plain aligned loads and stores of a machine word are already atomic on
 * this architecture, and TSO already orders them against each other.  What
 * is left to prevent is the compiler folding, duplicating or reordering
 * them, which is what volatile buys — so these carry no instruction and are
 * not a weaker form of the operations below, they are the whole thing.
 */
static inline uint64_t atomic_load64(const volatile uint64_t *p)
{
	return *p;
}

static inline void atomic_store64(volatile uint64_t *p, uint64_t v)
{
	*p = v;
}

/* Add, and answer with the value that was there before. */
static inline uint64_t atomic_add64(volatile uint64_t *p, uint64_t v)
{
	__asm__ volatile("lock xaddq %0, %1"
			 : "+r"(v), "+m"(*p)
			 : : "memory", "cc");
	return v;
}

static inline uint32_t atomic_add32(volatile uint32_t *p, uint32_t v)
{
	__asm__ volatile("lock xaddl %0, %1"
			 : "+r"(v), "+m"(*p)
			 : : "memory", "cc");
	return v;
}

static inline uint64_t atomic_inc64(volatile uint64_t *p)
{
	return atomic_add64(p, 1);
}

static inline uint64_t atomic_dec64(volatile uint64_t *p)
{
	return atomic_add64(p, (uint64_t)-1);
}

/*
 * Compare and exchange, answering with the value actually found.  The caller
 * compares that against what it expected: equal means the exchange happened.
 *
 * Returning the value rather than a boolean is deliberate — a failed attempt
 * hands back what it lost to, which is what a retry loop needs and would
 * otherwise have to re-read, possibly seeing a third value by then.
 */
static inline uint64_t atomic_cmpxchg64(volatile uint64_t *p,
					uint64_t expect, uint64_t fresh)
{
	__asm__ volatile("lock cmpxchgq %2, %1"
			 : "+a"(expect), "+m"(*p)
			 : "r"(fresh)
			 : "memory", "cc");
	return expect;
}

static inline uint32_t atomic_cmpxchg32(volatile uint32_t *p,
					uint32_t expect, uint32_t fresh)
{
	__asm__ volatile("lock cmpxchgl %2, %1"
			 : "+a"(expect), "+m"(*p)
			 : "r"(fresh)
			 : "memory", "cc");
	return expect;
}

/*
 * A byte-wide compare-exchange (#452).
 *
 * The kernel mutex is one byte with three states, and its uncontended
 * acquire is exactly this: free -> held, or tell me who got there first.
 * A swap cannot express it — swap always writes, so it cannot fail without
 * having already disturbed the word, and the whole point of the fast path
 * is that a failed attempt leaves no trace for the winner's cache line.
 */
static inline uint8_t atomic_cmpxchg8(volatile uint8_t *p,
				      uint8_t expect, uint8_t fresh)
{
	__asm__ volatile("lock cmpxchgb %2, %1"
			 : "+a"(expect), "+m"(*p)
			 : "r"(fresh)
			 : "memory", "cc");
	return expect;
}

/*
 * Exchange unconditionally, answering with the previous value.
 *
 * No `lock` is written because xchg with a memory operand asserts it by
 * itself — the one instruction where its absence does not mean the access
 * is unlocked.  Anyone auditing the disassembly for `lock` should know to
 * expect this one bare.
 */
static inline uint64_t atomic_swap64(volatile uint64_t *p, uint64_t v)
{
	__asm__ volatile("xchgq %0, %1"
			 : "+r"(v), "+m"(*p)
			 : : "memory");
	return v;
}

static inline uint8_t atomic_swap8(volatile uint8_t *p, uint8_t v)
{
	__asm__ volatile("xchgb %0, %1"
			 : "+q"(v), "+m"(*p)
			 : : "memory");
	return v;
}

/* Set a bit, answering with what it was. */
static inline int atomic_test_and_set_bit(volatile uint64_t *p, unsigned bit)
{
	uint8_t was;

	__asm__ volatile("lock btsq %2, %1\n\t"
			 "setc %0"
			 : "=qm"(was), "+m"(*p)
			 : "Ir"((uint64_t)bit)
			 : "memory", "cc");
	return was;
}

/* Clear a bit, answering with what it was. */
static inline int atomic_test_and_clear_bit(volatile uint64_t *p, unsigned bit)
{
	uint8_t was;

	__asm__ volatile("lock btrq %2, %1\n\t"
			 "setc %0"
			 : "=qm"(was), "+m"(*p)
			 : "Ir"((uint64_t)bit)
			 : "memory", "cc");
	return was;
}

/*
 * Compare and exchange sixteen bytes at once — the operation i386 has no
 * equivalent of, and the reason a pointer and a counter can be swapped
 * together instead of being packed into one word to dodge ABA.
 *
 * `expect` is read and written: on failure it comes back holding what was
 * actually there, so a retry loop has the current pair without re-reading
 * it non-atomically.  Answers 1 when the exchange happened.
 *
 * ⚠️ Two conditions, and neither is checked here because both belong to the
 * caller and checking them per operation would cost more than the operation:
 *   - the address must be 16-byte aligned, or this raises #GP;
 *   - the CPU must have the instruction — cpu_has_cmpxchg16b() — since the
 *     earliest AMD64 parts lack it.
 */
struct atomic128 {
	uint64_t lo;
	uint64_t hi;
};

static inline int atomic_cmpxchg128(volatile struct atomic128 *p,
				    struct atomic128 *expect,
				    struct atomic128 fresh)
{
	uint8_t won;

	__asm__ volatile("lock cmpxchg16b %1\n\t"
			 "setz %0"
			 : "=q"(won), "+m"(*p),
			   "+a"(expect->lo), "+d"(expect->hi)
			 : "b"(fresh.lo), "c"(fresh.hi)
			 : "memory", "cc");
	return won;
}

#endif	/* _X86_64_SYNC_ATOMIC_H_ */
