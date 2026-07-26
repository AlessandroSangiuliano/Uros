/*
 * Copyright (c) 2026 Alessandro Sangiuliano (Slex) <alex22_7@hotmail.com>
 * SPDX-License-Identifier: MIT
 *
 * x86-64 boot: first C running in long mode (#406, MD contract 1/6).
 *
 * boot.S enters long mode, jumps to the higher-half virtual address, sets a
 * kernel stack, and calls here.  This file is the first machine-dependent C
 * of the x86-64 port — compiled -mcmodel=kernel so its absolute references
 * resolve into the top-2 GiB higher-half where the image is linked.
 *
 * It installs the definitive machine state that the boot GDT (a bare
 * transitional table at a low physical address) only stood in for:
 *   - a 64-bit GDT living at the kernel's own high address;
 *   - a TSS with RSP0 (the stack a ring-3 -> ring-0 transition lands on) and
 *     IST1 (an unconditional stack for a chosen vector, e.g. #DF later);
 *   - TR loaded to point at that TSS.
 *
 * Then it announces that the contract is met and halts.  The trap/interrupt
 * wiring that actually consults RSP0/IST is a later MD contract (#428 DDB,
 * the IDT); this establishes the structures they will use.
 */

#include <stdint.h>

#include <boot/multiboot2.h>
#include <cpu/acpi.h>
#include <cpu/desc.h>
#include <cpu/ipi.h>
#include <cpu/lapic.h>
#include <cpu/percpu.h>
#include <cpu/pic.h>
#include <cpu/smp.h>
#include <cpu/regs.h>
#include <cpu/tss.h>
#include <pmap/bootmem.h>
#include <pmap/direct.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <pmap/tlb.h>
#include <pmap/walk.h>
#include <syscall/probe.h>
#include <syscall/syscall.h>
#include <thread/context.h>
#include <thread/fpu.h>
#include <thread/state.h>
#include <sync/atomic.h>
#include <sync/barrier.h>
#include <sync/lock.h>
#include <trap/trap.h>

#define COM1 0x3F8

/* ------------------------------------------------------------------ */
/*  Minimal polled serial (COM1) — enough to narrate the bring-up.     */
/* ------------------------------------------------------------------ */
static void kputc(char c)
{
	while (!(inb(COM1 + 5) & 0x20))		/* wait THR empty */
		;
	outb(COM1, (uint8_t)c);
}

static void kputs(const char *s)
{
	for (; *s; s++)
		kputc(*s);
}

static void kputhex64(uint64_t v)
{
	kputs("0x");
	for (int i = 60; i >= 0; i -= 4) {
		uint8_t nib = (v >> i) & 0xF;
		kputc(nib < 10 ? '0' + nib : 'a' + (nib - 10));
	}
}

static void kputdec(uint64_t v)
{
	char buf[20];
	int i = 0;

	if (v == 0) {
		kputc('0');
		return;
	}
	while (v > 0) {
		buf[i++] = '0' + (v % 10);
		v /= 10;
	}
	while (i > 0)
		kputc(buf[--i]);
}

/* ------------------------------------------------------------------ */
/*  pte.h against reality                                               */
/* ------------------------------------------------------------------ */
/*
 * Decode the higher-half base with the new index macros and check the
 * answer against the tables we are demonstrably running on: boot.S built
 * this window by hand as PML4[511] -> PDPT[510] -> PD[0].  If the macros
 * agree, they describe the live hardware, not just an intention.
 */
static void pte_selftest(void)
{
	const uint64_t va = 0xffffffff80000000ULL;
	uint64_t l4 = pml4_index(va), l3 = pdpt_index(va);
	uint64_t l2 = pd_index(va),   l1 = pt_index(va);

	kputs("UrMach x86-64: pte decode ");
	kputhex64(va);
	kputs(" -> pml4[");
	kputdec(l4);
	kputs("] pdpt[");
	kputdec(l3);
	kputs("] pd[");
	kputdec(l2);
	kputs("] pt[");
	kputdec(l1);
	kputs(l4 == 511 && l3 == 510 && l2 == 0 && l1 == 0
	      ? "] agrees with the live boot tables\r\n"
	      : "] MISMATCH against the live boot tables\r\n");
}

/*
 * layout.h pins its constants with _Static_assert at build time, so the
 * only thing left to establish here is the live fact: that the address we
 * are actually executing from falls inside the region the layout calls the
 * kernel image, and is canonical.
 */
static void layout_selftest(void)
{
	uint64_t here = (uint64_t)(uintptr_t)&layout_selftest;

	kputs("UrMach x86-64: layout image base ");
	kputhex64(KERNEL_IMAGE_BASE);
	kputs(", direct map ");
	kputhex64(DIRECT_MAP_BASE);
	kputs("\r\nUrMach x86-64: running from ");
	kputhex64(here);
	kputs(va_in_kernel_image(here) && va_is_canonical(here)
	      ? ", inside the image region and canonical\r\n"
	      : ", OUTSIDE the image region\r\n");
}

/*
 * A witness with a value we can recognise, living in the kernel image's
 * .rodata — so it is loaded, and reachable both at its linked high address
 * and at the physical address the loader put it at.
 */
static const volatile uint32_t phys_witness = 0x5ec0ffee;

/*
 * Prove kernel_va_to_phys() against the machine instead of trusting the
 * arithmetic: read the witness through the kernel image's high mapping, and
 * again through the low identity map that boot.S left in place, at the
 * physical address the macro computes.  Two routes to the same byte — if
 * they agree, the translation is the one the loader actually used.
 */
static void phys_selftest(void)
{
	uint64_t va = (uint64_t)(uintptr_t)&phys_witness;
	uint64_t pa = kernel_va_to_phys(va);
	uint32_t through_image = phys_witness;
	uint32_t through_identity = *(const volatile uint32_t *)(uintptr_t)pa;

	kputs("UrMach x86-64: witness at va ");
	kputhex64(va);
	kputs(" -> pa ");
	kputhex64(pa);
	kputs("\r\nUrMach x86-64: reads ");
	kputhex64(through_image);
	kputs(" via image, ");
	kputhex64(through_identity);
	kputs(through_image == through_identity && through_image == 0x5ec0ffee
	      ? " via identity, agree\r\n"
	      : " via identity, DISAGREE\r\n");
}

/*
 * What the machine says about itself.  The page size the direct map can use
 * is a question for the CPU, not an assumption — and long mode had better
 * be reporting itself active, since we are executing in it.
 */
static void cpu_selftest(void)
{
	char vendor[13];
	uint64_t efer = rdmsr(MSR_EFER);

	cpu_vendor(vendor);
	kputs("UrMach x86-64: cpu ");
	kputs(vendor);
	kputs(cpu_has_1gb_pages() ? ", 1 GiB pages available"
				  : ", no 1 GiB pages (2 MiB fallback)");
	kputs(efer & EFER_LMA ? ", long mode active\r\n"
			      : ", EFER.LMA CLEAR?!\r\n");
}

/*
 * Open the boot information GRUB left us and report what memory the machine
 * says it has.  Until now the handoff pointer has only been carried; this is
 * the first thing to read it, and the answer is what the direct map has to
 * be sized against rather than guessed at.
 */
static void memmap_selftest(uint32_t info)
{
	const struct mb2_tag_mmap *mm;
	const uint8_t *p, *end;
	uint64_t top = mb2_top_of_ram(info);
	uint64_t usable = mb2_usable_ram(info);
	unsigned shown = 0;

	mm = (const struct mb2_tag_mmap *)mb2_find_tag(info,
						       MB2_TAG_MEMORY_MAP);
	if (mm == 0) {
		kputs("UrMach x86-64: NO multiboot2 memory map\r\n");
		return;
	}

	p = (const uint8_t *)mm + sizeof(*mm);
	end = (const uint8_t *)mm + mm->size;

	for (; p + mm->entry_size <= end; p += mm->entry_size) {
		const struct mb2_mmap_entry *e;

		e = (const struct mb2_mmap_entry *)p;
		if (e->type != MB2_MEM_AVAILABLE)
			continue;

		kputs("UrMach x86-64:   ram ");
		kputhex64(e->addr);
		kputs(" + ");
		kputdec(e->len / 1024);
		kputs(" KiB\r\n");
		shown++;
	}

	kputs("UrMach x86-64: ");
	kputdec(shown);
	kputs(" usable regions, ");
	kputdec(usable / (1024 * 1024));
	kputs(" MiB total, top of ram ");
	kputhex64(top);
	kputs(top != 0 && usable != 0 ? "\r\n" : " NOTHING?!\r\n");
}

/*
 * Build the direct map, then prove it by reading the witness a third way:
 * not through the image mapping, not through the low identity map, but at
 * DIRECT_MAP_BASE + its physical address.  If that yields the same word,
 * every physical page is now an addition away.
 */
static void direct_map_selftest(uint32_t info)
{
	uint64_t pa = kernel_va_to_phys((uint64_t)(uintptr_t)&phys_witness);
	uint64_t top = mb2_top_of_ram(info);
	uint32_t through_direct;

	direct_map_init(top);

	kputs("UrMach x86-64: direct map at ");
	kputhex64(DIRECT_MAP_BASE);
	kputs(", ");
	kputdec(direct_map_covered / (1024 * 1024));
	kputs(" MiB in ");
	kputdec(direct_map_page_size / (1024 * 1024));
	kputs(" MiB pages, ");
	kputs(direct_map_covered >= top ? "reaches all ram\r\n"
					: "CLAMPED below top of ram\r\n");

	through_direct = *(const volatile uint32_t *)(uintptr_t)phys_to_direct(pa);

	kputs("UrMach x86-64: witness via direct map ");
	kputhex64(through_direct);
	kputs(through_direct == 0x5ec0ffee
	      ? ", agrees\r\n"
	      : ", DISAGREES\r\n");
}

/*
 * Walk the live tables for three addresses whose answers we already know by
 * other means, so the walk is checked rather than merely exercised:
 *
 *   - the witness in the kernel image, whose physical address the
 *     subtraction in kernel_va_to_phys() gives independently;
 *   - its direct-map alias, which must land on the same physical page;
 *   - an address in the kernel heap region, which nothing has mapped, so
 *     the walk has to report failure instead of inventing a number.
 */
static void walk_selftest(void)
{
	uint64_t root = read_cr3() & INTEL_PTE_PFN;
	uint64_t va = (uint64_t)(uintptr_t)&phys_witness;
	uint64_t expect = kernel_va_to_phys(va);
	uint64_t pa = 0, size = 0;
	int ok;

	ok = pmap_resolve(root, va, &pa, &size);
	kputs("UrMach x86-64: walk image ");
	kputhex64(va);
	kputs(" -> ");
	kputhex64(pa);
	kputs(" in ");
	kputdec(size / 1024);
	kputs(" KiB page, ");
	kputs(ok && pa == expect ? "matches the subtraction\r\n"
				 : "DISAGREES with the subtraction\r\n");

	ok = pmap_resolve(root, phys_to_direct(expect), &pa, &size);
	kputs("UrMach x86-64: walk direct ");
	kputhex64(phys_to_direct(expect));
	kputs(" -> ");
	kputhex64(pa);
	kputs(" in ");
	kputdec(size / 1024);
	kputs(" KiB page, ");
	kputs(ok && pa == expect ? "same physical page\r\n"
				 : "WRONG physical page\r\n");

	ok = pmap_resolve(root, KERNEL_HEAP_BASE, &pa, &size);
	kputs("UrMach x86-64: walk unmapped ");
	kputhex64(KERNEL_HEAP_BASE);
	kputs(ok ? " -> claims a mapping?!\r\n"
		 : " -> correctly reports no mapping\r\n");
}

/*
 * Three frames from the boot pool, checked for the properties a page table
 * depends on: 4 KiB aligned (the hardware would otherwise read a table from
 * the wrong address), distinct from each other, and actually zero when read
 * back through the direct map.
 */
