/* A bare-metal image shaped like tests/ir_tests/qemu/mps2-an505's: a vector
 * table first in flash, .data run in RAM and loaded after the code, and a
 * page-aligned .bss after it (15_segment_sizes.ld).  test_linker.py checks
 * each PT_LOAD's file and memory sizes against its sections.
 */

__attribute__((section(".isr_vector"), used))
const unsigned vectors[4] = {0x20010000u, 0x101u, 0x103u, 0x105u};

int initialized = 42;

/* page-aligned, as an mmap arena would be: the gap before it is no file data */
__attribute__((aligned(4096))) unsigned char arena[64 * 1024];

int _start(void)
{
  arena[100] = (unsigned char)initialized;
  return arena[100] + (int)vectors[1];
}
