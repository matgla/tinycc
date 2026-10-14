/* An asm "r" input that is a comparison or &&/|| result lived in the flags
 * (VT_CMP / VT_JMP), and parse_asm_operands handed the asm the comparison's
 * left operand register instead of the 0/1 value -- at every -O level.
 * Found by the gen_c.py "hazard" fuzz profile. */
#include <stdio.h>

__attribute__((noinline)) unsigned gt_m(unsigned a, unsigned b)
{
  unsigned t = 0x55u;
  __asm__ volatile("str %1, %0" : "=m"(t) : "r"(a > b));
  return t;
}

__attribute__((noinline)) unsigned gt_r(unsigned a, unsigned b)
{
  unsigned t;
  __asm__ volatile("mov %0, %1" : "=r"(t) : "r"(a > b));
  return t;
}

__attribute__((noinline)) unsigned andand(unsigned a, unsigned b)
{
  unsigned t;
  __asm__ volatile("mov %0, %1" : "=r"(t) : "r"(a && b));
  return t;
}

__attribute__((noinline)) unsigned oror(unsigned a, unsigned b)
{
  unsigned t;
  __asm__ volatile("mov %0, %1" : "=r"(t) : "r"(a || b));
  return t;
}

__attribute__((noinline)) unsigned two(unsigned a, unsigned b)
{
  /* the second operand's code must not clobber the first comparison */
  unsigned t;
  __asm__ volatile("add %0, %1, %2" : "=r"(t) : "r"(a == b), "r"(a * 3u > b));
  return t;
}

int main(void)
{
  printf("%u %u %u %u\n", gt_m(7u, 3u), gt_m(3u, 7u), gt_r(9u, 2u), gt_r(2u, 9u));
  printf("%u %u %u %u\n", andand(5u, 6u), andand(5u, 0u), oror(0u, 0u), oror(0u, 4u));
  printf("%u %u %u\n", two(4u, 4u), two(4u, 13u), two(1u, 2u));
  return 0;
}
