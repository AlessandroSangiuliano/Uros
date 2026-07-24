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
