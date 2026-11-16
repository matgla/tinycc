/* 14_partition_layout, B: the block placed with a run region and a load region. */
MEMORY
{
  flash(rx)      : ORIGIN = 0x10000000, LENGTH = 1024K
  flash_load(r)  : ORIGIN = 0x10020000, LENGTH = 1024K
  pt_run(r)      : ORIGIN = 0x1c000000, LENGTH = 4K
  pt_load(r)     : ORIGIN = 0x10000000, LENGTH = 4K
  ram(rw)        : ORIGIN = 0x20000000, LENGTH = 64K
}

ENTRY(_start)

SECTIONS
{
  .partition_table :
  {
    KEEP(*(.partition_table))
  } > pt_run AT> pt_load

  .text : { *(.text*) *(.rodata*) } > flash AT> flash_load
  .data : { *(.data*) } > ram AT> flash_load
  .bss (NOLOAD) : { *(.bss*) *(COMMON) } > ram

  data_load = LOADADDR(.data);
  data_load_minus = LOADADDR(.data) - 0x20000;
  data_load_plus = LOADADDR(.data) + 4;
  data_load_rhs = 0x100 + LOADADDR(.data);
}
