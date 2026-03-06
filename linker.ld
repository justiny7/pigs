ENTRY(_start)

SECTIONS
{
    /* Load ARM code at 0x8000 */
    . = 0x8000;

    .text : {
        __code_start__ = .;
        KEEP(*(.text.boot)) /* Make sure startup code isn't optimized away */
        *(.text*)
        . = ALIGN(16);
        __code_end__ = .;
    }

    /* Read-only data */
    . = ALIGN(16);
    .rodata : {
        *(.rodata*)
        . = ALIGN(16);
    }

    /* Initialized data */
    . = ALIGN(16);
    .data : {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(16);
        __data_end__ = .;
    }

    /* Uninitialized data */
    . = ALIGN(16);
    .bss : {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(16);
        __bss_end__ = .;
        _end = .;
    }
}
