# -static: an orphan .got after .bss loads at the wrong address

**Status:** open · **Severity:** wrong output (bare-metal ELF images) · **Found:** 2026-11-25 (writing test_segment_sizes_stop_at_file_contents) · **Reported by:** claude-opus-5-5

## Summary

A `-static` link whose linker script does not name `.got` places it, as an
orphan, after `.bss` in the RAM region, and in the same PT_LOAD as `.data` and
`.bss`. `.bss` adds no file bytes, so `.got`'s file offset is `.bss`'s while
its address is `.bss`'s end: the segment's file-to-memory mapping no longer
holds for it. A loader that copies the segment (QEMU, a debugger's `load`,
startup code copying `__data_start..`) puts `.got`'s bytes at `.bss`'s start,
and `.got` itself stays zero.

```
tests/linker/sections/15_segment_sizes.{c,ld}, linked -static:
  LOAD  off 0x26c  vaddr 0x80000000  paddr 0x10000030  filesz 0x1030  memsz 0x11030  RW
  [4] .data  PROGBITS  80000000  off 00026c  size 000008
  [5] .bss   NOBITS    80001000  off 00126c  size 010000  align 4096
  [6] .got   PROGBITS  80011000  off 00126c  size 000030      <- 0x26c + 0x11000 expected
```

Without `-static` the dynamic sections change the flags after `.bss`, `.got`
gets a PT_LOAD of its own, and the layout is consistent (that is how
`tests/ir_tests/qemu/mps2-an505` links, and what the test checks).

## Reproducer

```
bin/armv8m-tcc -ccompat -nostdlib -static -Wl,-oformat=elf32-littlearm \
  -T tests/linker/sections/15_segment_sizes.ld 15_segment_sizes.o -o t.elf
arm-none-eabi-readelf -lSW t.elf
```

## Root cause

`source/obj/tccelf_layout.c` (the section/segment loop of the layout pass)
keeps one PT_LOAD while the section flags and memory region stay the same, and
a NOBITS section does not advance `file_offset`. A section with file contents
after a NOBITS one in the same segment therefore gets a file offset that is
short by the NOBITS size (and any alignment before it).

## Fix directions

- What GNU ld does: an orphan `.got` goes next to `.data` (before `.bss`), by
  the orphan-placement rules for writable PROGBITS.
- Or, when a section with file contents follows a NOBITS section in the same
  PT_LOAD, start a new PT_LOAD for it. Careful: the same loop lays out YAFF
  images, where a new segment re-aligns the section (`align = s_align - 1`),
  which would move addresses in every rootfs binary with that shape.
- Writing the NOBITS bytes out as zeros (GNU ld's fallback) keeps addresses but
  puts `.bss` into the flash image of an `AT>` segment.

## Regression lock

`tests/linker/test_linker.py::test_segment_sizes_stop_at_file_contents` links
without `-static`; drop that comment and link `-static` once this is fixed.