static void bootmem_selftest(uint32_t info)
{
	uint64_t a, b, c, mark;
	const volatile uint64_t *first;
	int clean = 1;

	boot_frame_init(info);
	mark = boot_frame_low_water();

	kputs("UrMach x86-64: frames start above ");
	kputhex64(mark);
	kputs(", ");
	kputdec(boot_frames_total());
	kputs(" available (");
	kputdec(boot_frames_total() * PAGE_SIZE_4K / (1024 * 1024));
	kputs(" MiB)\r\n");

	a = boot_frame_alloc();
	b = boot_frame_alloc();
	c = boot_frame_alloc();

	first = (const volatile uint64_t *)(uintptr_t)phys_to_direct(a);
	for (unsigned i = 0; i < PAGE_SIZE_4K / sizeof(uint64_t); i++)
		if (first[i] != 0)
			clean = 0;

	kputs("UrMach x86-64: boot frames ");
	kputhex64(a);
	kputs(" ");
	kputhex64(b);
	kputs(" ");
	kputhex64(c);
	kputs("\r\nUrMach x86-64: ");
	kputs(a && b && c
	      && ((a | b | c) & (PAGE_SIZE_4K - 1)) == 0
	      && a != b && b != c && a != c
	      && a >= mark && b >= mark && c >= mark
	      && clean
	      ? "aligned, distinct, clear of the image, and zeroed\r\n"
	      : "BAD frames\r\n");
}

/*
 * Map a page where the walk just proved nothing was mapped, then establish
 * that it is really there — by writing through the new address and finding
 * the value on the physical frame via the direct map, and by having the walk
 * resolve the address back to that same frame.
 *
 * This is also the first 4 KiB leaf in the port: every mapping so far has
 * been a large page, so the PT level and the walk's bottom case have had no
 * coverage until now.
 *
 * Then the refusal: asking to map inside the direct map, which a 1 GiB or
 * 2 MiB page already covers, must be reported as blocked rather than
 * silently splitting that page.
 */
static void map_selftest(void)
{
	uint64_t root = read_cr3() & INTEL_PTE_PFN;
	uint64_t va = KERNEL_HEAP_BASE;
	uint64_t frame = boot_frame_alloc();
	uint64_t got = 0, size = 0;
	volatile uint32_t *page;
	uint32_t via_direct;
	int rc, resolved;

	rc = pmap_map_page(root, va, frame, INTEL_PTE_WRITE | INTEL_PTE_NX);

	kputs("UrMach x86-64: map ");
	kputhex64(va);
	kputs(" -> frame ");
	kputhex64(frame);
	kputs(rc == PMAP_MAP_OK ? ", mapped\r\n" : ", FAILED\r\n");

	page = (volatile uint32_t *)(uintptr_t)va;
	*page = 0x0dd1e5;

	via_direct = *(const volatile uint32_t *)(uintptr_t)phys_to_direct(frame);
	resolved = pmap_resolve(root, va, &got, &size);

	kputs("UrMach x86-64: wrote through the new page, frame reads ");
	kputhex64(via_direct);
	kputs(via_direct == 0x0dd1e5 ? ", agrees\r\n" : ", DISAGREES\r\n");

	kputs("UrMach x86-64: walk now resolves it to ");
	kputhex64(got);
	kputs(" in ");
	kputdec(size / 1024);
	kputs(" KiB page, ");
	kputs(resolved && got == frame && size == PAGE_SIZE_4K
	      ? "as mapped\r\n" : "WRONG\r\n");

	rc = pmap_map_page(root, DIRECT_MAP_BASE, frame, INTEL_PTE_WRITE);
	kputs("UrMach x86-64: map over a large page ");
	kputs(rc == PMAP_MAP_BLOCKED ? "correctly refused\r\n"
				     : "NOT refused?!\r\n");
}

/*
 * Reprotect then unmap a fresh page.  With no fault handler yet, a write to
 * a read-only page would triple-fault rather than report anything, so the
 * check reads the PTE the walk returns instead: after clearing write the
 * bit is gone while the frame and validity remain, and a read still returns
 * the value (read-only forbids writes, not reads).  Then unmap clears it and
 * the walk finds nothing.
 */
static void protect_unmap_selftest(void)
{
	uint64_t root = read_cr3() & INTEL_PTE_PFN;
	uint64_t va = KERNEL_HEAP_BASE + 0x4000;
	uint64_t frame = boot_frame_alloc();
	volatile uint32_t *page = (volatile uint32_t *)(uintptr_t)va;
	pt_entry_t *e;
	uint64_t size;

	pmap_map_page(root, va, frame, INTEL_PTE_WRITE | INTEL_PTE_NX);
	*page = 0xcafe;

	size = pmap_protect_page(root, va, INTEL_PTE_NX);	/* drop write */
	e = pmap_walk(root, va, 0);
	kputs("UrMach x86-64: protect read-only -> entry ");
	kputhex64(*e);
	kputs(size == PAGE_SIZE_4K && !(*e & INTEL_PTE_WRITE)
	      && (*e & INTEL_PTE_VALID) && pte_to_pa(*e) == frame
	      ? "\r\nUrMach x86-64: write bit cleared, frame kept, reads "
	      : "\r\nUrMach x86-64: WRONG, reads ");
	kputhex64(*page);
	kputs(*page == 0xcafe ? " still\r\n" : " CORRUPT\r\n");

	size = pmap_unmap_page(root, va);
	e = pmap_walk(root, va, 0);
	kputs("UrMach x86-64: unmap -> ");
	kputs(size == PAGE_SIZE_4K && e == PT_ENTRY_NULL
	      ? "cleared, walk finds nothing\r\n"
	      : "STILL MAPPED?!\r\n");
}

/*
 * Split the direct map's large page that covers the witness, and show it
 * changed nothing observable: the address still resolves to the same
 * physical page, one size class finer, and reading through the direct map
 * still returns the witness.  CPU-independent — a 1 GiB leaf becomes 2 MiB,
 * a 2 MiB leaf becomes 4 KiB — so both models exercise a different level.
 */
static void split_selftest(void)
{
	uint64_t root = read_cr3() & INTEL_PTE_PFN;
	uint64_t wpa = kernel_va_to_phys((uint64_t)(uintptr_t)&phys_witness);
	uint64_t dva = phys_to_direct(wpa);
	const volatile uint32_t *p = (const volatile uint32_t *)(uintptr_t)dva;
	uint64_t before = 0, after = 0, s0 = 0, s1 = 0, newsz;
	uint32_t read_before = *p, read_after;

	pmap_resolve(root, dva, &before, &s0);
	newsz = pmap_split_page(root, dva);
	pmap_resolve(root, dva, &after, &s1);
	read_after = *p;

	kputs("UrMach x86-64: split direct page ");
	kputdec(s0 / 1024);
	kputs(" KiB -> ");
	kputdec(s1 / 1024);
	kputs(" KiB, pa ");
	kputhex64(after);
	kputs(before == after ? " kept" : " CHANGED");
	kputs(", reads ");
	kputhex64(read_after);
	kputs(newsz == s1 && s1 < s0 && before == after
	      && read_after == read_before && read_after == 0x5ec0ffee
	      ? ", mapping intact\r\n"
	      : ", SPLIT BROKE THE MAP\r\n");
}

/*
 * Adopt the live tables as the kernel pmap and check the two things this
 * layer adds: that its root is the CR3 we are running on, and that the
 * protection translation gives the bits each case should — read maps to NX
 * only (no write, no execute), read/write adds write, anything executable
 * clears NX, and full access is write with execute allowed.
 */
static void pmap_selftest(void)
{
	uint64_t cr3root = read_cr3() & INTEL_PTE_PFN;
	pmap_t k;
	int prot_ok;

	pmap_bootstrap();
	k = pmap_kernel();

	kputs("UrMach x86-64: kernel pmap root ");
	kputhex64(k->root_pa);
	kputs(k->root_pa == cr3root ? " == CR3\r\n" : " != CR3?!\r\n");

	prot_ok =
	    pmap_flags_for_prot(VM_PROT_READ) == INTEL_PTE_NX &&
	    pmap_flags_for_prot(VM_PROT_READ | VM_PROT_WRITE)
		    == (INTEL_PTE_WRITE | INTEL_PTE_NX) &&
	    pmap_flags_for_prot(VM_PROT_READ | VM_PROT_EXECUTE) == 0 &&
	    pmap_flags_for_prot(VM_PROT_ALL) == INTEL_PTE_WRITE;

	kputs("UrMach x86-64: prot R=NX RW=W|NX RX=exec RWX=W: ");
	kputs(prot_ok ? "all correct\r\n" : "WRONG\r\n");
}

/*
 * A mapping's whole life through the pmap_t interface rather than the naked
 * primitives: enter it read/write and confirm extract finds the frame and a
 * write lands; reprotect read-only and confirm the write bit clears; remove
 * it and confirm extract now finds nothing.  This is the shape the MI side
 * will call.
 */
static void pmap_verbs_selftest(void)
{
	pmap_t k = pmap_kernel();
	uint64_t va = KERNEL_HEAP_BASE + 0x8000;
	uint64_t frame = boot_frame_alloc();
	volatile uint32_t *page = (volatile uint32_t *)(uintptr_t)va;
	pt_entry_t *e;
	uint64_t got;
	int rc;

	rc = pmap_enter(k, va, frame, VM_PROT_READ | VM_PROT_WRITE, 0);
	*page = 0xbeef;
	got = pmap_extract(k, va);
	kputs("UrMach x86-64: pmap_enter RW, extract -> ");
	kputhex64(got);
	kputs(rc == PMAP_MAP_OK && got == frame && *page == 0xbeef
	      ? ", frame matches, writable\r\n" : ", WRONG\r\n");

	pmap_protect(k, va, va + PAGE_SIZE_4K, VM_PROT_READ);
	e = pmap_walk(k->root_pa, va, 0);
	kputs("UrMach x86-64: pmap_protect READ -> ");
	kputs(e && !(*e & INTEL_PTE_WRITE) ? "write bit cleared\r\n"
					   : "WRONG\r\n");

	pmap_remove(k, va, va + PAGE_SIZE_4K);
	got = pmap_extract(k, va);
	kputs("UrMach x86-64: pmap_remove -> extract ");
	kputhex64(got);
	kputs(got == 0 ? ", gone\r\n" : ", STILL MAPPED?!\r\n");
}

/*
 * Build the physical index, then put one physical page into three mappings
 * across two address spaces and check the index can name them all — that is
 * the question the page tables cannot answer, and the reason it exists.
 *
 * The third mapping matters on its own: the head of a list is an entry, so
 * the first mapping costs no allocation and only the ones after it come off
 * the free list.  Removing the head is the awkward case, since the head
 * cannot move — its address is what identifies the page — so its successor
 * has to be copied up into it.
 */
static void pv_selftest(uint32_t info)
{
	pmap_t k = pmap_kernel();
	pmap_t u;
	uint64_t frame;
	uint64_t va1 = KERNEL_HEAP_BASE + 0x10000;
	uint64_t va2 = KERNEL_HEAP_BASE + 0x11000;
	uint64_t uva = 0x0000000200000000ULL;

	pv_bootstrap(mb2_top_of_ram(info));

	frame = boot_frame_alloc();
	kputs("UrMach x86-64: pv index covers ");
	kputhex64(frame);
	kputs(pv_managed(frame) ? ", page is managed" : ", NOT MANAGED");
	kputs(pv_count(frame) == 0 ? ", no mappings yet\r\n"
				   : ", ALREADY MAPPED?!\r\n");

	u = pmap_create(0);
	pmap_enter(k, va1, frame, VM_PROT_READ | VM_PROT_WRITE, 0);
	pmap_enter(k, va2, frame, VM_PROT_READ, 0);
	pmap_enter(u, uva, frame, VM_PROT_READ | VM_PROT_WRITE, 0);

	kputs("UrMach x86-64: one page mapped three ways -> pv_count ");
	kputdec(pv_count(frame));
	kputs(pv_count(frame) == 3 ? ", all three found\r\n" : ", WRONG\r\n");

	/* Drop the head's own mapping; the other two must survive it. */
	pmap_remove(k, va1, va1 + PAGE_SIZE_4K);
	kputs("UrMach x86-64: removed the head mapping -> pv_count ");
	kputdec(pv_count(frame));
	kputs(pv_count(frame) == 2
	      && pmap_extract(k, va2) == frame
	      && pmap_extract(u, uva) == frame
	      ? ", the other two intact\r\n" : ", WRONG\r\n");

	pmap_remove(k, va2, va2 + PAGE_SIZE_4K);
	pmap_remove(u, uva, uva + PAGE_SIZE_4K);
	kputs("UrMach x86-64: removed the rest -> pv_count ");
	kputdec(pv_count(frame));
	kputs(pv_count(frame) == 0 ? ", page is free of mappings\r\n"
				   : ", STILL LISTED\r\n");

	pmap_destroy(u);
}

