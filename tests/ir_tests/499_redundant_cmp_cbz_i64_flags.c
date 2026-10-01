/* ra:redundant_cmp deletes a CMP identical to one just before a branch and
   lets its reader use the flags the first left.  Two ways that first compare
   leaves no such flags:
   - a 32-bit `CMP r, #0` branched on EQ/NE becomes CBZ/CBNZ in codegen, and
     CBZ sets no flags: `if (x != 0) { if (x < 0) ...` compiled to
     `cbz r0; bge`, the bge reading whatever an earlier compare left;
   - a 64-bit CMP is lowered by its reader: `!= 0` is `ORRS t, lo, hi`, whose N
     is the sign of lo | hi, not of the value, and `< 0` then read it.
   sign_cbz is entered with N = Z = C = V = 0 ("greater or equal") set by the
   caller, which is what its bge read; the others have an earlier compare
   leave the same. */
#include <stdint.h>
#include <stdio.h>

__attribute__((noinline)) int sign_cbz(int x, int y) {
  if (x != 0) {
    if (x < 0)
      return y + 1;
    return y * 3;
  }
  return 7;
}

/* Call sign_cbz with known flags: APSR.NZCV all clear. */
int call_with_ge_flags(int x, int y);
__asm__(".text\n"
        ".global call_with_ge_flags\n"
        ".thumb_func\n"
        "call_with_ge_flags:\n"
        "  push {r4, lr}\n"
        "  movs r2, #0\n"
        "  msr APSR_nzcvq, r2\n"
        "  bl sign_cbz\n"
        "  pop {r4, pc}\n");

__attribute__((noinline)) int sign3(int x, int y, int z) {
  int r = 0;
  if (y > z)
    r = 10;
  if (x != 0) {
    if (x < 0)
      return r + 1;
    return r + 3;
  }
  return r + 7;
}

__attribute__((noinline)) int sign3_eq(int x, unsigned y, unsigned z) {
  int r = y >= z ? 20 : 0;
  if (x == 0)
    return r + 7;
  if (x < 0)
    return r + 1;
  return r + 3;
}

__attribute__((noinline)) int sign64(int64_t v, int64_t w) {
  if (v != 0 && (v < 0) != (w < 0))
    return 1;
  return 0;
}

int main(void) {
  volatile int a = -4, b = 9, c = 0, big = 100, small = 1;
  volatile int64_t p = 1243716170416LL, n = -176628268885896625LL, lowbit = 0x80000000LL;
  printf("%d %d %d\n", call_with_ge_flags(a, 1), call_with_ge_flags(b, 1), call_with_ge_flags(c, 1));
  printf("%d %d %d\n", sign3(a, big, small), sign3(b, big, small), sign3(c, big, small));
  printf("%d %d %d\n", sign3(a, small, big), sign3(b, small, big), sign3(c, small, big));
  printf("%d %d %d\n", sign3_eq(a, big, small), sign3_eq(b, big, small), sign3_eq(c, big, small));
  printf("%d %d %d %d\n", sign64(p, p), sign64(lowbit, p), sign64(n, p), sign64(p, n));
  return 0;
}
