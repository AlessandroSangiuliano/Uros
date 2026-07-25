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
#include <cpu/regs.h>
#include <pmap/bootmem.h>
#include <pmap/direct.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
#include <pmap/pv.h>
#include <pmap/walk.h>

#define COM1 0x3F8

/* ------------------------------------------------------------------ */
/*  Minimal polled serial (COM1) — enough to narrate the bring-up.     */
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
	*(volatile uint32_t *)(uintptr_t)USER_TEST_VA = 0xa11caca0;
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

/* ------------------------------------------------------------------ */
/*  GDT + TSS                                                           */
/* ------------------------------------------------------------------ */
struct gdt_ptr {
	uint16_t limit;
	uint64_t base;
} __attribute__((packed));

/* 64-bit TSS: only RSP0 and the IST slots matter to us here. */
struct tss64 {
	uint32_t reserved0;
	uint64_t rsp0;
	uint64_t rsp1;
	uint64_t rsp2;
	uint64_t reserved1;
	uint64_t ist[7];			/* IST1..IST7 */
	uint64_t reserved2;
	uint16_t reserved3;
	uint16_t iomap_base;
} __attribute__((packed));

/* Five 8-byte slots: null, kernel code, kernel data, then the TSS
 * descriptor which is 16 bytes wide in long mode (spans slots 3 and 4). */
static uint64_t gdt[5];
static struct tss64 tss;

/* Dedicated stacks the TSS points at.  16-byte aligned per the ABI. */
static uint8_t rsp0_stack[4096] __attribute__((aligned(16)));
static uint8_t ist1_stack[4096] __attribute__((aligned(16)));

/* Write a 64-bit available-TSS descriptor into gdt[idx], gdt[idx+1]. */
static void gdt_set_tss(int idx, uint64_t base, uint32_t limit)
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

static void load_gdt(void)
{
	struct gdt_ptr gp = { sizeof(gdt) - 1, (uint64_t)(uintptr_t)gdt };

	__asm__ volatile("lgdt %0" : : "m"(gp) : "memory");

	/* Reload the data segment registers from the new table. */
	__asm__ volatile(
		"movw $0x10, %%ax\n\t"
		"movw %%ax, %%ds\n\t"
		"movw %%ax, %%es\n\t"
		"movw %%ax, %%ss\n\t"
		"movw %%ax, %%fs\n\t"
		"movw %%ax, %%gs\n\t"
		: : : "rax");

	/* Reload CS with a far return to the new 0x08 selector. */
	__asm__ volatile(
		"pushq $0x08\n\t"
		"leaq 1f(%%rip), %%rax\n\t"
		"pushq %%rax\n\t"
		"lretq\n\t"
		"1:\n\t"
		: : : "rax", "memory");
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

	/* Definitive GDT: null, kernel code (L=1), kernel data, TSS. */
	gdt[0] = 0;
	gdt[1] = 0x00209A0000000000ULL;		/* code: L, present, DPL0, R/X */
	gdt[2] = 0x0000920000000000ULL;		/* data: present, DPL0, R/W    */
	gdt_set_tss(3, (uint64_t)(uintptr_t)&tss, sizeof(tss) - 1);

	tss.rsp0 = (uint64_t)(uintptr_t)(rsp0_stack + sizeof(rsp0_stack));
	tss.ist[0] = (uint64_t)(uintptr_t)(ist1_stack + sizeof(ist1_stack));
	tss.iomap_base = sizeof(tss);		/* no I/O permission bitmap */

	load_gdt();
	__asm__ volatile("ltr %w0" : : "r"((uint16_t)0x18));

	kputs("UrMach x86-64: definitive GDT + TSS installed (RSP0 + IST1), TR=0x18\r\n");
	kputs("UrMach x86-64: boot contract #406 (1/6) complete\r\n");

	for (;;)
		__asm__ volatile("cli; hlt");
}