/*
 * The operations that start from a physical page, which the index has just
 * made possible.  One frame is mapped twice in the kernel and once in a
 * second address space, so a change made by physical address has to reach
 * all three.
 *
 * pmap_page_protect(READ) across them is exactly the copy-on-write arming
 * step: after it, no mapping of the page can be written, so whichever holder
 * writes next faults and a copy is made only then.
 */
static void phys_ops_selftest(void)
{
	pmap_t k = pmap_kernel();
	pmap_t u = pmap_create(0);
	uint64_t frame = boot_frame_alloc();
	uint64_t kva1 = KERNEL_HEAP_BASE + 0x20000;
	uint64_t kva2 = KERNEL_HEAP_BASE + 0x21000;
	uint64_t uva = 0x0000000300000000ULL;
	volatile uint32_t *p = (volatile uint32_t *)(uintptr_t)kva1;
	pt_entry_t *e1, *e2, *e3;
	int ref, mod;

	pmap_enter(k, kva1, frame, VM_PROT_READ | VM_PROT_WRITE, 0);
	pmap_enter(k, kva2, frame, VM_PROT_READ | VM_PROT_WRITE, 0);
	pmap_enter(u, uva, frame, VM_PROT_READ | VM_PROT_WRITE, 0);

	/* Clear first: the frame was just zeroed, which is itself a write. */
	pmap_clear_reference(frame);
	pmap_clear_modify(frame);
	ref = pmap_is_referenced(frame);
	mod = pmap_is_modified(frame);
	kputs("UrMach x86-64: after clearing, referenced=");
	kputdec(ref);
	kputs(" modified=");
	kputdec(mod);
	kputs(ref == 0 && mod == 0 ? ", both clear\r\n" : ", NOT CLEAR\r\n");

	(void)*p;				/* a read sets accessed */
	ref = pmap_is_referenced(frame);
	mod = pmap_is_modified(frame);
	kputs("UrMach x86-64: after a read, referenced=");
	kputdec(ref);
	kputs(" modified=");
	kputdec(mod);
	kputs(ref == 1 && mod == 0 ? ", read seen, not a write\r\n"
				   : ", WRONG\r\n");

	*p = 0xf00d;				/* a write sets dirty */
	kputs("UrMach x86-64: after a write, modified=");
	kputdec(pmap_is_modified(frame));
	kputs(pmap_is_modified(frame) == 1 ? ", write seen\r\n" : ", WRONG\r\n");

	/* Arm copy-on-write: no mapping of this page may be written. */
	pmap_page_protect(frame, VM_PROT_READ);
	e1 = pmap_walk(k->root_pa, kva1, 0);
	e2 = pmap_walk(k->root_pa, kva2, 0);
	e3 = pmap_walk(u->root_pa, uva, 0);
	kputs("UrMach x86-64: page_protect READ -> ");
	kputs(e1 && e2 && e3
	      && !(*e1 & INTEL_PTE_WRITE) && !(*e2 & INTEL_PTE_WRITE)
	      && !(*e3 & INTEL_PTE_WRITE)
	      && pte_to_pa(*e1) == frame && pte_to_pa(*e3) == frame
	      ? "all three mappings read-only, frames kept\r\n"
	      : "WRONG\r\n");

	pmap_page_protect(frame, VM_PROT_NONE);
	kputs("UrMach x86-64: page_protect NONE -> pv_count ");
	kputdec(pv_count(frame));
	kputs(pv_count(frame) == 0
	      && pmap_extract(k, kva1) == 0 && pmap_extract(k, kva2) == 0
	      && pmap_extract(u, uva) == 0
	      ? ", every mapping gone\r\n" : ", STILL MAPPED\r\n");

	pmap_destroy(u);
}

/*
 * Zero and copy on physical pages.  The interesting check is not that a copy
 * copies, but that a partial one touches exactly its own range: a zero that
 * runs one byte wide destroys a neighbour's data, and a copy that stops one
 * byte short leaves a page subtly wrong — neither announces itself.
 */
static void phys_copy_selftest(void)
{
	uint64_t a = boot_frame_alloc();
	uint64_t b = boot_frame_alloc();
	uint8_t *pa = (uint8_t *)(uintptr_t)phys_to_direct(a);
	uint8_t *pb = (uint8_t *)(uintptr_t)phys_to_direct(b);
	int ok;

	/* A pattern that differs byte to byte, so a misplaced copy shows. */
	for (unsigned i = 0; i < PAGE_SIZE_4K; i++)
		pa[i] = (uint8_t)(i * 7 + 1);

	pmap_zero_page(b);
	pmap_copy_page(a, b);

	ok = 1;
	for (unsigned i = 0; i < PAGE_SIZE_4K; i++)
		if (pb[i] != (uint8_t)(i * 7 + 1))
			ok = 0;

	kputs("UrMach x86-64: copy_page whole page ");
	kputs(ok ? "matches byte for byte\r\n" : "DIFFERS\r\n");

	/* Zero [0x100, 0x180) and require the bytes either side to survive. */
	pmap_zero_part_page(b, 0x100, 0x80);

	ok = pb[0x0ff] == (uint8_t)(0xff * 7 + 1)
	  && pb[0x180] == (uint8_t)(0x180 * 7 + 1);
	for (unsigned i = 0x100; i < 0x180; i++)
		if (pb[i] != 0)
			ok = 0;

	kputs("UrMach x86-64: zero_part_page ");
	kputs(ok ? "cleared its range and nothing either side\r\n"
		 : "SPILLED\r\n");

	/* Copy 16 bytes from one offset to a different one. */
	pmap_zero_page(b);
	pmap_copy_part_page(a, 0x40, b, 0x200, 16);

	ok = pb[0x1ff] == 0 && pb[0x210] == 0;
	for (unsigned i = 0; i < 16; i++)
		if (pb[0x200 + i] != (uint8_t)((0x40 + i) * 7 + 1))
			ok = 0;

	kputs("UrMach x86-64: copy_part_page ");
	kputs(ok ? "landed at its offset, bounds clean\r\n" : "WRONG\r\n");
}

/* A mutable global — lands in .bss, the writable region. */
static volatile uint32_t wx_data_probe;

/*
 * Apply W^X to the image, then check each section came out with the right
 * bits: .text executable and not writable, .rodata neither, .data/.bss
 * writable and not executable.  The strongest check is liveness: this code
 * is in .text and keeps running, and the write below lands — so text stayed
 * executable and data stayed writable across the reprotection, with CR0.WP
 * now binding the read-only pages.
 */
static void wx_selftest(void)
{
	pmap_t k = pmap_kernel();
	uint64_t root = k->root_pa;
	pt_entry_t *t, *r, *d;

	pmap_protect_kernel();

	t = pmap_walk(root, (uint64_t)(uintptr_t)&wx_selftest, 0);
	r = pmap_walk(root, (uint64_t)(uintptr_t)&phys_witness, 0);
	d = pmap_walk(root, (uint64_t)(uintptr_t)&wx_data_probe, 0);

	kputs("UrMach x86-64: W^X text ");
	kputs(t && !(*t & INTEL_PTE_WRITE) && !(*t & INTEL_PTE_NX)
	      ? "RX" : "WRONG");
	kputs(", rodata ");
	kputs(r && !(*r & INTEL_PTE_WRITE) && (*r & INTEL_PTE_NX)
	      ? "RO+NX" : "WRONG");
	kputs(", data ");
	kputs(d && (*d & INTEL_PTE_WRITE) && (*d & INTEL_PTE_NX)
	      ? "RW+NX\r\n" : "WRONG\r\n");

	wx_data_probe = 0x1234;
	kputs("UrMach x86-64: still executing .text, wrote .data = ");
	kputhex64(wx_data_probe);
	kputs(wx_data_probe == 0x1234 ? ", W^X live\r\n" : ", WRONG\r\n");

	/*
	 * The rest of the posture.  Each is reported as offered or not,
	 * because "enabled" on a CPU that does not have it would be a comfort
	 * rather than a protection — and the emulator's default model does not
	 * offer them, which is worth seeing.
	 */
	{
		uint64_t on = pmap_enable_smep_smap();
		uint64_t cr4 = read_cr4();

		kputs("UrMach x86-64: SMEP ");
		kputs(!cpu_has_smep() ? "not offered by this cpu"
		      : (cr4 & CR4_SMEP) ? "on" : "OFFERED BUT OFF");
		kputs(", SMAP ");
		kputs(!cpu_has_smap() ? "not offered by this cpu"
		      : (cr4 & CR4_SMAP) ? "on" : "OFFERED BUT OFF");
		kputs((on & cr4) == on ? "\r\n" : " MISMATCH\r\n");
	}
}

/*
 * An address in the lower half, past the 1 GiB the boot identity map covers,
 * so nothing else has a claim on it.  It shares PML4 slot 0 with that
 * identity map, which makes it a sharper test: the two spaces must differ
 * beneath the same top-level slot.
 */
#define USER_TEST_VA	0x0000000100000000ULL

/*
 * Build a second address space and show it is one: the kernel half shared
 * entry-for-entry, the lower half its own, and a mapping made in it invisible
 * from the kernel's.
 *
 * Then the part that cannot be faked — switch CR3 to it and keep running.
 * The code executing this, its stack and the page tables it reads are all in
 * the kernel half, so surviving the switch is exactly the proof that the half
 * really is shared and not merely copied to look alike.  A write through the
 * new mapping is read back afterwards from the physical frame, through the
 * direct map, once the kernel space is restored.
 */
static void user_pmap_selftest(void)
{
	pmap_t k = pmap_kernel();
	pmap_t u = pmap_create(0);
	uint64_t frame = boot_frame_alloc();
	const pt_entry_t *kroot, *uroot;
	uint32_t readback;
	int shared = 1;

	if (u == PMAP_NULL) {
		kputs("UrMach x86-64: pmap_create FAILED\r\n");
		return;
	}

	kroot = (const pt_entry_t *)(uintptr_t)phys_to_direct(k->root_pa);
	uroot = (const pt_entry_t *)(uintptr_t)phys_to_direct(u->root_pa);

	for (unsigned i = 256; i < 512; i++)
		if (kroot[i] != uroot[i])
			shared = 0;

	kputs("UrMach x86-64: pmap_create root ");
	kputhex64(u->root_pa);
	kputs(shared ? ", kernel half shared" : ", KERNEL HALF DIFFERS");
	kputs(uroot[0] == 0 ? ", lower half empty\r\n"
			    : ", LOWER HALF NOT EMPTY\r\n");

	pmap_enter(u, USER_TEST_VA, frame, VM_PROT_READ | VM_PROT_WRITE, 0);

	kputs("UrMach x86-64: mapped ");
	kputhex64(USER_TEST_VA);
	kputs(" in the new space -> ");
	kputhex64(pmap_extract(u, USER_TEST_VA));
	kputs(pmap_extract(u, USER_TEST_VA) == frame
	      && pmap_extract(k, USER_TEST_VA) == 0
	      ? ", unmapped in the kernel's, isolated\r\n"
	      : ", NOT ISOLATED\r\n");

	/* The switch.  Everything below runs in the new address space. */
	pmap_activate(u);

	/*
	 * And the write says so.  The page is in the lower half, which since
	 * #411 means ring 3 can reach it, which means SMAP forbids the kernel
	 * from touching it without asking.  This is the kernel asking — the
	 * same bracket copyin and copyout will use, here because this is the
	 * first deliberate access of the kind the tree has.
	 *
	 * Without it the store faults, and it did: on the first boot with a
	 * processor model that has SMAP at all.  The default emulated one does
	 * not, which is why this is a lesson about testing more than one.
	 */
	pmap_user_access_begin();
	*(volatile uint32_t *)(uintptr_t)USER_TEST_VA = 0xa11caca0;
	pmap_user_access_end();

	kputs("UrMach x86-64: running in the new space, wrote through it\r\n");
	pmap_activate(k);

	readback = *(const volatile uint32_t *)(uintptr_t)phys_to_direct(frame);
	kputs("UrMach x86-64: back in the kernel space, frame holds ");
	kputhex64(readback);
	kputs(readback == 0xa11caca0 && pmap_extract(k, USER_TEST_VA) == 0
	      ? ", write survived and the address is gone again\r\n"
	      : ", WRONG\r\n");

	pmap_destroy(u);
	kputs("UrMach x86-64: pmap_destroy -> ");
	kputs(u->ref_count == 0 && u->root_pa == 0 ? "space released\r\n"
						   : "STILL HELD?!\r\n");
}

