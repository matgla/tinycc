/* RP2350 RAM-only linker script */

MEMORY
{
    /* SRAM at 0x20000000, first 512KB */
    RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 512K
    
    /* Stack at end of RAM */
    STACK_TOP (rw) : ORIGIN = 0x20080000, LENGTH = 0
}

/* Entry point */
ENTRY(_entry_point)

SECTIONS
{
    /* Text section */
    .text : {
        __text_start__ = .;
        KEEP(*(.vectors))
        KEEP(*(.text._entry_point))
        *(.text*)
        *(.rodata*)
        . = ALIGN(4);
        __text_end__ = .;
    } > RAM
    
    /* Data section */
    .data : {
        __data_start__ = .;
        *(.data*)
        . = ALIGN(4);
        __data_end__ = .;
    } > RAM
    
    /* BSS */
    .bss (NOLOAD) : {
        __bss_start__ = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        __bss_end__ = .;
    } > RAM
    
    __stack_bottom__ = ORIGIN(STACK_TOP);
}
