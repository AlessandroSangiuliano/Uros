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

#include <cpu/regs.h>
#include <pmap/bootmem.h>
#include <pmap/direct.h>
#include <pmap/layout.h>
#include <pmap/map.h>
#include <pmap/pmap.h>
#include <pmap/pte.h>
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
 * Build the direct map, then prove it by reading the witness a third way:
 * not through the image mapping, not through the low identity map, but at
 * DIRECT_MAP_BASE + its physical address.  If that yields the same word,
 * every physical page is now an addition away.
 */
static void direct_map_selftest(void)
{
	uint64_t pa = kernel_va_to_phys((uint64_t)(uintptr_t)&phys_witness);
	uint32_t through_direct;

	direct_map_init();

	kputs("UrMach x86-64: direct map at ");
	kputhex64(DIRECT_MAP_BASE);
	kputs(", ");
	kputdec(direct_map_covered / (1024 * 1024));
	kputs(" MiB in ");
	kputdec(direct_map_page_size / (1024 * 1024));
	kputs(" MiB pages\r\n");

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
static void bootmem_selftest(void)
{
	uint64_t a = boot_frame_alloc();
	uint64_t b = boot_frame_alloc();
	uint64_t c = boot_frame_alloc();
	const volatile uint64_t *first;
	int clean = 1;

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
	kputdec(boot_frames_used());
	kputs(" of ");
	kputdec(boot_frames_total());
	kputs(" used, ");
	kputs(a && b && c
	      && ((a | b | c) & (PAGE_SIZE_4K - 1)) == 0
	      && a != b && b != c && a != c
	      && clean
	      ? "aligned, distinct and zeroed\r\n"
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
	direct_map_selftest();
	walk_selftest();
	bootmem_selftest();
	map_selftest();
	protect_unmap_selftest();
	split_selftest();
	pmap_selftest();
	pmap_verbs_selftest();
	wx_selftest();

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

	(void)info;
	for (;;)
		__asm__ volatile("cli; hlt");
}
