/*
 * Bug: parse_number 64-bit multiply corruption on ARM32
 *
 * The host compiler (Zig/LLVM) generates incorrect ARM Thumb2 code for
 * the 64-bit multiply in parse_number() (tccpp.c). Specifically, the
 * instruction `n = n * b + t` where n is unsigned long long and b is 10
 * generates:
 *   mul.w r0, r3, r3   (b_low * b_low = 100)
 * instead of:
 *   mul.w r0, r2, r3   (n_high * b_low)
 *
 * This leaves garbage (0x64 = 100 = 10*10) in the upper 32 bits of tokc.i,
 * causing expr_const() to fail with "constant exceeds 32 bit" when parsing
 * array dimensions or other integer constants.
 *
 * The test verifies that arrays with explicit dimensions can be declared
 * and used correctly, which requires parse_number to produce clean 64-bit
 * constant values.
 */
#include <stdio.h>

int main(void)
{
  /* Array declarations exercise parse_number → expr_const path */
  int a[1];
  int b[10];
  int c[100];
  int d[1000];
  char e[256];

  a[0] = 42;
  b[9] = 99;
  c[50] = 123;
  d[999] = 456;
  e[255] = 'Z';

  printf("a[0]=%d\n", a[0]);
  printf("b[9]=%d\n", b[9]);
  printf("c[50]=%d\n", c[50]);
  printf("d[999]=%d\n", d[999]);
  printf("e[255]=%c\n", e[255]);

  /* Also test sizeof with arrays to exercise constant evaluation */
  printf("sizeof(a)=%d\n", (int)sizeof(a));
  printf("sizeof(b)=%d\n", (int)sizeof(b));
  printf("sizeof(c)=%d\n", (int)sizeof(c));

  return 0;
}
