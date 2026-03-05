/*
 * OSFMK Mach Kernel Linker Script
 * Target: i386 (AT386)
 * 
 * This linker script is compatible with multiboot specification.
 * The kernel is loaded at physical address 0x100000 (1MB) but
 * linked at virtual address 0xC0100000 (KERNELBASE + 1MB).
 * 
 * The flat memory model uses segment base=0 for all segments.
 * Kernel/user separation is done entirely via paging:
 *   0x00000000 - 0xBFFFFFFF  user space (PTE U/S=1)
 *   0xC0000000 - 0xFFFFFFFF  kernel space (PTE U/S=0)
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)

/* Physical load address (multiboot loads here) */
PHYS_LOAD = 0x100000;

/* Virtual base for kernel space */
KERNELBASE = 0xC0000000;

/* Virtual address = physical + KERNELBASE */
TEXTORG = KERNELBASE + PHYS_LOAD;

ENTRY(pstart)

SECTIONS
{
    /* Start at virtual address TEXTORG, loaded at PHYS_LOAD */
    . = TEXTORG;

    /* Multiboot header must be in the first 8KB of the kernel image */
    .multiboot ALIGN(4) : AT(ADDR(.multiboot) - KERNELBASE) {
        *(.multiboot)
    }

    /* Code section */
    .text ALIGN(4096) : AT(ADDR(.text) - KERNELBASE) {
        _text_start = .;
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t.*)
        _text_end = .;
    }

    /* Read-only data */
    .rodata ALIGN(4096) : AT(ADDR(.rodata) - KERNELBASE) {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r.*)
        _rodata_end = .;
    }

    /* Data section */
    .data ALIGN(4096) : AT(ADDR(.data) - KERNELBASE) {
        _data_start = .;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d.*)
        _data_end = .;
        edata = .;
    }

    /* BSS section (uninitialized data) */
    .bss ALIGN(4096) : AT(ADDR(.bss) - KERNELBASE) {
        _bss_start = .;
        *(.bss)
        *(.bss.*)
        *(COMMON)
        *(.gnu.linkonce.b.*)
        _bss_end = .;
    }

    /* End of kernel */
    _kernel_end = .;
    end = .;

    /* Stabs debugging sections */
    .stab          0 : { *(.stab) }
    .stabstr       0 : { *(.stabstr) }
    .stab.excl     0 : { *(.stab.excl) }
    .stab.exclstr  0 : { *(.stab.exclstr) }
    .stab.index    0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment       0 : { *(.comment) }

    /* DWARF debugging sections */
    .debug          0 : { *(.debug) }
    .line           0 : { *(.line) }
    .debug_srcinfo  0 : { *(.debug_srcinfo) }
    .debug_sfnames  0 : { *(.debug_sfnames) }
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_macinfo  0 : { *(.debug_macinfo) }

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.note.GNU-stack)
        *(.eh_frame)
        *(.eh_frame_hdr)
    }
}
