/* ssa:sccp resolved loads of frame slots / address-taken VARs by walking back
 * to a constant store, treating neither inline asm ("memory" clobber, slot
 * address as input) nor a BLOCK_COPY in a sibling block as a write. */
#include <stdio.h>
#include <string.h>

__attribute__((noinline)) int blk(int c)
{
  char buf[16];
  buf[0] = 'a';
  if (c)
    strcpy(buf, "xyz");
  return buf[0];
}

__attribute__((noinline)) int asm_slot(void)
{
  char buf[16];
  buf[0] = 'a';
  __asm__ volatile("strb %1, [%0]" ::"r"(buf), "r"(120) : "memory");
  return buf[0];
}

__attribute__((noinline)) int asm_var(void)
{
  int v = 5;
  __asm__ volatile("str %1, [%0]" ::"r"(&v), "r"(9) : "memory");
  return v;
}

__attribute__((noinline)) int asm_var_cmp(void)
{
  int v = 5;
  int *p = &v;
  __asm__ volatile("str %1, [%0]" ::"r"(p), "r"(9) : "memory");
  if (v == 5)
    return 1;
  return 2;
}

int main(void)
{
  printf("%d %d %d %d %d\n", blk(1), blk(0), asm_slot(), asm_var(), asm_var_cmp());
  return 0;
}
