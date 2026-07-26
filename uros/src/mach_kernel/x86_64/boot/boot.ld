/*
 * x86-64 boot linker script (#406/#407, MD contracts 1-2/6).
 *
 * ELF64 image, loaded by GRUB via multiboot2 (qemu's -kernel multiboot1
 * loader rejects ELF64).  GRUB hands off in 32-bit protected mode, paging
 * off, and jumps to the ELF entry point _start — so _start is 32-bit code
 * that must live at a LOW physical address, reachable in 32 bits.
 *
 * Two address worlds in one image (the classic higher-half split):
 *
 *   - the .boot* sections stay LOW (VMA == LMA at 1 MiB): the 32-bit entry,
 *     the long-mode trampoline, the initial page tables and boot stack, the
 *     GDT — everything touched while paging is off or by physical address.
 *
 *   - the kernel sections (.text/.rodata/.data/.bss) are linked HIGH at the
 *     higher-half base KERNEL_VBASE (docs/uros_design.md ch.11), with their
 *     LOAD address kept low via AT(VMA - KERNEL_VBASE).  GRUB loads them by
 *     physical (LMA); the boot code maps KERNEL_VBASE -> phys 0 before the
 *     jump, so the 64-bit code runs at its high virtual address.
 *
 * One segment per permission (#407 W^X): no segment is both writable and
 * executable, which is what the "RWX load segment" warning was about, and
 * the kernel sections are page-aligned and bracketed by symbols so the pmap
 * can reprotect each to its own permission once paging is fully up.  The ELF
 * flags here document intent and silence the loader; the enforcement is the
 * page tables, which are ours to set.
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_LOAD    = 0x100000;			/* 1 MiB — where GRUB loads us   */
KERNEL_VBASE = 0xffffffff80000000;		/* higher-half base (-2 GiB)     */

PHDRS
{
	boot_text	PT_LOAD FLAGS(5);	/* R-X: mb2 hdr, entry, trampoline */
	boot_data	PT_LOAD FLAGS(6);	/* RW-: boot data, tables, stack   */
	ktext		PT_LOAD FLAGS(5);	/* R-X: kernel text                */
	krodata		PT_LOAD FLAGS(4);	/* R--: kernel rodata              */
	kdata		PT_LOAD FLAGS(6);	/* RW-: kernel data + bss          */
}

SECTIONS
{
	. = PHYS_LOAD;

	/* ---- low: boot code, tables, stack (VMA == LMA) ---- */

	/* Multiboot2 header must sit in the first 32 KiB of the image. */
	.multiboot2 ALIGN(8) : { *(.multiboot2) } :boot_text
	.boot ALIGN(16) : { *(.boot) } :boot_text
	.boot.rodata ALIGN(16) : { *(.boot.rodata) } :boot_text

	.boot.data ALIGN(16) : { *(.boot.data) } :boot_data
	.boot.bss ALIGN(4096) : { *(.boot.bss) } :boot_data

	/* Physical cursor where the high kernel image is loaded, right after
	 * the low boot image — contiguous, no overlap. */
	. = ALIGN(16);
	_kernel_lma = .;

	/* ---- high: kernel image (VMA high, LMA low via AT) ---- *
	 * Each section is page-aligned and bracketed, so a page belongs to
	 * exactly one section and W^X can be applied a page at a time. */
	. = KERNEL_VBASE + _kernel_lma;

	. = ALIGN(4096);
	__ktext_start = .;
	.text : AT(ADDR(.text) - KERNEL_VBASE) {
		*(.text)
		*(.text.*)
	} :ktext

	. = ALIGN(4096);
	__ktext_end = .;
	__krodata_start = .;
	.rodata : AT(ADDR(.rodata) - KERNEL_VBASE) {
		*(.rodata)
		*(.rodata.*)
	} :krodata

	/*
	 * The AP trampoline travels in the image and is copied to a page
	 * below 1 MiB before use.  It is never executed from here — read-only
	 * is the right home for something that is only ever a source.
	 */
	.trampoline ALIGN(16) : AT(ADDR(.trampoline) - KERNEL_VBASE) {
		__trampoline_start = .;
		*(.trampoline)
		__trampoline_end = .;
	} :krodata

	/*
	 * The ring-3 probe travels the same way and for the same reason: it is
	 * copied into a page an address space owns before it is executed, and
	 * is never run from here.  Read-only is where a source belongs — and
	 * here it is also the point, since this is the one piece of code in
	 * the image that must never be executable by the kernel.
	 */
	.user_probe ALIGN(16) : AT(ADDR(.user_probe) - KERNEL_VBASE) {
		__user_probe_start = .;
		*(.user_probe)
		__user_probe_end = .;
	} :krodata

	. = ALIGN(4096);
	__krodata_end = .;
	__kdata_start = .;
	.data : AT(ADDR(.data) - KERNEL_VBASE) {
		*(.data)
		*(.data.*)
	} :kdata

	/* .bss is a normal NOBITS section inside the RW load segment, not
	 * NOLOAD: that way the loader zeros it (memsz > filesz), so the C
	 * globals that must start at zero actually do on real hardware — RAM
	 * is not guaranteed zero the way QEMU's is. */
	.bss ALIGN(4096) : AT(ADDR(.bss) - KERNEL_VBASE) {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	} :kdata

	. = ALIGN(4096);
	__kernel_end = .;

	/DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