/*
 * Provoke the protection rather than assert it.  A read-only page can only
 * be shown to be read-only by writing to it and being refused, which needs
 * both a handler to see the refusal and a way to carry on afterwards.
 *
 * The resume point is a label in this same function on purpose: iretq
 * restores the stack pointer along with the instruction pointer, so the
 * frame the faulting store left behind is still exactly right — landing
 * anywhere else would be landing on someone else's stack.
 */
static void wx_enforcement_selftest(void)
{
	volatile void *ro = (volatile void *)(uintptr_t)&phys_witness;
	volatile void *rw = (volatile void *)&wx_data_probe;
	int refused;

	kputs("UrMach x86-64: writing to .rodata on purpose\r\n");
	trap_expect(T_PAGE_FAULT, (uint64_t)(uintptr_t)trap_probe_faulted);
	refused = trap_probe_write(ro);

	/*
	 * Refused, and the witness still holds what it held: the store was
	 * stopped, not merely reported.
	 */
	kputs("UrMach x86-64: W^X ");
	kputs(refused && phys_witness == 0x5ec0ffee
	      ? "holds — the store was refused, .rodata unchanged\r\n"
	      : "BROKEN — the store to .rodata got through\r\n");

	/*
	 * The same probe against a writable page.  Two things need saying
	 * about the result above and only this says them: that the probe does
	 * not report refusal whatever it is given, and that when it reports
	 * success the store really landed — which is why it writes a pattern
	 * rather than a zero, most memory being zero already.
	 */
	wx_data_probe = 0;
	trap_expect(T_PAGE_FAULT, (uint64_t)(uintptr_t)trap_probe_faulted);
	refused = trap_probe_write(rw);

	kputs("UrMach x86-64: the same probe on .bss ");
	kputs(!refused && wx_data_probe == TRAP_PROBE_PATTERN
	      ? "went through and left its pattern, so the refusal was real\r\n"
	      : "MISBEHAVED — the control says nothing\r\n");
}

/*
 * Ask the firmware which processors exist.
 *
 * The count is the whole point: it is what #438 will start, and getting it
 * from the MADT rather than from a build-time constant is what lets one
 * binary boot any machine.  i386 could also read the older MP tables; long
 * mode implies ACPI, so there is one source of truth here where there were
 * two.
 *
 * Enabled and present are different answers — the firmware can describe a
 * socket that is not populated, or one that could be hot-plugged later —
 * and only the enabled ones are startable.
 */
static void acpi_selftest(uint32_t info)
{
	unsigned found = acpi_find_cpus(info);

	if (found == 0) {
		kputs("UrMach x86-64: no usable ACPI — cannot enumerate cpus\r\n");
		return;
	}

	kputs("UrMach x86-64: acpi reports ");
	kputdec(acpi_cpu_count());
	kputs(" processors, ");
	kputdec(acpi_usable_cpu_count());
	kputs(" startable, local apic at ");
	kputhex64(acpi_lapic_base());
	kputs("\r\nUrMach x86-64:   apic ids");
	for (unsigned i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);

		kputs(" ");
		kputdec(c->apic_id);
		if (!c->usable)
			kputs("(off)");
	}
	kputs("\r\n");

	/*
	 * Bring up the local APIC and ask it who we are.  The answer has to
	 * match what ACPI said about one of the processors it listed — if it
	 * names a CPU the firmware never mentioned, one of the two is being
	 * misread, and finding that out here is much cheaper than finding it
	 * out from an IPI that went to nobody.
	 */
	lapic_init(acpi_lapic_base());
	{
		uint32_t id = lapic_id();
		int listed = 0;

		for (unsigned i = 0; i < acpi_cpu_count(); i++)
			if (acpi_cpu(i)->apic_id == id)
				listed = 1;

		kputs("UrMach x86-64: local apic mapped, this cpu is id ");
		kputdec(id);
		kputs(lapic_is_bsp() ? " (the boot processor)" : " (NOT the bsp?!)");
		kputs(listed ? ", and acpi lists it\r\n"
			     : ", WHICH ACPI NEVER MENTIONED\r\n");

		/*
		 * Check the mapping is uncached, by the bits rather than by
		 * behaviour — because behaviour will not tell us.  A cached
		 * mapping of device registers works perfectly under emulation,
		 * where there is no device and no cache to be wrong about, and
		 * fails on hardware in ways that look like the device is
		 * broken.  This is the only place the mistake is visible while
		 * it is still cheap.
		 */
		{
			extern uint64_t lapic_probe_va(void);
			uint64_t va = lapic_probe_va();
			pt_entry_t *e = pmap_walk(pmap_kernel()->root_pa, va, 0);

			kputs("UrMach x86-64: its mapping at ");
			kputhex64(va);
			kputs(e && (*e & INTEL_PTE_NCACHE) && (*e & INTEL_PTE_WTHRU)
			      && (*e & INTEL_PTE_NX) && !(*e & INTEL_PTE_USER)
			      ? " is uncached, NX and kernel-only\r\n"
			      : " HAS THE WRONG CACHING OR PERMISSIONS\r\n");
		}
	}
}

/*
 * The atomics, against what they claim.
 *
 * One CPU cannot show that these are atomic — that needs a second one racing
 * them, and #438 has to land first.  What one CPU can show is that each does
 * the arithmetic it promises and answers with the right value, which is the
 * half that a wrong constraint or a swapped operand breaks. The other half,
 * that each compiles to a genuinely locked instruction rather than a
 * sequence that merely works when nobody is looking, is a question for the
 * disassembly and is checked there (~/uros-tests/audit-atomics-x86_64.sh).
 */
static void atomic_selftest(void)
{
	volatile uint64_t w = 100;
	volatile uint64_t bits = 0;
	struct atomic128 pair __attribute__((aligned(16))) = { 1, 2 };
	struct atomic128 expect, fresh;
	uint64_t old;
	int ok;

	old = atomic_add64(&w, 5);
	ok = old == 100 && w == 105;

	old = atomic_cmpxchg64(&w, 105, 200);		/* should win */
	ok = ok && old == 105 && w == 200;

	old = atomic_cmpxchg64(&w, 105, 300);		/* should lose */
	ok = ok && old == 200 && w == 200;

	old = atomic_swap64(&w, 7);
	ok = ok && old == 200 && w == 7;

	ok = ok && atomic_test_and_set_bit(&bits, 3) == 0 && bits == 8;
	ok = ok && atomic_test_and_set_bit(&bits, 3) == 1;
	ok = ok && atomic_test_and_clear_bit(&bits, 3) == 1 && bits == 0;

	kputs("UrMach x86-64: atomics add/cmpxchg/swap/bit ");
	kputs(ok ? "all answer with the value they replaced\r\n" : "WRONG\r\n");

	if (!cpu_has_cmpxchg16b()) {
		kputs("UrMach x86-64: no cmpxchg16b on this cpu, 128-bit path unusable\r\n");
		return;
	}

	expect = (struct atomic128){ 1, 2 };
	fresh = (struct atomic128){ 3, 4 };
	ok = atomic_cmpxchg128(&pair, &expect, fresh) == 1
	     && pair.lo == 3 && pair.hi == 4;

	/* Now lose, and check it hands back what it lost to. */
	expect = (struct atomic128){ 1, 2 };
	fresh = (struct atomic128){ 5, 6 };
	ok = ok && atomic_cmpxchg128(&pair, &expect, fresh) == 0
	     && expect.lo == 3 && expect.hi == 4
	     && pair.lo == 3 && pair.hi == 4;

	kputs("UrMach x86-64: cmpxchg16b ");
	kputs(ok ? "swaps both words, and reports the pair it lost to\r\n"
		 : "WRONG\r\n");

	/*
	 * The lock, as far as one CPU can take it: free, taken, refused while
	 * held, free again.  What one CPU cannot show is the part that
	 * matters — that two of them cannot both be inside — and that needs
	 * #438 before it can be asked at all.
	 */
	{
		hw_lock_data_t l;

		hw_lock_init(&l);
		ok = !hw_lock_held(&l);
		ok = ok && hw_lock_try(&l) && hw_lock_held(&l);
		ok = ok && !hw_lock_try(&l);		/* already ours */
		hw_lock_unlock(&l);
		ok = ok && !hw_lock_held(&l);
		hw_lock_lock(&l);			/* uncontended, must not spin */
		ok = ok && hw_lock_held(&l);
		hw_lock_unlock(&l);
		ok = ok && !hw_lock_held(&l);

		kputs("UrMach x86-64: hw_lock takes, refuses while held, releases: ");
		kputs(ok ? "as specified\r\n" : "WRONG\r\n");
	}
}

/*
 * The two places the pmap used to say one thing and do another.
 *
 * Wiring was accepted and dropped on the floor, so a caller asking for a
 * page the pager must not reclaim got no such promise. It is now a software
 * bit in the entry, which also means a reprotect must leave it alone — it is
 * not one of the permission bits, and checking that is checking the mask.
 *
 * Destroying a space returned nothing, while reporting the space released.
 * Counting frames either side is the only way to tell the difference: a
 * space costs a root and one table per level it needed, and all of them
 * should come back.
 */
static void reclaim_selftest(void)
{
	pmap_t k = pmap_kernel();
	uint64_t frame = boot_frame_alloc();
	uint64_t va = KERNEL_HEAP_BASE + 0x30000;
	uint64_t uva = 0x0000000400000000ULL;
	uint64_t before, after;
	int on, off, kept;
	pmap_t u;

	pmap_enter(k, va, frame, VM_PROT_READ | VM_PROT_WRITE, 1);
	on = pmap_is_wired(k, va);
	pmap_change_wiring(k, va, 0);
	off = pmap_is_wired(k, va);
	pmap_change_wiring(k, va, 1);
	pmap_protect(k, va, va + PAGE_SIZE_4K, VM_PROT_READ);
	kept = pmap_is_wired(k, va);
	pmap_remove(k, va, va + PAGE_SIZE_4K);

	kputs("UrMach x86-64: wired recorded=");
	kputdec(on);
	kputs(" cleared=");
	kputdec(!off);
	kputs(" survives a reprotect=");
	kputdec(kept);
	kputs(on && !off && kept ? ", all three\r\n" : ", WRONG\r\n");

	before = boot_frames_used();
	u = pmap_create(0);
	pmap_enter(u, uva, frame, VM_PROT_READ | VM_PROT_WRITE, 0);
	pmap_destroy(u);
	after = boot_frames_used();

	kputs("UrMach x86-64: a space cost ");
	kputdec(after >= before ? after - before : before - after);
	kputs(after == before ? " frames after being destroyed, all returned"
			      : " frames it never gave back, LEAKED");
	kputs(pv_count(frame) == 0
	      ? ", and left no entry in the index\r\n"
	      : ", AND LEFT A STALE INDEX ENTRY\r\n");
}

/*
 * Bring up this CPU's block and show %gs actually reaches it.
 *
 * Runs late by necessity: the block is a mapped page, so it needs the frame
 * allocator and the kernel pmap, neither of which exists at the point the
 * descriptor tables go in.  Placing it earlier had percpu_init() find no
 * frame and return without writing the MSR, leaving %gs based at zero —
 * where the boot identity map still has something readable, so %gs:0
 * returned BIOS data instead of faulting.  Exactly the quiet failure the
 * header warns about, and it took the check to see it.
 *
 * The base is read before and after on purpose: the GDT setup loads %gs as
 * a segment register, and in long mode that zeroes the hidden base, so a
 * per-CPU area established earlier would have been silently thrown away.
 * Seeing zero first is that ordering rule being demonstrated rather than
 * asserted in a comment.
 *
 * swapgs is then exercised on its own.  It cannot be tested where it
 * matters — that is kernel entry from ring 3, which does not exist yet — but
 * the instruction and the MSR pairing can be: swap, and %gs holds what
 * IA32_KERNEL_GS_BASE held; swap back, and it is the block again.
 */
