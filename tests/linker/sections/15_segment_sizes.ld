/* 15_segment_sizes: vector table, code, data loaded after the code, .bss. */
MEMORY
{
  flash(rx) : ORIGIN = 0x10000000, LENGTH = 512K
  ram(rwx)  : ORIGIN = 0x80000000, LENGTH = 1M
}

ENTRY(_start)

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > flash
  .text : { *(.text*) *(.rodata*) } > flash
  .data : { *(.data*) } > ram AT> flash
  .bss : { *(.bss*) *(COMMON) } > ram
}
