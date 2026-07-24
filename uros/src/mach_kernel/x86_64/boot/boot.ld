/*
 * x86-64 boot linker script (#406, MD contract 1/6).
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
 */

OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

PHYS_LOAD    = 0x100000;			/* 1 MiB — where GRUB loads us   */
KERNEL_VBASE = 0xffffffff80000000;		/* higher-half base (-2 GiB)     */

SECTIONS
{
	. = PHYS_LOAD;

	/* ---- low: boot code, tables, stack (VMA == LMA) ---- */

	/* Multiboot2 header must sit in the first 32 KiB of the image. */
	.multiboot2 ALIGN(8) : { *(.multiboot2) }

	.boot ALIGN(16) : { *(.boot) }
	.boot.rodata ALIGN(16) : { *(.boot.rodata) }
	.boot.data ALIGN(16) : { *(.boot.data) }
	.boot.bss ALIGN(4096) : { *(.boot.bss) }

	/* Physical cursor where the high kernel image is loaded, right after
	 * the low boot image — contiguous, no overlap. */
	. = ALIGN(16);
	_kernel_lma = .;

	/* ---- high: kernel image (VMA high, LMA low via AT) ---- */
	. = KERNEL_VBASE + _kernel_lma;

	.text ALIGN(16) : AT(ADDR(.text) - KERNEL_VBASE) {
		*(.text)
		*(.text.*)
	}

	.rodata ALIGN(16) : AT(ADDR(.rodata) - KERNEL_VBASE) {
		*(.rodata)
		*(.rodata.*)
	}

	.data ALIGN(16) : AT(ADDR(.data) - KERNEL_VBASE) {
		*(.data)
		*(.data.*)
	}

	/* 4 KiB, not 16: the pmap's page tables live here, and the hardware
	 * takes a table's frame from bits 51:12 — a misaligned table is read
	 * from the wrong address rather than rejected. */
	.bss ALIGN(4096) (NOLOAD) : AT(ADDR(.bss) - KERNEL_VBASE) {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	}

	/DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