static void percpu_selftest(void)
{
	uint64_t before = rdmsr(MSR_GS_BASE);
	struct percpu *p;
	uint64_t swapped, restored;
	/*
	 * The boot processor's own id, not zero.  Nothing guarantees the
	 * firmware started the processor numbered zero — and if it did not,
	 * taking zero here hands the boot processor the block that belongs to
	 * whichever processor really is zero, and the two write over each
	 * other the moment it wakes.
	 */
	uint32_t self = cpu_apic_id();

	percpu_alloc(self);
	percpu_activate(self);
	p = percpu();

	kputs("UrMach x86-64: gs base was ");
	kputhex64(before);
	kputs(" after the GDT load, now ");
	kputhex64(rdmsr(MSR_GS_BASE));
	kputs(before == 0 ? " — the segment load had cleared it\r\n"
			  : " — UNEXPECTED, it was not cleared\r\n");

	kputs("UrMach x86-64: per-cpu block at ");
	kputhex64((uint64_t)(uintptr_t)p);
	kputs(", cpu_id ");
	kputdec(p->cpu_id);
	kputs((uint64_t)(uintptr_t)p == PERCPU_BASE + (uint64_t)self * PAGE_SIZE_4K
	      && p->cpu_id == self
	      ? ", reached through %gs\r\n" : ", WRONG\r\n");

	/* Park a recognisable value in the other half, then swap to it. */
	wrmsr(MSR_KERNEL_GS_BASE, 0xdead0000);
	__asm__ volatile("swapgs" ::: "memory");
	swapped = rdmsr(MSR_GS_BASE);
	__asm__ volatile("swapgs" ::: "memory");
	restored = rdmsr(MSR_GS_BASE);
	wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)(uintptr_t)p);

	kputs("UrMach x86-64: swapgs took gs to ");
	kputhex64(swapped);
	kputs(" and back to ");
	kputhex64(restored);
	kputs(swapped == 0xdead0000 && restored == (uint64_t)(uintptr_t)p
	      ? ", the pair exchanges as it should\r\n" : ", WRONG\r\n");
}

/*
 * Fire one exception of each family and check what came back.
 *
 * Every stub was given its error-code convention by hand from the manual;
 * only the page fault and the double fault have ever fired, so thirty of the
 * thirty-two assignments are still just an intention. A stub that pushes a
 * dummy error code for a vector that supplies its own — or the reverse —
 * shifts every field of the frame by eight bytes, which shows up as a
 * plausible-looking report full of wrong numbers rather than as a crash.
 *
 * So the check is on the frame, not on surviving: the vector must be the one
 * expected, the instruction pointer must land inside the probe that raised
 * it, and the error code must be zero for the three that have none and the
 * offending selector for the one that does.
 */
static void trap_vectors_selftest(void)
{
	static const struct {
		const char *name;
		int	  (*probe)(void);
		uint64_t   vector;
		uint64_t   fn_addr;
	} probes[] = {
		{ "invalid opcode",    trap_probe_ud, T_INVALID_OPCODE,
		  (uint64_t)(uintptr_t)trap_probe_ud },
		{ "breakpoint",        trap_probe_bp, T_BREAKPOINT,
		  (uint64_t)(uintptr_t)trap_probe_bp },
		{ "divide error",      trap_probe_de, T_DIVIDE_ERROR,
		  (uint64_t)(uintptr_t)trap_probe_de },
		{ "general protection", trap_probe_gp, T_GENERAL_PROTECTION,
		  (uint64_t)(uintptr_t)trap_probe_gp },
	};

	for (unsigned i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
		const struct trap_record *t;
		int stepped, rip_ok, error_ok;

		trap_expect(probes[i].vector,
			    (uint64_t)(uintptr_t)trap_probe_faulted);
		stepped = probes[i].probe();
		t = trap_last();

		/*
		 * The probes are aligned to sixteen and are a few instructions
		 * long, so a saved rip within a short distance of the entry is
		 * inside the probe and nowhere else.
		 */
		rip_ok = t->rip >= probes[i].fn_addr
		      && t->rip < probes[i].fn_addr + 64;

		error_ok = probes[i].vector == T_GENERAL_PROTECTION
			   ? t->error == 0x30 : t->error == 0;

		kputs("UrMach x86-64: ");
		kputs(probes[i].name);
		kputs(" -> error ");
		kputhex64(t->error);
		kputs(stepped && t->caught && t->vector == probes[i].vector
		      && rip_ok && error_ok
		      ? ", vector, frame and error code all as they should be\r\n"
		      : ", WRONG\r\n");
	}
}

/*
 * The vectors above the exceptions, which were written by a macro and have
 * never run.
 *
 * Two hundred and twenty-four stubs came out of one .rept, so they are
 * either all right or all wrong in the same way — and the way they would be
 * wrong is the number each pushes, which is the one thing the macro varies.
 * A stub that pushes the wrong vector still returns cleanly; what it breaks
 * is every decision made downstream about which interrupt this was.
 *
 * So the check is what the frame says, not that the machine survived.
 *
 * Both encodings, deliberately: a vector under 128 pushes as a byte and one
 * above it as a full word, so they are different instructions of different
 * lengths, and the alignment the table's arithmetic assumes has to hold for
 * the longer one.
 */
#define PROBE_VECTOR_LOW	0x21	/* free until the legacy IRQs move here */

#define STRINGIFY_(x)		#x
#define STRINGIFY(x)		STRINGIFY_(x)

static volatile unsigned probe_vector_hits;
static volatile uint64_t probe_vector_seen;

static void probe_vector_handler(struct trap_frame *frame)
{
	probe_vector_seen = frame->vector;
	probe_vector_hits++;
}

static void external_vectors_selftest(void)
{
	unsigned hits_low, hits_high;
	uint64_t seen_low, seen_high;

	trap_set_handler(PROBE_VECTOR_LOW, probe_vector_handler);
	trap_set_handler(T_PROBE_VECTOR, probe_vector_handler);

	__asm__ volatile("int $" STRINGIFY(PROBE_VECTOR_LOW));
	hits_low = probe_vector_hits;
	seen_low = probe_vector_seen;

	__asm__ volatile("int $" STRINGIFY(T_PROBE_VECTOR));
	hits_high = probe_vector_hits;
	seen_high = probe_vector_seen;

	kputs("UrMach x86-64: raised vectors ");
	kputhex64(PROBE_VECTOR_LOW);
	kputs(" and ");
	kputhex64(T_PROBE_VECTOR);
	kputs(", handler saw ");
	kputhex64(seen_low);
	kputs(" then ");
	kputhex64(seen_high);
	kputs(hits_low == 1 && hits_high == 2
	      && seen_low == PROBE_VECTOR_LOW && seen_high == T_PROBE_VECTOR
	      ? " — every stub carries its own number\r\n"
	      : " — WRONG\r\n");

	trap_set_handler(PROBE_VECTOR_LOW, 0);
	trap_set_handler(T_PROBE_VECTOR, 0);
}

/*
 * The same vector again, raised by the interrupt controller instead of by an
 * instruction.
 *
 * A software interrupt proves the gate; it proves nothing about the hardware
 * that will actually deliver a message from another processor, because it
 * never goes near it.  A self-interrupt takes the whole of that path — the
 * command register, the delivery logic, the priority arbitration, the
 * acknowledgement — with the one part that could independently be broken,
 * a second processor, left out.
 *
 * Three of them rather than one, and that is the part worth explaining. The
 * APIC holds a vector's priority level busy from delivery until the end-of-
 * interrupt is written, and refuses anything of equal priority while it
 * does. So a handler that forgot to acknowledge would take the first
 * interrupt and no others — the count would stop at one, having looked
 * perfectly successful. Asking for three is how the acknowledgement gets
 * tested rather than assumed.
 */
static volatile unsigned self_ipi_hits;

static void self_ipi_handler(struct trap_frame *frame)
{
	(void)frame;
	self_ipi_hits++;
	lapic_eoi();
}

/*
 * The one the APIC raises on its own initiative, when a line it had decided
 * to report dropped before it got round to reporting it.  Nothing happened,
 * so there is nothing to do — and deliberately no acknowledgement: the APIC
 * never marked the level busy, so telling it the interrupt is finished would
 * be answering for one that was never started.
 */
static void spurious_handler(struct trap_frame *frame)
{
	(void)frame;
}

static void self_ipi_selftest(void)
{
	unsigned delivered = 0;

	trap_set_handler(LAPIC_SPURIOUS_VECTOR, spurious_handler);
	trap_set_handler(T_PROBE_VECTOR, self_ipi_handler);

	/* The legacy controller was silenced with the descriptor tables. */
	lapic_enable();
	interrupts_enable();

	for (unsigned round = 0; round < 3; round++) {
		unsigned before = self_ipi_hits;
		uint64_t spins;

		lapic_send_self(T_PROBE_VECTOR);

		/*
		 * Bounded, because the failure this is looking for is one that
		 * never arrives — and waiting forever for it would replace a
		 * report with a hang.
		 */
		for (spins = 0; spins < 100000000ULL; spins++) {
			if (self_ipi_hits != before)
				break;
			cpu_pause();
		}

		if (self_ipi_hits == before + 1)
			delivered++;
	}

	kputs("UrMach x86-64: interrupts on, sent 3 to myself, ");
	kputdec(delivered);
	kputs(delivered == 3
	      ? " arrived — delivery and acknowledgement both work\r\n"
	      : " arrived — WRONG\r\n");

	trap_set_handler(T_PROBE_VECTOR, 0);
}

/*
 * A message to somebody else, which is the first thing in this kernel that
 * makes another processor act.
 *
 * Until now every processor that arrived went straight to a halt loop and
 * was never heard from again; whether the interrupt path on those
 * processors worked was untested, and would have stayed untested until the
 * first shootdown depended on it.
 *
 * Two things are being checked and they are not the same. That the function
 * ran on every other processor — counted by the processors themselves, one
 * counter each, so a silent one can be named rather than merely missed. And
 * that the shared count came out exact, which is what says the answers were
 * one per processor rather than one processor answering repeatedly.
 *
 * Three rounds, for the same reason the self-interrupt used three: a
 * receiver that never acknowledged would serve the first and no others, and
 * one round cannot tell the difference.
 */
static volatile uint64_t cross_call_marks;

static void cross_call_mark(void *arg)
{
	atomic_inc64((volatile uint64_t *)arg);
}

#define CROSS_CALL_ROUNDS	3

static void ipi_selftest(void)
{
	unsigned others = smp_online_count() - 1;
	unsigned answered = 0;
	uint32_t self = cpu_apic_id();

	if (others == 0) {
		kputs("UrMach x86-64: alone — no processor to cross-call\r\n");
		return;
	}

	for (unsigned round = 0; round < CROSS_CALL_ROUNDS; round++)
		ipi_call_others(cross_call_mark, (void *)&cross_call_marks);

	for (unsigned i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);

		if (c->apic_id == self || !smp_is_online(c->apic_id))
			continue;
		if (ipi_calls_served(c->apic_id) == CROSS_CALL_ROUNDS)
			answered++;
	}

	kputs("UrMach x86-64: cross-called ");
	kputdec(others);
	kputs(" processors ");
	kputdec(CROSS_CALL_ROUNDS);
	kputs(" times, the function ran ");
	kputdec((unsigned)atomic_load64(&cross_call_marks));
	kputs(" times, ");
	kputdec(answered);
	kputs(answered == others
	      && atomic_load64(&cross_call_marks) == (uint64_t)others * CROSS_CALL_ROUNDS
	      ? " served every round\r\n" : " served every round — WRONG\r\n");

	/* Name anybody who did not, since the counters can say who. */
	for (unsigned i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);

		if (c->apic_id == self || !smp_is_online(c->apic_id))
			continue;
		if (ipi_calls_served(c->apic_id) == CROSS_CALL_ROUNDS)
			continue;

		kputs("UrMach x86-64:   cpu ");
		kputdec(c->apic_id);
		kputs(" served ");
		kputdec((unsigned)ipi_calls_served(c->apic_id));
		kputs("\r\n");
	}
}

