/*
 * OSFMK Mach Kernel Linker Script
 * Target: i386 (AT386)
 * 
 * This linker script is compatible with multiboot specification.
 * The kernel is loaded at physical address 0x100000 (1MB).
 * 
 * Virtual address mapping:
 *   Physical 0x100000 -> Virtual 0x100000 (identity mapped initially)
 *   After paging: maps to LINEAR_KERNEL_ADDRESS (0xc0000000)
 */

OUTPUT_FORMAT("elf32-i386")
OUTPUT_ARCH(i386)
ENTRY(pstart)

/* Kernel physical load address (TEXTORG from config) */
TEXTORG = 0x100000;

SECTIONS
{
    /* Start at 1MB physical address */
    . = TEXTORG;

    /* Multiboot header must be in the first 8KB of the kernel image */
    .multiboot ALIGN(4) : {
        *(.multiboot)
    }

    /* Code section */
    .text ALIGN(4096) : {
        _text_start = .;
        *(.text)
        *(.text.*)
        *(.gnu.linkonce.t.*)
        _text_end = .;
    }

    /* Read-only data */
    .rodata ALIGN(4096) : {
        _rodata_start = .;
        *(.rodata)
        *(.rodata.*)
        *(.gnu.linkonce.r.*)
        _rodata_end = .;
    }

    /* Data section */
    .data ALIGN(4096) : {
        _data_start = .;
        *(.data)
        *(.data.*)
        *(.gnu.linkonce.d.*)
        _data_end = .;
        edata = .;
    }

    /* BSS section (uninitialized data) */
    .bss ALIGN(4096) : {
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
