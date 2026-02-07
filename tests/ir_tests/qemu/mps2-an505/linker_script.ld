MEMORY
{
    FLASH(rwx) : ORIGIN = 0x10000000, LENGTH = 512K
    /* QEMU mps2-an505 maps main SRAM at 0x80000000 (see `info mtree`). */
    RAM(rwx) : ORIGIN = 0x80000000, LENGTH = 16M
}

ENTRY (Reset_Handler)

__stack_size__ = 0x20000;  /* 8KB stack */
__heap_size__  = 0x100000; /* 64KB heap */


SECTIONS
{
    .text :
    {
        KEEP(*(.isr_vector))
        *(.text)
        *(.text*)
        *(.rodata)
        *(.rodata*)
    } > FLASH

    /* Ensure toolchain-provided _init/_fini are complete under --gc-sections.
       (crti.o provides prologue, crtn.o provides epilogue/return). */
    .init :
    {
        KEEP(*(.init))
    } > FLASH

    .fini :
    {
        KEEP(*(.fini))
    } > FLASH

    .preinit_array :
    {
        PROVIDE_HIDDEN(__preinit_array_start = .);
        KEEP(*(.preinit_array*))
        PROVIDE_HIDDEN(__preinit_array_end = .);
    } > FLASH

    .init_array :
    {
        PROVIDE_HIDDEN(__init_array_start = .);
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        PROVIDE_HIDDEN(__init_array_end = .);
    } > FLASH

    .fini_array :
    {
        PROVIDE_HIDDEN(__fini_array_start = .);
        KEEP(*(SORT(.fini_array.*)))
        KEEP(*(.fini_array*))
        PROVIDE_HIDDEN(__fini_array_end = .);
    } > FLASH

    .data :
    {
        __data_start__ = .;
        *(.data)
        *(.data*)
        __data_end__ = .;
    } > RAM AT > FLASH

    __data_load__ = LOADADDR(.data);

    .bss (NOLOAD) :
    {
        __bss_start__ = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        __bss_end__ = .;
    } > RAM

    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        __heap_start__ = .;
        __end__ = .;
        end = .;
        . = . + __heap_size__;
        __heap_end__ = .;
        __HeapLimit = .;
    } > RAM

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        __stack_start__ = .;
        __StackLimit = .;
        . = . + __stack_size__;
        __stack = .;
        __stack_end__ = .;
        __StackTop = .;
    } > RAM
}