/*
 * A page ring 3 can actually reach (#411).
 *
 * The permission bit for user access is not a property of the leaf. The
 * processor walks four entries and requires U/S set in *every* one of them —
 * it is a conjunction, unlike the no-execute bit, which is a disjunction: NX
 * anywhere on the path forbids execution everywhere below.  Two bits in the
 * same word, combined in opposite directions.
 *
 * Which means a leaf marked user-accessible, correct in every visible
 * respect, is unreachable if any table above it was built without the bit —
 * and the fault it produces points at the leaf, which is fine.  So the check
 * is the whole path, at every level, the way the processor reads it.
 */
static void user_reachable_selftest(void)
{
	const uint64_t va = 0x0000700000000000ULL;	/* lower half, unused */
	uint64_t frame = boot_frame_alloc();
	unsigned levels_set = 0;
	const pt_entry_t *table;
	unsigned idx[4];
	uint64_t root;
	pmap_t space;

	if (frame == 0) {
		kputs("UrMach x86-64: no frame for the user-reach probe\r\n");
		return;
	}

	/*
	 * In an address space of its own, because the kernel's map has no
	 * user half and pmap_enter() now refuses to pretend otherwise.  That
	 * refusal is the point of the check, so the test has to respect it
	 * rather than work around it.
	 */
	space = pmap_create(0);
	if (space == PMAP_NULL) {
		kputs("UrMach x86-64: no space for the user-reach probe\r\n");
		return;
	}

	root = space->root_pa;

	if (pmap_enter(space, va, frame,
		       VM_PROT_READ | VM_PROT_WRITE, 0) != PMAP_MAP_OK) {
		kputs("UrMach x86-64: could not map the user-reach probe\r\n");
		return;
	}

	idx[0] = pml4_index(va);
	idx[1] = pdpt_index(va);
	idx[2] = pd_index(va);
	idx[3] = pt_index(va);

	table = (const pt_entry_t *)(uintptr_t)phys_to_direct(root);
	for (unsigned lvl = 0; lvl < 4; lvl++) {
		pt_entry_t e = table[idx[lvl]];

		if (!pte_is_valid(e))
			break;
		if (e & INTEL_PTE_USER)
			levels_set++;
		if (pte_is_leaf(e) && lvl < 3)
			break;
		table = (const pt_entry_t *)(uintptr_t)
			phys_to_direct(pte_to_pa(e));
	}

	kputs("UrMach x86-64: a lower-half mapping carries the user bit at ");
	kputdec(levels_set);
	kputs(levels_set == 4
	      ? " of 4 levels — ring 3 can reach it\r\n"
	      : " of 4 levels — WRONG, the walk forbids it\r\n");

	/*
	 * And the rule that says where such a mapping may live at all, asked
	 * both ways round.  pmap_enter()'s answer to a violation is to stop,
	 * so the rule is a predicate as well as a panic — otherwise the only
	 * way to find out whether it works would be to end the boot.
	 */
	kputs("UrMach x86-64: an address space may hold it (");
	kputdec(pmap_may_map(space, va));
	kputs("), the kernel map may not (");
	kputdec(pmap_may_map(pmap_kernel(), va));
	kputs(pmap_may_map(space, va) && !pmap_may_map(pmap_kernel(), va)
	      ? ") — the kernel half stays out of reach\r\n"
	      : ") — WRONG\r\n");

	pmap_remove(space, va, PAGE_SIZE_4K);
	pmap_destroy(space);
	boot_frame_free(frame);
}


/*
 * Two threads, and the registers that have to survive between them (#408).
 *
 * A context switch preserves six registers and no others, because the ABI
 * has already declared the rest dead across a call and a switch is a call
 * with a different stack in the middle.  That is an economy, and an economy
 * is exactly the kind of thing that is right until it is one register short.
 *
 * So the test is not "did both threads run" — that would pass with the
 * saving removed entirely, since the counter lives in memory.  Each thread
 * loads a distinct pattern into all six callee-saved registers, switches
 * away, and reports what *differs* on its return: zero means every one of
 * them came back.  The patterns differ per thread, so a switch that saved
 * to the wrong place hands a thread its neighbour's values rather than its
 * own, and that shows up too.
 *
 * It is written in assembly because there is no way to say this in C: the
 * compiler owns those registers and will not let a function observe them
 * across a call it can see through.
 */
extern uint64_t context_probe(struct context *self, struct context *other,
			      uint64_t pattern, unsigned rounds);
extern uint64_t fpu_probe(struct context *self, struct context *other,
			  uint64_t pattern, unsigned rounds);

static volatile uint64_t thread_xmm[2];

static struct context ctx_main, ctx_a, ctx_b;
static volatile unsigned thread_ran[2];
static volatile uint64_t thread_kept[2];

#define THREAD_ROUNDS	8
#define PATTERN_A	0x1111111100000001ULL
#define PATTERN_B	0x2222222200000002ULL

/*
 * The two run as a relay, and the order they finish in is not incidental.
 *
 * Each round is one switch away and one switch back, so the two threads
 * leave the loop one switch apart: whoever finishes first is still owed a
 * return by the other.  A first attempt had both of them making for the
 * boot context, and the second never ran again — it was suspended inside
 * the switch that would have resumed it, waiting for a partner that had
 * already gone home.
 *
 * So the first to finish hands over to the second, and only the second
 * returns to the boot context.  Neither is ever resumed afterwards, which
 * is what the panics below assert rather than assume.
 */
static void thread_a(void *arg)
{
	(void)arg;
	thread_ran[0] = 1;
	thread_kept[0] = context_probe(&ctx_a, &ctx_b, PATTERN_A, THREAD_ROUNDS);
	thread_xmm[0] = fpu_probe(&ctx_a, &ctx_b, PATTERN_A, THREAD_ROUNDS);

	context_switch(&ctx_a, &ctx_b);		/* let the other one finish */
	panic("thread: resumed a thread that had finished");
}

static void thread_b(void *arg)
{
	(void)arg;
	thread_ran[1] = 1;
	thread_kept[1] = context_probe(&ctx_b, &ctx_a, PATTERN_B, THREAD_ROUNDS);
	thread_xmm[1] = fpu_probe(&ctx_b, &ctx_a, PATTERN_B, THREAD_ROUNDS);

	context_switch(&ctx_b, &ctx_main);	/* and hand the boot back */
	panic("thread: resumed a thread that had finished");
}

static void context_selftest(void)
{
	uint64_t stack_a = boot_frame_alloc();
	uint64_t stack_b = boot_frame_alloc();
	uint64_t fpu_frames = boot_frames_alloc(3);
	uint64_t saved_kernel_rsp = percpu()->kernel_rsp;
	void *fpu_a, *fpu_b, *fpu_main;

	if (stack_a == 0 || stack_b == 0 || fpu_frames == 0) {
		kputs("UrMach x86-64: no memory for the context probe\r\n");
		return;
	}

	stack_a = phys_to_direct(stack_a) + PAGE_SIZE_4K;
	stack_b = phys_to_direct(stack_b) + PAGE_SIZE_4K;

	/*
	 * A page each for the extended state, which is more than it needs and
	 * far simpler than the alternative: the size is the processor's to
	 * decide and a page covers every part that exists.  Page-aligned is
	 * more than the sixty-four bytes the instruction demands.
	 */
	if (fpu_area_size() > PAGE_SIZE_4K)
		panic("thread: the extended state does not fit in a page");

	fpu_a = (void *)(uintptr_t)phys_to_direct(fpu_frames);
	fpu_b = (void *)(uintptr_t)phys_to_direct(fpu_frames + PAGE_SIZE_4K);
	fpu_main = (void *)(uintptr_t)phys_to_direct(fpu_frames + 2 * PAGE_SIZE_4K);

	context_init(&ctx_a, stack_a, thread_a, 0, fpu_a);
	context_init(&ctx_b, stack_b, thread_b, 0, fpu_b);
	fpu_area_init(fpu_main);

	/*
	 * The boot path becomes a thread by being switched away from: its
	 * context is filled in by the switch itself, which is the whole point
	 * — a running thread's saved state is wherever it was interrupted,
	 * and there is nothing to prepare in advance.
	 */
	context_become_current(&ctx_main, saved_kernel_rsp, fpu_main);
	context_switch(&ctx_main, &ctx_a);

	kputs("UrMach x86-64: two threads ran ");
	kputdec(thread_ran[0] + thread_ran[1]);
	kputs(" of 2, register differences ");
	kputhex64(thread_kept[0]);
	kputs(" and ");
	kputhex64(thread_kept[1]);
	kputs(thread_ran[0] && thread_ran[1]
	      && thread_kept[0] == 0 && thread_kept[1] == 0
	      ? " — six registers each, across every switch\r\n"
	      : " — WRONG\r\n");

	kputs("UrMach x86-64: vector state kept across switches, differences ");
	kputhex64(thread_xmm[0]);
	kputs(" and ");
	kputhex64(thread_xmm[1]);
	kputs(", saved by ");
	kputs(fpu_save_instruction());
	kputs(thread_xmm[0] == 0 && thread_xmm[1] == 0
	      ? " — sixteen registers each\r\n" : " — WRONG\r\n");

	kputs("UrMach x86-64: the entry stack followed the thread, and is ");
	kputs(percpu()->kernel_rsp == saved_kernel_rsp
	      ? "back where it started\r\n"
	      : "NOT restored — WRONG\r\n");
}


/*
 * The shape a debugger sees, and the two things it must do (#408).
 *
 * Round-tripping is the easy half: what goes out must come back, or a
 * debugger that reads registers and writes them back has quietly altered
 * the thread it was inspecting.
 *
 * The half that matters is where the round trip is deliberately *not* the
 * identity.  Thread state arrives from whoever holds a port to the thread,
 * so every field is attacker-controlled, and three of them are privilege: a
 * code segment naming ring 0, I/O privilege in the flags, or interrupts
 * disabled.  Each is a way to obtain something the caller does not have, and
 * each is stopped by imposing the field rather than copying it.
 *
 * So the test asks for all three at once — the state a hostile caller would
 * send — and checks the frame that comes out is not the one requested.
 */
