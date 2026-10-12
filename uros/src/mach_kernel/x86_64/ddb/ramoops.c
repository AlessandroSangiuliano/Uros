/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * The console kept in RAM for Linux to read back after a reset (#373).
 *
 * On the bare metal of #595 there is no serial port, and the screen was read
 * off a phone's video, which drops lines.  Linux, on each machine, reserves a
 * megabyte at a fixed address and loads ramoops on it; when Linux boots after
 * this kernel has reset the machine, ramoops reads what it finds there in the
 * format of its console zone and gives it back as
 * /sys/fs/pstore/console-ramoops-0.  This file writes that format: every byte
 * the console says, from the first one kept before the zone existed (#666) to
 * the reset's own lines (cpu/reset.c).
 *
 * The format, from fs/pstore/ram_core.c (Linux master, read 10/10/2026):
 *
 *	struct persistent_ram_buffer {
 *		uint32_t    sig;	PERSISTENT_RAM_SIG ^ the zone's: 0x43474244
 *		atomic_t    start;	where the next byte goes
 *		atomic_t    size;	how many bytes it holds
 *		uint8_t     data[];
 *	};
 *
 * What it holds is data[start..size) followed by data[0..start): a ring, read
 * oldest first.  ramoops takes it only when size is at most the zone less
 * these twelve bytes and start is at most size; anything else is "found
 * existing invalid buffer", and the zone is wiped.  That rule is why
 * zone_put() writes the header in the order it does.
 *
 * Measured on 10/10/2026: a zone written this way from outside Linux, from
 * /dev/mem, was read back by Linux's ramoops after `reboot' on omen and on
 * OMEGA, size for size (~/uros-tests/ramoops/ramoops-prova.sh).  ACPI 6.5,
 * 4.8.3.6, calls the reset "the logical equivalent to power cycling the
 * system": that memory survives it is a property of those machines, measured,
 * and every new one is tested the same way before its log is trusted to it.
 */

#include <stdint.h>
#include <kern/misc_protos.h>
#include <kern/cpu_number.h>
#include <boot/bootarg.h>
#include <cpu/regs.h>
#include <ddb/fbcons.h>
#include <ddb/ramoops.h>
#include <pmap/bootmem.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/tlb.h>
#include <pmap/walk.h>
#include <sync/atomic.h>

#define	RAMOOPS_SIG		0x43474244u	/* "DBGC", the console zone's */

struct ramoops_buffer {
	uint32_t	sig;
	uint32_t	start;
	uint32_t	size;
	uint8_t		data[];
};

_Static_assert(sizeof(struct ramoops_buffer) == 12,
	       "ramoops' persistent_ram_buffer header is twelve bytes");

static volatile struct ramoops_buffer	*rz;
static uint64_t				rz_pa;
static uint32_t				rz_cap;		/* data[]'s bytes */
static uint32_t				rz_n;		/* bytes taken, all of them */
static uint32_t				rz_seeded;	/* of them, #666's */
static uint32_t				rz_hash;	/* FNV-1a of them, in order */
static volatile uint32_t		rz_dropped;
static volatile int			rz_live;

#define	FNV_OFFSET		2166136261u
#define	FNV_PRIME		16777619u

/*
 * Who is writing the zone: 0, or 1 + that processor's number.  The console
 * hands bytes over from more than one place (cons.c), not all under one lock,
 * and an NMI or a fault can print in the middle of a byte on the same
 * processor -- which must not wait for itself, so it is told apart and its
 * byte counted as dropped rather than spun on.
 */
static volatile uint32_t		rz_owner;

/* Far longer than one byte takes, even uncached; a holder that never lets go -- parked by the debugger -- costs this once per byte. */
#define	RZ_SPINS		100000u

int ramoops_wanted(uint64_t *pa)
{
	uint64_t	a;
	int		r = boot_value("ramoops", &a);

	if (r <= 0)
		return r;
	if (a == 0 || (a & (PAGE_SIZE_4K - 1)) != 0
	    || a > UINT64_MAX - RAMOOPS_RESERVED)
		return -1;
	*pa = a;
	return 1;
}

/*
 * The console zone's pages, uncached in the one mapping that reaches them.
 *
 * The direct map covers all of memory write-back, with pages of a gigabyte
 * where the processor has them, and the zone is in it.  A second mapping of
 * the same pages uncached would give them two memory types at once, which
 * the processor does not support; so the direct map's own leaves over the
 * zone are made uncached instead, after splitting its large page down to
 * four kilobytes around them (split_selftest() splits the direct map the same
 * way).  PCD and PWT with the 4 KiB PAT bit clear select PAT entry 3, UC,
 * which this kernel never rewrites (pmap.c rewrites entry 1, for WC).
 *
 * Uncached so that a byte written is a byte in memory: a reset does not write
 * the caches back, and nor does the reset button of a machine that hung.
 *
 * Returns how many of the zone's pages read back uncached.  Called once, on
 * the boot processor, before the others are started: the TLB flush is local,
 * and the others load a CR3 whose tables already say this.
 */
static unsigned zone_uncached(uint64_t pa)
{
	uint64_t	root = read_cr3() & INTEL_PTE_PFN;
	uint64_t	off, size;
	pt_entry_t	*e;
	unsigned	ok = 0;

	for (off = 0; off < RAMOOPS_CONSOLE; off += PAGE_SIZE_4K) {
		uint64_t	va = phys_to_direct(pa + off);
		pt_entry_t	old, seen;

		e = pmap_walk(root, va, &size);
		while (e != PT_ENTRY_NULL && size > PAGE_SIZE_4K) {
			if (pmap_split_page(PMAP_NULL, va) == 0)
				break;
			e = pmap_walk(root, va, &size);
		}
		if (e == PT_ENTRY_NULL || size != PAGE_SIZE_4K)
			continue;

		/* One exchange over what was read: the processor may set
		 * ACCESSED or DIRTY in the word meanwhile (#604). */
		old = *e;
		for (;;) {
			seen = atomic_cmpxchg64((volatile uint64_t *)e, old,
						old | INTEL_PTE_NCACHE
						| INTEL_PTE_WTHRU);
			if (seen == old)
				break;
			old = seen;
		}
	}
	tlb_flush_all(PMAP_NULL);

	/*
	 * And no line of the zone left in a cache from when it was write-back:
	 * one written back later would land over what was written uncached.
	 * After the flush above there is no write-back mapping of it to bring
	 * a line in again.
	 */
	for (off = 0; off < RAMOOPS_CONSOLE; off += 64)
		__asm__ volatile("clflush %0"
				 : : "m"(*(volatile const char *)(uintptr_t)
					 phys_to_direct(pa + off)));
	__asm__ volatile("mfence" : : : "memory");

	for (off = 0; off < RAMOOPS_CONSOLE; off += PAGE_SIZE_4K) {
		e = pmap_walk(root, phys_to_direct(pa + off), &size);
		/* INTEL_PTE_PS is bit 7, which in a 4 KiB leaf is PAT. */
		if (e != PT_ENTRY_NULL && size == PAGE_SIZE_4K
		    && (*e & (INTEL_PTE_NCACHE | INTEL_PTE_WTHRU))
		       == (INTEL_PTE_NCACHE | INTEL_PTE_WTHRU)
		    && (*e & INTEL_PTE_PS) == 0)
			ok++;
	}
	return ok;
}

/*
 * One byte into the ring, for a caller that is the only writer.
 *
 * 🔑 SIZE BEFORE START.  The machine can reset between any two of these
 * stores -- a hard hang and a hand on the button -- and ramoops wipes a zone
 * whose start is past its size.  Written in this order, every state in
 * between is one ramoops takes: before the ring wraps, start and size are the
 * same number, and a reset between the two stores leaves size one ahead,
 * which reads as the last byte first and the rest in order.  The other order
 * would leave start one ahead, and the whole log would be wiped.  Once the
 * ring has wrapped, size stays at the capacity and only start moves.
 */
static void zone_put(char c)
{
	uint32_t	n = rz_n;

	rz->data[n % rz_cap] = (uint8_t)c;
	rz_hash = (rz_hash ^ (uint8_t)c) * FNV_PRIME;
	n++;
	rz_n = n;
	rz->size = n < rz_cap ? n : rz_cap;
	rz->start = n % rz_cap;
}

static int zone_enter(uint32_t me)
{
	unsigned	i;

	for (i = 0; i < RZ_SPINS; i++) {
		uint32_t seen = atomic_cmpxchg32(&rz_owner, 0, me);

		if (seen == 0)
			return 1;
		if (seen == me)
			return 0;	/* this processor, inside its own byte */
		cpu_pause();
	}
	return 0;
}

void ramoops_putc(char c)
{
	uint64_t	flags;
	uint32_t	me;

	if (!rz_live)
		return;

	flags = read_rflags();
	interrupts_disable();
	me = 1 + (uint32_t)cpu_number();
	if (zone_enter(me)) {
		zone_put(c);
		__asm__ volatile("" : : : "memory");
		rz_owner = 0;
	} else
		atomic_add32(&rz_dropped, 1);
	if (flags & RFLAGS_IF)
		interrupts_enable();
}

void ramoops_init(void)
{
	uint64_t	pa = 0;
	int		r = ramoops_wanted(&pa);
	unsigned	uncached;
	const char	*early;
	uint32_t	kept, offered, i;

	if (r == 0)
		return;
	if (r < 0) {
		printf("ramoops: ramoops= is not a page-aligned address: no "
		       "zone (#373)\n");
		return;
	}

	/*
	 * Cut out when the frames were first counted (pmap/bootmem.c), or not
	 * at all: not all of it in the memory this kernel hands out -- the
	 * address of another machine, or one inside the kernel and its modules
	 * -- or no room left to cut it out.  Then nothing is written there,
	 * since it may be anything.
	 */
	if (!boot_frames_cut(pa, RAMOOPS_RESERVED)) {
		printf("ramoops: 0x%llx..0x%llx is not all inside the memory "
		       "this kernel hands out, or there was no room to cut it "
		       "out: no zone, and nothing written there (#373)\n",
		       (unsigned long long)pa,
		       (unsigned long long)(pa + RAMOOPS_RESERVED - 1));
		return;
	}

	uncached = zone_uncached(pa);
	if (uncached != RAMOOPS_CONSOLE / PAGE_SIZE_4K) {
		printf("ramoops: only %u of the zone's %llu pages read back "
		       "uncached: no zone (#373)\n", uncached,
		       (unsigned long long)(RAMOOPS_CONSOLE / PAGE_SIZE_4K));
		return;
	}

	/*
	 * Started empty, the signature last: a reset in between leaves either
	 * the old signature, which ramoops ignores, or this one over an empty
	 * zone.  What Linux left there is its own console zone, which with its
	 * console off is empty anyway.
	 */
	rz = (volatile struct ramoops_buffer *)(uintptr_t)phys_to_direct(pa);
	rz_pa = pa;
	rz_cap = (uint32_t)(RAMOOPS_CONSOLE - sizeof(struct ramoops_buffer));
	rz_n = 0;
	rz_hash = FNV_OFFSET;
	rz->start = 0;
	rz->size = 0;
	rz->sig = RAMOOPS_SIG;

	kept = fbcons_early(&early, &offered);
	for (i = 0; i < kept; i++)
		zone_put(early[i]);
	rz_seeded = kept;
	rz_live = 1;

	printf("ramoops: the zone at 0x%llx: the megabyte out of the memory "
	       "this kernel hands out, its first %llu KiB uncached (%u pages "
	       "read back PCD and PWT) and holding the console for Linux's "
	       "ramoops, from the %u bytes kept before it existed",
	       (unsigned long long)pa,
	       (unsigned long long)(RAMOOPS_CONSOLE / 1024), uncached, kept);
	if (offered > kept)
		printf(" -- and without the %u after the first %u", offered - kept,
		       kept);
	printf(" (#373)\n");
}

/*
 * The zone read back the way ramoops will read it, and compared with what was
 * written, at the moment it matters: the reset (cpu/reset.c).  The header's
 * three words, read and not remembered; while the ring has not wrapped, the
 * bytes in ramoops' order hashed and compared with the hash taken as they
 * were written; and their beginning compared with what #666 kept, which is
 * where they must begin.  A zone this calls WRONG is one Linux would wipe or
 * read wrong; one it passes may still not survive a machine's reset, which
 * only Linux after it can say (ramoops-prova.sh, ramoops-readback.py).
 */
void ramoops_check(void)
{
	uint32_t	sig, start, size, n, h, i;
	uint32_t	want_size, want_start, kept, offered;
	const char	*early;
	const char	*wrong = 0;

	if (!rz_live)
		return;

	n = rz_n;
	want_size = n < rz_cap ? n : rz_cap;
	want_start = n % rz_cap;
	sig = rz->sig;
	start = rz->start;
	size = rz->size;

	if (sig != RAMOOPS_SIG)
		wrong = "the signature is not the console zone's";
	else if (size != want_size || start != want_start)
		wrong = "start and size are not the ones written";
	else if (n <= rz_cap) {
		h = FNV_OFFSET;
		for (i = 0; i < size; i++)
			h = (h ^ rz->data[i]) * FNV_PRIME;
		kept = fbcons_early(&early, &offered);
		if (kept > rz_seeded)
			kept = rz_seeded;
		if (h != rz_hash)
			wrong = "its bytes do not hash as they were written";
		for (i = 0; wrong == 0 && i < kept; i++)
			if (rz->data[i] != (uint8_t)early[i])
				wrong = "it does not begin with the bytes kept "
					"before it existed";
	}

	if (wrong != 0)
		printf("reset: the zone at 0x%llx does NOT read back as ramoops "
		       "would read it -- %s: signature 0x%08x, start %u, size "
		       "%u, where %u bytes were written -- WRONG (#373)\n",
		       (unsigned long long)rz_pa, wrong, sig, start, size, n);
	else if (n <= rz_cap)
		printf("reset: the zone at 0x%llx reads back as ramoops would "
		       "read it: signature, start %u and size %u as written, "
		       "the %u bytes hashing as written and beginning with the "
		       "%u kept before it existed, %u dropped (#373)\n",
		       (unsigned long long)rz_pa, start, size, size, rz_seeded,
		       rz_dropped);
	else
		printf("reset: the zone at 0x%llx reads back as ramoops would "
		       "read it: signature, start %u and size %u as written; it "
		       "has wrapped, %u bytes overwritten, so the hash and the "
		       "beginning are not asked, %u dropped (#373)\n",
		       (unsigned long long)rz_pa, start, size, n - rz_cap,
		       rz_dropped);
}

int ramoops_live(uint64_t *pa, uint32_t *bytes, uint32_t *dropped)
{
	if (!rz_live)
		return 0;
	*pa = rz_pa;
	*bytes = rz_n;
	*dropped = rz_dropped;
	return 1;
}
