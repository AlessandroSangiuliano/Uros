/*
 * x86-64 boot-stub linker script (#406, MD contract 1/6).
 *
 * This increment is the 32-bit boot entry only: paging is off, so the
 * image is a flat elf32 loaded and run at 1 MiB, VMA == LMA.  It is
 * NOT yet the higher-half kernel of docs/uros_design.md ch.11 — the
 * kernel image moves to 0xffffffff80000000 once the long-mode page
 * tables exist to map it there (a later increment).  The output stays
 * elf32 because the multiboot1 loader (qemu -kernel) loads a 32-bit
 * ELF; the eventual 64-bit code will be embedded sections the boot
 * code jumps into after enabling long mode.
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(_start)

PHYS_LOAD = 0x100000;			/* 1 MiB — where multiboot loads us */

SECTIONS
{
	. = PHYS_LOAD;

	/* Multiboot header must sit in the first 8 KiB of the image. */
	.multiboot ALIGN(8) : { *(.multiboot) }

	.boot ALIGN(16) : { *(.boot) }

	.text ALIGN(16) : {
		*(.text)
		*(.text.*)
	}

	.rodata ALIGN(16) : {
		*(.rodata)
		*(.rodata.*)
	}

	.data ALIGN(16) : {
		*(.data)
		*(.data.*)
	}

	.bss ALIGN(16) : {
		*(.bss)
		*(.bss.*)
		*(COMMON)
	}

	/DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
