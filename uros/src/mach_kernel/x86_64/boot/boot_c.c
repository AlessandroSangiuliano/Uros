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
#include <pmap/direct.h>
#include <pmap/layout.h>
#include <pmap/pte.h>

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
