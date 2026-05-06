ENTRY(_start)

SECTIONS {
    . = 0x100000;

    .text : {
        *(.multiboot)
        *(.text*)
    }

    .rodata ALIGN(0x1000) : { *(.rodata*) }
    .data   ALIGN(0x1000) : { *(.data*) }

    .bss ALIGN(0x1000) : {
        _bss_start = .;
        *(.bss*)
        *(COMMON)
        _bss_end = .;
    }
}