static void thread_state_selftest(void)
{
	struct x86_64_thread_state out, back;
	struct x86_64_float_state fstate;
	struct trap_frame frame;
	uint8_t *area, *copy;
	uint64_t frames;
	int regs_ok, refused, float_ok = 1;

	/* Distinct values, so a field copied from its neighbour shows up. */
	for (unsigned i = 0; i < sizeof(frame) / 8; i++)
		((uint64_t *)&frame)[i] = 0x5000000000000000ULL + i;

	frame.cs = USER_CS_RPL3;
	frame.ss = USER_DS_RPL3;
	frame.rflags = RFLAGS_IF | 2;

	thread_state_from_frame(&frame, &out);
	thread_state_to_frame(&out, &frame);
	thread_state_from_frame(&frame, &back);

	regs_ok = out.rax == back.rax && out.rbx == back.rbx
	       && out.rcx == back.rcx && out.rdx == back.rdx
	       && out.rdi == back.rdi && out.rsi == back.rsi
	       && out.rbp == back.rbp && out.rsp == back.rsp
	       && out.r8  == back.r8  && out.r9  == back.r9
	       && out.r10 == back.r10 && out.r11 == back.r11
	       && out.r12 == back.r12 && out.r13 == back.r13
	       && out.r14 == back.r14 && out.r15 == back.r15
	       && out.rip == back.rip;

	kputs("UrMach x86-64: thread state is ");
	kputdec(x86_64_THREAD_STATE_COUNT);
	kputs(" words, float ");
	kputdec(x86_64_FLOAT_STATE_COUNT);
	kputs(", and seventeen registers ");
	kputs(regs_ok ? "round-trip unchanged\r\n" : "DO NOT round-trip — WRONG\r\n");

	/* Now the state a caller would send to gain something. */
	out.cs = KERNEL_CS_SELECTOR;
	out.ss = KERNEL_DS_SELECTOR;
	out.rflags = 0x3000;			/* I/O privilege 3, interrupts off */

	thread_state_to_frame(&out, &frame);

	refused = frame.cs == USER_CS_RPL3
	       && frame.ss == USER_DS_RPL3
	       && (frame.rflags & 0x3000) == 0
	       && (frame.rflags & RFLAGS_IF) != 0;

	kputs("UrMach x86-64: asked for ring 0, iopl 3 and interrupts off, got cs ");
	kputhex64(frame.cs);
	kputs(" rflags ");
	kputhex64(frame.rflags);
	kputs(refused ? " — imposed, not copied\r\n"
			: " — WRONG, the request was honoured\r\n");

	/* And the floating-point image, which must survive a trip through. */
	frames = boot_frames_alloc(2);
	if (frames == 0) {
		kputs("UrMach x86-64: no frames for the float state probe\r\n");
		return;
	}

	area = (uint8_t *)(uintptr_t)phys_to_direct(frames);
	copy = (uint8_t *)(uintptr_t)phys_to_direct(frames + PAGE_SIZE_4K);

	fpu_area_init(area);
	fpu_save(area);
	for (unsigned i = 0; i < 512; i++)
		copy[i] = area[i];

	float_state_from_area(area, &fstate);
	for (unsigned i = 0; i < 512; i++)
		area[i] = 0;
	float_state_to_area(&fstate, area);

	for (unsigned i = 0; i < 512; i++)
		if (area[i] != copy[i])
			float_ok = 0;

	kputs("UrMach x86-64: the floating-point image ");
	kputs(float_ok ? "survives the trip out and back\r\n"
			: "CHANGED — WRONG\r\n");

	boot_frame_free(frames);
	boot_frame_free(frames + PAGE_SIZE_4K);
}

/*
 * What ring 3 carries in %gs while it runs.  Recognisable, and nothing the
 * kernel would ever have there — seeing it afterwards is proof of a swapgs
 * that did not happen.
 */
#define GS_SENTINEL	0x00000000BADC0FFEULL

/*
 * Ring 3, entered for the first time (#411).
 *
 * Everything this kernel has ever run has been privileged, which is why the
 * syscall path cannot yet be tested: a syscall's entire meaning is the
 * privilege change, so issuing one from ring 0 would prove the instruction
 * exists and nothing more.  There has to be somewhere less privileged to
 * come from.
 *
 * Three separate things are being established, and the third is the one that
 * matters:
 *
 *   that user code *ran* — the witness it stores before doing anything else,
 *   because a fault on the very first instruction would look identical from
 *   the kernel's side;
 *
 *   that it had no rights — HLT stops the processor in ring 0 and is a
 *   general protection fault in ring 3, so which of the two happened is the
 *   answer, and it needs no cooperation from the code being tested;
 *
 *   that the processor agrees — the code segment the fault frame carries is
 *   the machine's own record of the ring it interrupted, and its low two
 *   bits are the claim.  Everything else is inference; this is testimony.
 *
 * It runs in an address space of its own, entered by loading its root and
 * left by loading the kernel's back.  The kernel half is shared into every
 * space, so the stack this function is standing on and the code it will
 * return to are mapped throughout — which is the higher-half design paying
 * for itself at the first moment anything depended on it.
 */
static void ring3_selftest(void)
{
	uint64_t kernel_root = read_cr3();
	uint64_t code_frame = boot_frame_alloc();
	uint64_t data_frame = boot_frame_alloc();
	const struct trap_record *t;
	struct trap_record first, bases;
	uint64_t witness, answer, kernel_gs;
	pmap_t space;
	uint64_t size;
	uint8_t *dst;
	const uint8_t *src = (const uint8_t *)__user_probe_start;

	if (code_frame == 0 || data_frame == 0) {
		kputs("UrMach x86-64: no frames for the ring-3 probe\r\n");
		return;
	}

	space = pmap_create(0);
	if (space == PMAP_NULL) {
		kputs("UrMach x86-64: no space for the ring-3 probe\r\n");
		return;
	}

	/*
	 * Executable and read-only; writable and not executable.  W^X applies
	 * to a user program exactly as it does to the kernel, and the first
	 * one ever mapped is a good place to start as we mean to go on.
	 */
	if (pmap_enter(space, USER_PROBE_CODE_VA, code_frame,
		       VM_PROT_READ | VM_PROT_EXECUTE, 0) != PMAP_MAP_OK
	 || pmap_enter(space, USER_PROBE_DATA_VA, data_frame,
		       VM_PROT_READ | VM_PROT_WRITE, 0) != PMAP_MAP_OK) {
		kputs("UrMach x86-64: could not map the ring-3 probe\r\n");
		return;
	}

	/* Copied through the direct map: the space it will run in is not ours. */
	size = (uint64_t)(__user_probe_end - __user_probe_start);
	dst = (uint8_t *)(uintptr_t)phys_to_direct(code_frame);
	for (uint64_t i = 0; i < size; i++)
		dst[i] = src[i];

	*(volatile uint64_t *)(uintptr_t)phys_to_direct(data_frame) = 0;

	/*
	 * The entry arms its own way back, because the stack that return has
	 * to land on only exists inside it.
	 *
	 * The other half of the block pair gets a value of its own first.
	 * Until now both halves have held the same address, which made swapgs
	 * a no-op and its absence undetectable — a missing one looked exactly
	 * like a correct one.  Ring 3 runs carrying the sentinel, so anything
	 * the kernel sees afterwards names which instruction ran.
	 */
	kernel_gs = (uint64_t)(uintptr_t)percpu();
	wrmsr(MSR_KERNEL_GS_BASE, GS_SENTINEL);

	write_cr3(space->root_pa);
	user_probe_enter(USER_PROBE_CODE_VA, USER_PROBE_STACK_TOP);

	/*
	 * Kept before the second visit overwrites it.  One record, one armed
	 * expectation: the machinery is deliberately not a stack, so a test
	 * that wants two answers has to take the first one with it.
	 */
	first = *trap_last();

	/* And back in, at the other entry point in the same page. */
	user_probe_enter(USER_PROBE_FSGSBASE_VA, USER_PROBE_STACK_TOP);
	bases = *trap_last();

	write_cr3(kernel_root);

	wrmsr(MSR_KERNEL_GS_BASE, kernel_gs);

	t = &first;
	witness = *(volatile uint64_t *)(uintptr_t)phys_to_direct(data_frame);
	answer = *(volatile uint64_t *)(uintptr_t)(phys_to_direct(data_frame) + 8);

	kputs("UrMach x86-64: ring 3 ran and left ");
	kputhex64(witness);
	kputs(", then ");
	kputs(trap_name(t->vector));
	kputs(" from cs ");
	kputhex64(t->cs);
	kputs(witness == USER_PROBE_WITNESS && t->caught
	      && t->vector == T_GENERAL_PROTECTION
	      && (t->cs & 3) == USER_RPL
	      ? " — ring 3, on the processor's own testimony\r\n"
	      : " — WRONG\r\n");

	/*
	 * And the syscall it made from there.  The answer names each argument
	 * register separately, so this reports the value rather than a verdict:
	 * a wrong byte says which register the entry path put in the wrong
	 * place, which a pass/fail would not.
	 */
	kputs("UrMach x86-64: its syscall answered ");
	kputhex64(answer);
	kputs(answer == USER_PROBE_SYSCALL_RESULT
	      ? " — six arguments and a return, through SYSCALL and back\r\n"
	      : " — WRONG, expected 0x060504030201\r\n");

	/*
	 * And which block each entry path was reached with.  Ring 3 ran with
	 * the sentinel in %gs, so a path that forgot to swap would have handed
	 * the kernel that value — and kernel code reading %gs:0 would have
	 * been following an address a user program chose.
	 *
	 * Both are checked because they are different code with the same duty:
	 * the syscall entry swaps as its first instruction, the trap stubs
	 * swap only when the saved code segment says the trap came from ring 3.
	 */
	kputs("UrMach x86-64: entered with gs ");
	kputhex64(syscall_probe_gs());
	kputs(" by syscall, ");
	kputhex64(t->gs_base);
	kputs(" by fault, kernel's is ");
	kputhex64(kernel_gs);
	kputs(syscall_probe_gs() == kernel_gs && t->gs_base == kernel_gs
	      ? " — both paths swapped, neither saw the sentinel\r\n"
	      : " — WRONG, a path kept the user's gs\r\n");

	/*
	 * And whether ring 3 could have written that base itself (#440).
	 *
	 * The trap entry for NMI and its three relatives decides what to do by
	 * asking whether the loaded base is a kernel address.  That is a proof
	 * only while a user program cannot write one, which is why CR4.FSGSBASE
	 * is cleared rather than left as the firmware had it.
	 *
	 * Whether the processor even has the feature is reported alongside,
	 * because the two runs prove different things: on a processor without
	 * it the invalid opcode says nothing about the decision, and on one
	 * with it the same invalid opcode is the decision taking effect.  A
	 * verdict that read the same in both cases would be the emulated
	 * default CPU hiding the answer again.
	 */
	kputs("UrMach x86-64: rdgsbase in ring 3 -> ");
	kputs(trap_name(bases.vector));
	kputs(cpu_has_fsgsbase() ? " (the processor has it, cr4 bit "
				 : " (the processor has not, cr4 bit ");
	kputs(read_cr4() & CR4_FSGSBASE ? "set)" : "clear)");
	kputs(bases.caught && bases.vector == T_INVALID_OPCODE
	      && (bases.cs & 3) == USER_RPL
	      && (read_cr4() & CR4_FSGSBASE) == 0
	      ? " — a user program cannot write its own base\r\n"
	      : " — WRONG, ring 3 reached the segment bases\r\n");

	pmap_remove(space, USER_PROBE_CODE_VA, PAGE_SIZE_4K);
	pmap_remove(space, USER_PROBE_DATA_VA, PAGE_SIZE_4K);
	pmap_destroy(space);
	boot_frame_free(code_frame);
	boot_frame_free(data_frame);
}

/*
 * The descriptor table, checked against the arithmetic rather than against
 * its own comments (#411).
 *
 * SYSCALL and SYSRET are handed one number and derive four selectors from it
 * by addition.  That makes the table's order load-bearing in a way an
 * ordinary GDT's is not: there is no field naming the user code segment, only
 * an offset the processor will add.  Put the descriptors in a different order
 * and nothing complains — until the first return to user mode loads whichever
 * descriptor happens to live at the computed offset, which is how a program
 * in ring 3 ends up holding a kernel data segment.
 *
 * So the check does the processor's arithmetic and looks at what it lands on:
 * the right privilege level, the right kind of segment, and for the 64-bit
 * code selector the long-mode bit that makes it 64-bit at all.
 */
static void gdt_layout_selftest(void)
{
	uint64_t kdata = desc_gdt_entry(KERNEL_CS_SELECTOR + 8);
	uint64_t udata = desc_gdt_entry(SYSRET_SELECTOR_BASE + 8);
	uint64_t ucode = desc_gdt_entry(SYSRET_SELECTOR_BASE + 16);

	int kdata_ok = DESC_IS_PRESENT(kdata) && !DESC_IS_CODE(kdata)
		    && DESC_DPL(kdata) == 0;
	int udata_ok = DESC_IS_PRESENT(udata) && !DESC_IS_CODE(udata)
		    && DESC_DPL(udata) == 3;
	int ucode_ok = DESC_IS_PRESENT(ucode) && DESC_IS_CODE(ucode)
		    && DESC_DPL(ucode) == 3 && DESC_IS_LONG(ucode);

	kputs("UrMach x86-64: syscall lands on kernel data (dpl ");
	kputdec(DESC_DPL(kdata));
	kputs("), sysret on user data (dpl ");
	kputdec(DESC_DPL(udata));
	kputs(") and user code (dpl ");
	kputdec(DESC_DPL(ucode));
	kputs(ucode_ok ? ", 64-bit)" : ", NOT 64-bit)");
	kputs(kdata_ok && udata_ok && ucode_ok
	      ? " — the table suits the arithmetic\r\n"
	      : " — WRONG\r\n");
}

