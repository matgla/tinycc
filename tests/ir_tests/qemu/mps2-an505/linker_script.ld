MEMORY
{
    FLASH(rwx) : ORIGIN = 0x10000000, LENGTH = 512K
    RAM(rwx) : ORIGIN = 0x20000000, LENGTH = 512K
}

ENTRY (Reset_Handler)

__stack_size__ = 0x2000;  /* 8KB stack */
__heap_size__  = 0x10000; /* 64KB heap */


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

    .data :
    {
        __data_start__ = .;
        *(.data)
        *(.data*)
        __data_end__ = .;
    } > FLASH

    .bss :
    {
        __bss_start__ = .;
        *(.bss)
        *(.bss*)
        *(COMMON)
        __bss_end__ = .;
    } > FLASH

    .heap (NOLOAD) :
    {
        . = ALIGN(8);
        __heap_start__ = .;
        __end__ = .;
        end = .;
        . = . + __heap_size__;
        __heap_end__ = .;
        __HeapLimit = .;
    } > FLASH

    .stack (NOLOAD) :
    {
        . = ALIGN(8);
        __stack_start__ = .;
        . = . + __stack_size__;
        __stack = .;
        __stack_end__ = .;
        __StackTop = .;
    } > FLASH

    __StackLimit = __stack_start__;
}
