/* Shaped like yasboot/link/qemu_mps3_an524.ld: a KEEP()'d vector section
 * ahead of *(.text*) in a flash region, data in RAM. */
MEMORY
{
  flash (rx) : ORIGIN = 0x10000000, LENGTH = 128K
  ram   (rw) : ORIGIN = 0x20000000, LENGTH = 64K
}

ENTRY(_start)

SECTIONS
{
  .text :
  {
    KEEP(*(.vectors))
    *(.text*)
    *(.rodata*)
  } > flash

  .data : { *(.data*) *(.got*) } > ram
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram
}