/*
 * The message going the other way.
 *
 * Everything above is the boot processor asking and the others answering,
 * which exercises its command register and their handlers — and says nothing
 * about theirs or about its own.  Those are different pieces of hardware and
 * different code paths, and one of them is the one a processor needs in order
 * to ever initiate a shootdown of its own.  Right now nothing schedules work
 * on an application processor, so nothing has ever needed it; the moment #408
 * lands, everything will.
 *
 * The evidence is deliberately not "a counter went up somewhere".  The
 * function the volunteer sends asks the hardware whether the processor
 * running it is the boot processor, so what is being checked is that the
 * message arrived *there* rather than merely that it arrived.
 */
static void ap_to_bsp_selftest(void)
{
	uint32_t self = cpu_apic_id();
	uint64_t before = ipi_calls_served(self);
	unsigned reached;

	if (smp_online_count() < 2) {
		kputs("UrMach x86-64: alone — nobody can call back\r\n");
		return;
	}

	reached = smp_ap_call_probe();

	kputs("UrMach x86-64: a processor called back, boot processor ran it ");
	kputdec(reached);
	kputs(" time, served count ");
	kputdec((unsigned)before);
	kputs(" -> ");
	kputdec((unsigned)ipi_calls_served(self));
	kputs(reached == 1 && ipi_calls_served(self) == before + 1
	      ? " — messages travel both ways\r\n"
	      : " — WRONG\r\n");
}

/*
 * The shootdown, and the reason there has to be one.
 *
 * This is the acceptance criterion #407 has been waiting on, and it is the
 * one thing in the pmap that cannot be demonstrated on a single processor —
 * because the failure it prevents is another processor continuing to use a
 * translation that this one has taken away, and with one processor there is
 * no other processor.
 *
 * So the test is an experiment with a control, in three steps.
 *
 *   1. Map an address to a frame holding a recognisable value, and have
 *      every other processor read it.  That is what puts the translation
 *      into their caches; without this step the rest proves nothing,
 *      because there would be nothing stale to find.
 *
 *   2. Point the entry at a different frame, holding a different value, and
 *      tell nobody.  Read it again everywhere.  A processor that still
 *      reports the old value is using a translation that no longer exists —
 *      the bug, reproduced deliberately.
 *
 *   3. Do it properly, with the shootdown, and read once more.
 *
 * The entry is edited by hand rather than through pmap_enter(), and that is
 * deliberate: the verbs will shortly do the shootdown themselves, and a
 * control that went through them would stop being a control the moment they
 * did.  What step 2 needs is a change that genuinely tells nobody.
 *
 * If step 2 comes back clean, this machine did not keep the stale entry —
 * which is a fact about the machine and not a pass.  Emulators differ here.
 * It is reported as what it is, rather than counted as a success.
 */
#define STALE_WITNESS	0xA1A1A1A1A1A1A1A1ULL
#define FRESH_WITNESS	0xB2B2B2B2B2B2B2B2ULL

struct tlb_probe {
	uint64_t va;
	volatile uint64_t seen[SMP_MAX_CPUS];
};

static void tlb_probe_read(void *arg)
{
	struct tlb_probe *p = arg;

	p->seen[cpu_apic_id()] = *(volatile uint64_t *)(uintptr_t)p->va;
}

/* How many other processors reported `want`, and how many reported anything. */
static unsigned tlb_probe_count(const struct tlb_probe *p, uint64_t want,
				uint32_t self)
{
	unsigned n = 0;

	for (unsigned i = 0; i < acpi_cpu_count(); i++) {
		const struct acpi_cpu *c = acpi_cpu(i);

		if (c->apic_id == self || !smp_is_online(c->apic_id))
			continue;
		if (p->seen[c->apic_id] == want)
			n++;
	}
	return n;
}

static void tlb_shootdown_selftest(void)
{
	static struct tlb_probe probe;
	uint32_t self = cpu_apic_id();
	unsigned others = smp_online_count() - 1;
	uint64_t old_frame, new_frame;
	uint64_t root = read_cr3() & INTEL_PTE_PFN;
	pt_entry_t *entry;
	unsigned stale, fresh;

	if (others == 0) {
		kputs("UrMach x86-64: alone — a shootdown cannot be tested\r\n");
		return;
	}

	old_frame = boot_frame_alloc();
	new_frame = boot_frame_alloc();
	if (old_frame == 0 || new_frame == 0) {
		kputs("UrMach x86-64: no frames for the shootdown test\r\n");
		return;
	}

	*(volatile uint64_t *)(uintptr_t)phys_to_direct(old_frame) = STALE_WITNESS;
	*(volatile uint64_t *)(uintptr_t)phys_to_direct(new_frame) = FRESH_WITNESS;

	/* Somewhere in the heap region that nothing else has claimed. */
	probe.va = KERNEL_HEAP_BASE + 0x200000ULL;

	if (pmap_enter(pmap_kernel(), probe.va, old_frame,
		       VM_PROT_READ | VM_PROT_WRITE, 0) != PMAP_MAP_OK) {
		kputs("UrMach x86-64: could not map the shootdown probe\r\n");
		return;
	}

	/* 1 — everyone walks it, everyone caches it. */
	ipi_call_others(tlb_probe_read, &probe);
	fresh = tlb_probe_count(&probe, STALE_WITNESS, self);

	kputs("UrMach x86-64: shootdown probe mapped, ");
	kputdec(fresh);
	kputs(" of ");
	kputdec(others);
	kputs(fresh == others ? " processors read it\r\n"
			      : " processors read it — WRONG\r\n");

	/* 2 — the control: repoint the entry and tell nobody. */
	entry = pmap_walk(root, probe.va, 0);
	if (entry == PT_ENTRY_NULL) {
		kputs("UrMach x86-64: the probe lost its entry\r\n");
		return;
	}
	*entry = pa_to_pte(new_frame) | (*entry & ~INTEL_PTE_PFN);

	ipi_call_others(tlb_probe_read, &probe);
	stale = tlb_probe_count(&probe, STALE_WITNESS, self);

	kputs("UrMach x86-64: entry repointed silently, ");
	kputdec(stale);
	kputs(" of ");
	kputdec(others);
	kputs(stale > 0
	      ? " still saw the old page — that is the bug, reproduced\r\n"
	      : " still saw the old page — this machine did not keep it, so"
		" the control is inconclusive here\r\n");

	/* 3 — now say so. */
	tlb_flush_range(probe.va, PAGE_SIZE_4K);

	ipi_call_others(tlb_probe_read, &probe);
	fresh = tlb_probe_count(&probe, FRESH_WITNESS, self);

	kputs("UrMach x86-64: after the shootdown, ");
	kputdec(fresh);
	kputs(" of ");
	kputdec(others);
	kputs(fresh == others
	      ? " see the new page — every processor let go of it\r\n"
	      : " see the new page — WRONG\r\n");

	pmap_remove(pmap_kernel(), probe.va, PAGE_SIZE_4K);
}

/*
 * The proof that the interrupt stack table earns its place.
 *
 * Point the stack pointer at unmapped memory and push.  The push faults,
 * and the CPU tries to deliver that page fault — which means pushing a
 * frame, onto the same broken stack, which fails.  A fault while delivering
 * a fault is a double fault, and this is the ordinary way one happens: not
 * two unrelated bugs, but one bug and a stack that cannot carry the news.
 *
 * Without an interrupt stack table the double fault has the same broken
 * stack, fails the same way, and the machine resets with nothing said.
 * With one, it lands on a stack of its own and can still speak.
 *
 * Necessarily last: a double fault is an abort, with no defined state to
 * resume to, so the report is the end of the boot by design.
 */
static void double_fault_selftest(void)
{
	kputs("UrMach x86-64: breaking the stack on purpose — "
	      "expect a double fault, reported from its own stack\r\n");

	__asm__ volatile("movq %0, %%rsp\n\t"
			 "pushq $0"
			 : : "r"(KERNEL_HEAP_BASE + 0x100000ULL) : "memory");

	kputs("UrMach x86-64: THE PUSH SUCCEEDED — the stack was not broken\r\n");
}

/*
 * The descriptor tables, which now belong to a processor rather than to the
 * boot.
 *
 * What used to live here was the boot processor's alone — one table of
 * stacks, loaded once, on the assumption that there would only ever be one
 * processor to load it.  There are up to sixty-four, each of which needs its
 * own, and none of which can take so much as a page fault before it has one.
 * <cpu/desc.h> is where that division of labour is written down.
 */
static void descriptor_tables_init(void)
{
	desc_init_bsp();

	/*
	 * And silence the legacy interrupt controller in the same breath.
	 *
	 * Not later, when something first enables interrupts — here, the
	 * moment a fault can be reported at all.  The firmware leaves the
	 * 8259 enabled and pointing at vectors 0x08-0x0F, so its timer
	 * arrives as vector 8, and vector 8 is the double fault.  Anything
	 * that runs with interrupts on before this — including the first
	 * excursion into ring 3, which enables them by construction — takes
	 * a working timer interrupt and reports it as the fault a kernel
	 * cannot survive.
	 *
	 * That is not hypothetical: it is where this call used to be, and it
	 * cost an afternoon.  The frame said so, for anyone counting words —
	 * five pushed where six were expected, because an interrupt has no
	 * error code and an exception does.
	 */
	pic_disable();

	kputs("UrMach x86-64: GDT + TSS + IDT installed for cpu ");
	kputdec(cpu_apic_id());
	kputs(", faults are now reported\r\n");
}

/* ------------------------------------------------------------------ */
/*  Entry from boot.S (SysV: %edi = multiboot magic, %esi = info ptr).  */
/* ------------------------------------------------------------------ */
void x86_64_boot(uint32_t magic, uint32_t info)
{
	kputs("UrMach x86-64: reached C in long mode (higher-half)\r\n");

	/* multiboot2 hands off magic 0x36d76289 in %eax — captured by boot.S
	 * before the transition, proving the handoff survived into C. */
	kputs("UrMach x86-64: multiboot magic = ");
	kputhex64(magic);
	kputs("\r\n");

	/* Before anything else that could fault: with no IDT a fault is a
	 * triple fault and a silent reset, so every check below runs
	 * unprotected until this is installed. */
	descriptor_tables_init();
	kputs("UrMach x86-64: boot contract #406 (1/6) complete\r\n");

	pte_selftest();
	layout_selftest();
	phys_selftest();
	cpu_selftest();
	memmap_selftest(info);
	direct_map_selftest(info);
	walk_selftest();
	bootmem_selftest(info);
	map_selftest();
	protect_unmap_selftest();
	split_selftest();
	pmap_selftest();
	pmap_verbs_selftest();
	pv_selftest(info);
	phys_ops_selftest();
	phys_copy_selftest();
	wx_selftest();
	user_pmap_selftest();

	acpi_selftest(info);
	atomic_selftest();
	reclaim_selftest();
	percpu_selftest();
	gdt_layout_selftest();
	syscall_init();
	fpu_init();
	context_selftest();
	thread_state_selftest();
	user_reachable_selftest();
	ring3_selftest();
	external_vectors_selftest();
	self_ipi_selftest();
	ipi_init();
	{
		unsigned asked = acpi_usable_cpu_count();
		unsigned up = smp_start_others();

		kputs("UrMach x86-64: woke ");
		kputdec(asked ? asked - 1 : 0);
		kputs(" processors, ");
		kputdec(up);
		kputs(" reported in — ");
		kputdec(smp_online_count());
		kputs(" online\r\n");

		if (asked > 1) {
			kputs("UrMach x86-64:   awake:");
			for (unsigned i = 0; i < acpi_cpu_count(); i++) {
				const struct acpi_cpu *c = acpi_cpu(i);

				if (c->apic_id == lapic_id())
					continue;
				kputs(" ");
				kputdec(c->apic_id);
				if (!smp_is_online(c->apic_id))
					kputs("(silent)");
			}
			kputs("\r\n");
		}
	}

	ipi_selftest();
	ap_to_bsp_selftest();
	tlb_shootdown_selftest();

	wx_enforcement_selftest();
	trap_vectors_selftest();
	double_fault_selftest();

	for (;;)
		__asm__ volatile("cli; hlt");
}
